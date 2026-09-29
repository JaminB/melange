#include "wormsign/contrib.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "melange/jlog.h"
#include "melange/wormsign.h"
#include "wormsign/hash_engine.h"

namespace melange::wormsign {
namespace {
class FnvHasher final : public Hasher {
  public:
    uint64_t h = kFnvBasis;
    void Bytes(const void* p, size_t n) override {
        if (p && n) h = Fnv(p, n, h);
    }
};

struct Slot {
    uint32_t tick;
    uint64_t hash;
    bool computed;
};
struct C {
    int handle;
    char name[64];
    uint64_t nameHash;
    ContribFn fn;
    void* user;
    ContribOptions opt;
    bool dead = false, demoted = false, faulted = false;
    uint32_t demotedAt = 0, faultedAt = 0, lastP95 = 0, winN = 0;
    uint64_t calls = 0;
    uint16_t win[contrib::kDemoteWindow];
    Slot ring[contrib::kRingTicks];
};
struct ModsSlot {
    uint32_t tick;
    uint64_t all, replay;
    bool valid;
};

std::recursive_mutex g_mu;
std::vector<std::unique_ptr<C>> g_list, g_pending;  // g_list in name order
int g_next = 1, g_depth = 0;
ModsSlot g_mods[contrib::kRingTicks];
uint16_t g_sortBuf[contrib::kDemoteWindow];

int64_t RealQpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
int64_t (*g_qpc)() = &RealQpc;
int64_t g_freq = 0;

int64_t Freq() {
    if (!g_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
    }
    return g_freq;
}

bool ValidName(const char* n) {
    if (!n || !*n) return false;
    size_t len = 0;
    for (const char* p = n; *p; ++p, ++len) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        if (!ok || len >= 63) return false;
    }
    return true;
}

void ClearRing(C& c) {
    for (auto& s : c.ring) s = {0xffffffffu, 0, false};
}

bool Call(ContribFn fn, Hasher& h, uint32_t tick, void* user) {
    __try {
        fn(h, tick, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Sample(C& c, int64_t dt, uint32_t tick) {
    const int64_t u = dt * 10000000 / Freq();
    c.win[c.winN++] = static_cast<uint16_t>(std::clamp<int64_t>(u, 0, 65535));
    if (c.winN < contrib::kDemoteWindow) return;
    c.winN = 0;
    memcpy(g_sortBuf, c.win, sizeof g_sortBuf);
    const size_t k = contrib::kDemoteWindow * 95 / 100 - 1;
    std::nth_element(g_sortBuf, g_sortBuf + k, g_sortBuf + contrib::kDemoteWindow);
    c.lastP95 = g_sortBuf[k];
    if (c.demoted || c.opt.demoteEvery <= 1 || c.lastP95 <= contrib::kDemoteP95Us10) return;
    c.demoted = true;
    c.demotedAt = tick;
    LOG_WARN("[wormsign] contributor %s demoted at tick %u: p95 %u.%u us over %u ticks, now hashed every %u ticks",
             c.name, tick, c.lastP95 / 10, c.lastP95 % 10, contrib::kDemoteWindow, c.opt.demoteEvery);
    jlog::Rec("wormsign", jlog::Level::Warn, "contributor demoted")
        .Str("name", c.name).Uint("tick", tick).Uint("p95Us10", c.lastP95).Uint("every", c.opt.demoteEvery);
}

void Merge() {
    std::erase_if(g_list, [](const std::unique_ptr<C>& c) { return c->dead; });
    for (auto& p : g_pending) {
        if (p->dead) continue;
        auto at = std::lower_bound(g_list.begin(), g_list.end(), p->name,
                                   [](const std::unique_ptr<C>& e, const char* n) { return strcmp(e->name, n) < 0; });
        g_list.insert(at, std::move(p));
    }
    g_pending.clear();
}

bool Taken(const char* name) {
    for (auto* v : {&g_list, &g_pending})
        for (auto& c : *v)
            if (!c->dead && strcmp(c->name, name) == 0) return true;
    return false;
}

contrib::Info InfoOf(const C& c) {
    contrib::Info i{};
    memcpy(i.name, c.name, sizeof i.name);
    i.version = c.opt.version;
    i.demoteEvery = c.opt.demoteEvery;
    i.inReplayCompare = c.opt.inReplayCompare;
    i.demoted = c.demoted;
    i.faulted = c.faulted;
    i.demotedAt = c.demotedAt;
    i.faultedAt = c.faultedAt;
    i.lastP95Us10 = c.lastP95;
    i.calls = c.calls;
    return i;
}

void JsonStr(std::string& s, const char* v) {
    s += '"';
    for (const char* p = v; *p; ++p) {
        if (*p == '"' || *p == '\\') s += '\\';
        s += *p;
    }
    s += '"';
}
}  // namespace

int AddContributor(const char* name, ContribFn fn, void* user, const ContribOptions& opt) {
    if (!fn || !ValidName(name)) {
        LOG_WARN("[wormsign] contributor '%s' refused: the name must be 1-63 of A-Z a-z 0-9 . _ -", name ? name : "");
        return 0;
    }
    std::lock_guard lk(g_mu);
    if (Taken(name)) {
        LOG_WARN("[wormsign] contributor '%s' refused: the name is taken", name);
        return 0;
    }
    auto c = std::make_unique<C>();
    c->handle = g_next++;
    strcpy_s(c->name, name);
    c->nameHash = Fnv(name, strlen(name));
    c->fn = fn;
    c->user = user;
    c->opt = opt;
    if (!c->opt.demoteEvery) c->opt.demoteEvery = 1;
    ClearRing(*c);
    const int h = c->handle;
    g_pending.push_back(std::move(c));
    if (!g_depth) Merge();
    jlog::Rec("wormsign", jlog::Level::Debug, "contributor added").Str("name", name).Uint("version", opt.version);
    return h;
}

void RemoveContributor(int handle) {
    std::lock_guard lk(g_mu);
    for (auto* v : {&g_list, &g_pending})
        for (auto& c : *v)
            if (c->handle == handle) c->dead = true;
    if (!g_depth) Merge();
}

namespace contrib {
uint64_t HashTick(uint32_t tick) {
    std::lock_guard lk(g_mu);
    ++g_depth;
    bool any = false;
    uint64_t all = kFnvBasis, rep = kFnvBasis;
    for (size_t i = 0; i < g_list.size(); ++i) {
        C& c = *g_list[i];
        if (c.dead) continue;
        any = true;
        Slot& s = c.ring[tick % kRingTicks];
        s = {tick, 0, false};
        if (c.faulted || (c.demoted && tick % c.opt.demoteEvery)) continue;
        FnvHasher h;
        const int64_t t0 = g_qpc();
        const bool ok = Call(c.fn, h, tick, c.user);
        const int64_t dt = g_qpc() - t0;
        if (!ok) {
            c.faulted = true;
            c.faultedAt = tick;
            LOG_ERROR("[wormsign] contributor %s faulted at tick %u and is switched off", c.name, tick);
            jlog::Rec("wormsign", jlog::Level::Error, "contributor faulted").Str("name", c.name).Uint("tick", tick);
            continue;
        }
        ++c.calls;
        s.hash = h.h;
        s.computed = true;
        Sample(c, dt, tick);
        all = FnvV(h.h, FnvV(c.nameHash, all));
        if (c.opt.inReplayCompare) rep = FnvV(h.h, FnvV(c.nameHash, rep));
    }
    if (!--g_depth) Merge();
    if (!any) all = rep = 0;
    g_mods[tick % kRingTicks] = {tick, all, rep, true};
    return all;
}

size_t List(Info* out, size_t max) {
    std::lock_guard lk(g_mu);
    size_t n = 0;
    for (auto& c : g_list)
        if (!c->dead && n < max) out[n++] = InfoOf(*c);
    return n;
}

size_t Count() {
    std::lock_guard lk(g_mu);
    size_t n = 0;
    for (auto& c : g_list) n += !c->dead;
    return n;
}

uint64_t ListHash() {
    std::lock_guard lk(g_mu);
    uint64_t h = kFnvBasis;
    for (auto& c : g_list) {
        if (c->dead) continue;
        h = Fnv(c->name, strlen(c->name) + 1, h);
        h = FnvV(c->opt.version, h);
    }
    return h;
}

std::string Describe(const Info& i) {
    char b[160];
    if (i.faulted)
        snprintf(b, sizeof b, "faulted at tick %u, not hashed since", i.faultedAt);
    else if (i.demoted)
        snprintf(b, sizeof b, "hashed every %u ticks (demoted at tick %u, p95 %u.%u us)", i.demoteEvery, i.demotedAt,
                 i.lastP95Us10 / 10, i.lastP95Us10 % 10);
    else
        snprintf(b, sizeof b, "hashed every tick");
    return b;
}

std::string NoteJson() {
    Info buf[128];
    const size_t n = List(buf, 128);
    std::string s = "{\"contributors\":[";
    for (size_t k = 0; k < n; ++k) {
        const Info& i = buf[k];
        if (k) s += ",";
        s += "{\"name\":";
        JsonStr(s, i.name);
        s += ",\"version\":" + std::to_string(i.version) + ",\"every\":" +
             std::to_string(i.demoted ? i.demoteEvery : 1) + ",\"replay\":" + (i.inReplayCompare ? "true" : "false") +
             ",\"demoted\":" + (i.demoted ? "true" : "false") + ",\"faulted\":" + (i.faulted ? "true" : "false") +
             ",\"p95Us10\":" + std::to_string(i.lastP95Us10) + ",\"text\":";
        JsonStr(s, Describe(i).c_str());
        s += "}";
    }
    s += "]}";
    return s;
}

size_t HashesAt(uint32_t tick, Entry* out, size_t max) {
    std::lock_guard lk(g_mu);
    const ModsSlot& m = g_mods[tick % kRingTicks];
    if (!m.valid || m.tick != tick) return 0;
    size_t n = 0;
    for (auto& c : g_list) {
        if (c->dead || n >= max) continue;
        const Slot& s = c->ring[tick % kRingTicks];
        Entry& e = out[n++];
        memcpy(e.name, c->name, sizeof e.name);
        e.computed = s.tick == tick && s.computed;
        e.hash = e.computed ? s.hash : 0;
    }
    return n;
}

bool ModsAt(uint32_t tick, uint64_t* all, uint64_t* replay) {
    std::lock_guard lk(g_mu);
    const ModsSlot& m = g_mods[tick % kRingTicks];
    if (!m.valid || m.tick != tick) return false;
    if (all) *all = m.all;
    if (replay) *replay = m.replay;
    return true;
}

void ResetSession() {
    std::lock_guard lk(g_mu);
    for (auto& m : g_mods) m.valid = false;
    for (auto* v : {&g_list, &g_pending})
        for (auto& c : *v) ClearRing(*c);
}

void SetClockForTest(int64_t (*qpc)(), int64_t freq) {
    std::lock_guard lk(g_mu);
    g_qpc = qpc ? qpc : &RealQpc;
    g_freq = qpc ? freq : 0;
}
}  // namespace contrib
}  // namespace melange::wormsign
