// Mod hash contributors: wum.sim.hash and the per-tick digest of every sim mod's environment.
// The digest runs at the tick end, outside mod code, and uses only calls that neither allocate nor run the collector
// (rawget/rawgeti/next/type/to*), so the match VM's state and GC timing are the same with it or without it.
#include "lua/sim/sim_hash.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "core/log.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"
#include "melange/wormsign.h"
#include "wormsign/hash_engine.h"

namespace melange::simcore {
namespace {
using wormsign::Fnv;
using wormsign::FnvV;
using wormsign::kFnvBasis;
constexpr uint8_t kTagCut = 0x20, kTagOver = 0x21, kTagNone = 0x22;

simhash::EnvMode g_mode = simhash::EnvMode::Changed;
bool g_installed = false;
int g_matchObs = 0;

struct KeySlot {
    uint64_t keyHash, valHash;
    uint32_t gen;
    bool used, present;
    simhash::Value key, val;
    char keyText[32];
};
struct KeyMap {
    static constexpr size_t kSlots = 256;
    KeySlot slots[kSlots];
    uint32_t gen = 0, untracked = 0;
    bool baseline = true;
};
struct ModState {
    int simRef = -1;
    int envHandle = 0, hashHandle = 0;
    bool overWarned = false;
    std::unique_ptr<KeyMap> g, s;
    char name[48];
};
std::vector<ModState> g_state;
int g_storageKeyRef = -1;
uint32_t g_match = 0, g_tick = 0;

std::mutex g_changeMu;
constexpr size_t kChanges = 1024;
simhash::EnvChange g_changes[kChanges];
uint64_t g_changeCount = 0;

constexpr uint32_t kBins = 5000;
uint32_t g_hist[kBins];
uint64_t g_digests = 0, g_entriesTotal = 0;
uint32_t g_maxUs10 = 0;
int64_t g_freq = 0;

int64_t Qpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}

struct Walk {
    l5::State* L;
    const l5::Api* a;
    uint32_t entries;
    bool over;
};

uint64_t Scalar(Walk& w, int i, uint64_t h) {
    const int t = w.a->type(w.L, i);
    const uint8_t tag = static_cast<uint8_t>(t);
    switch (t) {
        case l5::kTNil: return FnvV(tag, h);
        case l5::kTBoolean: {
            const uint8_t b = w.a->toboolean(w.L, i) ? 1 : 0;
            return FnvV(b, FnvV(tag, h));
        }
        case l5::kTNumber: {
            const float f = w.a->tonumber(w.L, i);
            uint32_t bits;
            memcpy(&bits, &f, 4);
            return FnvV(bits, FnvV(tag, h));
        }
        case l5::kTString: {
            const char* s = w.a->tostring(w.L, i);
            const uint32_t n = static_cast<uint32_t>(w.a->strlen(w.L, i));
            return Fnv(s, s ? n : 0, FnvV(n, FnvV(tag, h)));
        }
        default: return FnvV(tag, h);
    }
}

void Snap(Walk& w, int i, simhash::Value* v) {
    *v = {};
    const int t = w.a->type(w.L, i);
    v->type = static_cast<uint8_t>(t);
    if (t == l5::kTBoolean) {
        v->bytes[0] = w.a->toboolean(w.L, i) ? 1 : 0;
        v->len = 1;
    } else if (t == l5::kTNumber) {
        const float f = w.a->tonumber(w.L, i);
        memcpy(v->bytes, &f, 4);
        v->len = 4;
    } else if (t == l5::kTString) {
        const char* s = w.a->tostring(w.L, i);
        v->fullLen = static_cast<uint32_t>(w.a->strlen(w.L, i));
        v->len = static_cast<uint8_t>(std::min<uint32_t>(v->fullLen, sizeof v->bytes));
        if (s) memcpy(v->bytes, s, v->len);
    }
}

void PushChange(const simhash::EnvChange& c) {
    std::lock_guard lk(g_changeMu);
    g_changes[g_changeCount++ % kChanges] = c;
}

void Log(const KeySlot& k, char root, uint8_t kind, const simhash::Value& before, uint64_t bh, const char* mod) {
    simhash::EnvChange c{};
    c.match = g_match;
    c.tick = g_tick;
    snprintf(c.mod, sizeof c.mod, "%s", mod);
    c.root = root;
    c.kind = kind;
    c.key = k.key;
    memcpy(c.keyText, k.keyText, sizeof c.keyText);
    c.before = before;
    c.after = kind == 2 ? simhash::Value{} : k.val;
    c.beforeHash = bh;
    c.afterHash = kind == 2 ? 0 : k.valHash;
    PushChange(c);
}

void Track(Walk& w, KeyMap& m, int k, int v, uint64_t kh, uint64_t vh, char root, const char* mod) {
    const size_t mask = KeyMap::kSlots - 1;
    size_t i = static_cast<size_t>(kh) & mask;
    KeySlot* s = nullptr;
    for (size_t n = 0; n < KeyMap::kSlots; ++n, i = (i + 1) & mask) {
        KeySlot& e = m.slots[i];
        if (e.used && e.keyHash == kh) {
            s = &e;
            break;
        }
        if (!e.used) {
            if (n > KeyMap::kSlots * 3 / 4) break;
            s = &e;
            s->used = true;
            s->present = false;
            s->keyHash = kh;
            Snap(w, k, &s->key);
            s->keyText[0] = 0;
            if (s->key.type == l5::kTString) {
                const char* t = w.a->tostring(w.L, k);
                snprintf(s->keyText, sizeof s->keyText, "%s", t ? t : "");
            }
            break;
        }
    }
    if (!s) {
        ++m.untracked;
        return;
    }
    if (s->present && s->gen == m.gen) return;
    const bool was = s->present;
    const simhash::Value before = s->val;
    const uint64_t bh = s->valHash;
    s->present = true;
    s->gen = m.gen;
    s->valHash = vh;
    Snap(w, v, &s->val);
    if (m.baseline) return;
    if (!was)
        Log(*s, root, 1, simhash::Value{}, 0, mod);
    else if (bh != vh)
        Log(*s, root, 0, before, bh, mod);
}

uint64_t Table(Walk& w, int t, int depth, KeyMap* km, char root, const char* mod) {
    const auto& a = *w.a;
    uint64_t sum = 0;
    uint32_t n = 0;
    a.pushnil(w.L);
    while (a.next(w.L, t)) {
        const int v = a.gettop(w.L), k = v - 1;
        if (++w.entries > simhash::kMaxEntries) {
            w.over = true;
            a.settop(w.L, k - 1);
            break;
        }
        const uint64_t kh = Scalar(w, k, kFnvBasis);
        uint64_t vh;
        if (a.type(w.L, v) == l5::kTTable)
            vh = depth > 0 ? Table(w, v, depth - 1, nullptr, 0, nullptr) : FnvV(kTagCut);
        else
            vh = Scalar(w, v, kFnvBasis);
        sum += FnvV(vh, kh);
        ++n;
        if (km) Track(w, *km, k, v, kh, vh, root, mod);
        a.settop(w.L, k);
    }
    return FnvV(n, FnvV(sum, FnvV(static_cast<uint8_t>(l5::kTTable))));
}

void EndWalk(KeyMap& m, char root, const char* mod) {
    for (auto& s : m.slots) {
        if (!s.used || !s.present || s.gen == m.gen) continue;
        s.present = false;
        if (!m.baseline) Log(s, root, 2, s.val, s.valHash, mod);
    }
    m.baseline = false;
}

uint64_t Root(Walk& w, int idx, KeyMap* km, char root, const char* mod) {
    if (w.a->type(w.L, idx) != l5::kTTable) return Scalar(w, idx, FnvV(kTagNone));
    if (km) ++km->gen;
    const uint64_t h = Table(w, idx, simhash::kDepth, km, root, mod);
    if (km && !w.over) EndWalk(*km, root, mod);
    return h;
}

uint64_t Digest(int mod, uint32_t* entries) {
    l5::State* L = g.L;
    const auto& a = l5::A();
    const Mod& m = g.mods[mod];
    ModState& st = g_state[mod];
    const int top = a.gettop(L);
    if (!a.checkstack(L, 4 * (simhash::kDepth + 2) + 8)) return FnvV(kTagOver);
    Walk w{L, &a, 0, false};
    a.rawgeti(L, l5::kRegistry, m.envRef);
    const uint64_t he = Root(w, a.gettop(L), st.g.get(), 'g', st.name);
    uint64_t hs = FnvV(kTagNone);
    if (!w.over && st.simRef >= 0 && g_storageKeyRef >= 0) {
        a.rawgeti(L, l5::kRegistry, st.simRef);
        if (a.type(L, -1) == l5::kTTable) {
            a.rawgeti(L, l5::kRegistry, g_storageKeyRef);
            a.rawget(L, -2);
            hs = Root(w, a.gettop(L), st.s.get(), 's', st.name);
        }
    }
    a.settop(L, top);
    *entries = w.entries;
    if (w.over) return FnvV(kTagOver);
    return FnvV(hs, FnvV(he));
}

bool GuardedDigest(int mod, uint64_t* out, uint32_t* entries) {
    __try {
        *out = Digest(mod, entries);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Usable(int i) {
    return g.L && i >= 0 && i < static_cast<int>(g.mods.size()) && i < static_cast<int>(g_state.size()) &&
           g.mods[i].loaded && g.mods[i].envRef >= 0;
}

void EnvContrib(wormsign::Hasher& h, uint32_t tick, void* user) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(user));
    if (!Usable(i)) {
        h.Val(kTagNone);
        return;
    }
    Mod& m = g.mods[i];
    if ((m.envDirty || g_mode == simhash::EnvMode::Always) && g_depth == 0) {
        g_tick = tick;
        const int top = l5::A().gettop(g.L);
        const int64_t t0 = Qpc();
        uint64_t d = 0;
        uint32_t entries = 0;
        if (!GuardedDigest(i, &d, &entries)) {
            l5::A().settop(g.L, top);
            d = FnvV(kTagOver);
            LOG_ERROR("[sim] %s: the environment digest faulted at tick %u", m.id.c_str(), tick);
        }
        const int64_t dt = Qpc() - t0;
        m.envDigest = d;
        m.envDirty = false;
        const uint32_t us10 = static_cast<uint32_t>(std::clamp<int64_t>(dt * 10000000 / g_freq, 0, 0xffffffff));
        ++g_hist[std::min(us10, kBins - 1)];
        g_maxUs10 = std::max(g_maxUs10, us10);
        ++g_digests;
        g_entriesTotal += entries;
        if (entries > simhash::kMaxEntries && !g_state[i].overWarned) {
            g_state[i].overWarned = true;
            LOG_WARN("[sim] %s: its globals and storage hold over %u values; the digest hashes only that it is over",
                     m.id.c_str(), simhash::kMaxEntries);
        }
    }
    h.Val(m.envDigest);
}

void HashContrib(wormsign::Hasher& h, uint32_t, void* user) {
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(user));
    if (i < 0 || i >= static_cast<int>(g.mods.size())) {
        h.Val(kTagNone);
        return;
    }
    Mod& m = g.mods[i];
    h.Val(m.hashCalls);
    h.Val(m.hashCalls ? m.hashAcc : 0);
    m.hashCalls = 0;
    m.hashAcc = 0;
}

std::string ContribName(const std::string& id, const char* what) {
    std::string s;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
                        c == '_' || c == '-';
        s += ok ? c : '_';
    }
    const size_t room = 63 - 4 - strlen(what) - 1;
    if (s.size() > room) s.resize(room);
    return "mod." + s + "." + what;
}

// `unref`: the references belong to the live match VM (at its end); a VM that was never seen closing is gone.
void DropState(bool unref) {
    for (auto& st : g_state) {
        if (st.envHandle) wormsign::RemoveContributor(st.envHandle);
        if (st.hashHandle) wormsign::RemoveContributor(st.hashHandle);
        if (unref) DropRef(st.simRef);
    }
    g_state.clear();
    if (unref) DropRef(g_storageKeyRef);
    g_storageKeyRef = -1;
}

void OnMatch(bool begin, void*) {
    DropState(!begin);
    if (!begin || !g.L) return;
    const auto& a = l5::A();
    l5::State* L = g.L;
    const int top = a.gettop(L);
    ++g_match;
    {
        std::lock_guard lk(g_changeMu);
        g_changeCount = 0;
    }
    a.pushstring(L, "storage");
    g_storageKeyRef = TakeRef();
    g_state.resize(g.mods.size());
    for (int i = 0; i < static_cast<int>(g.mods.size()); ++i) {
        const Mod& m = g.mods[i];
        ModState& st = g_state[i];
        snprintf(st.name, sizeof st.name, "%s", m.id.c_str());
        if (!m.loaded || m.envRef < 0) continue;
        a.rawgeti(L, l5::kRegistry, m.envRef);
        if (a.getmetatable(L, -1)) {
            a.pushstring(L, "__index");
            a.rawget(L, -2);
            a.pushstring(L, "wum");
            a.rawget(L, -2);
            if (a.type(L, -1) == l5::kTTable) {
                a.pushstring(L, "sim");
                a.rawget(L, -2);
                if (a.type(L, -1) == l5::kTTable)
                    st.simRef = TakeRef();
            }
        }
        a.settop(L, top);
        st.g = std::make_unique<KeyMap>();
        st.s = std::make_unique<KeyMap>();
        if (g_mode != simhash::EnvMode::Off)
            st.envHandle = wormsign::AddContributor(ContribName(m.id, "env").c_str(), &EnvContrib,
                                                    reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        st.hashHandle = wormsign::AddContributor(ContribName(m.id, "hash").c_str(), &HashContrib,
                                                 reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    }
    a.settop(L, top);
}

std::string ValueText(const simhash::Value& v) {
    char b[64];
    switch (v.type) {
        case l5::kTNil: return "nil";
        case l5::kTBoolean: return v.bytes[0] ? "true" : "false";
        case l5::kTNumber: {
            float f;
            memcpy(&f, v.bytes, 4);
            snprintf(b, sizeof b, "%.9g", static_cast<double>(f));
            return b;
        }
        case l5::kTString: {
            std::string s = "\"";
            for (uint8_t i = 0; i < v.len; ++i) {
                const char c = static_cast<char>(v.bytes[i]);
                s += (c >= 0x20 && c < 0x7f && c != '"') ? c : '?';
            }
            if (v.fullLen > v.len) s += "...";
            return s + "\"";
        }
        case l5::kTTable: return "table";
        case l5::kTFunction: return "function";
        default: return "userdata";
    }
}
}  // namespace

int __cdecl LHash(l5::State* L) {
    const auto& a = l5::A();
    const int mod = static_cast<int>(a.tonumber(L, Upvalue(1)));
    const int n = a.gettop(L);
    for (int i = 1; i <= n; ++i) {
        const int t = a.type(L, i);
        if (t != l5::kTNil && t != l5::kTBoolean && t != l5::kTNumber && t != l5::kTString) {
            a.pushstring(L, "wum.sim.hash: expected numbers, strings, booleans or nil");
            a.error(L);
            return 0;
        }
    }
    if (mod < 0 || mod >= static_cast<int>(g.mods.size())) return 0;
    Mod& m = g.mods[mod];
    Walk w{L, &a, 0, false};
    uint64_t h = m.hashCalls ? m.hashAcc : kFnvBasis;
    for (int i = 1; i <= n; ++i) h = Scalar(w, i, h);
    m.hashAcc = FnvV(static_cast<uint32_t>(n), h);
    ++m.hashCalls;
    return 0;
}

void NoteModRuns(int mod) {
    if (mod >= 0 && mod < static_cast<int>(g.mods.size())) {
        g.mods[mod].envDirty = true;
    } else {
        for (auto& m : g.mods) m.envDirty = true;
    }
}
}  // namespace melange::simcore

namespace melange::simhash {
using namespace simcore;

EnvMode ParseMode(const char* s) {
    if (s && _stricmp(s, "off") == 0) return EnvMode::Off;
    if (s && _stricmp(s, "always") == 0) return EnvMode::Always;
    return EnvMode::Changed;
}

const char* ModeName(EnvMode m) { return m == EnvMode::Off ? "off" : m == EnvMode::Always ? "always" : "changed"; }

void Install(EnvMode mode) {
    g_mode = mode;
    if (!g_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
    }
    if (g_installed) return;
    g_installed = true;
    g_matchObs = simbridge::OnMatch(&OnMatch, nullptr);
}

void Uninstall() {
    if (!g_installed) return;
    g_installed = false;
    simbridge::RemoveOnMatch(g_matchObs);
    DropState(g.L != nullptr);
}

size_t EnvChanges(uint32_t fromTick, uint32_t toTick, EnvChange* out, size_t max) {
    std::lock_guard lk(g_changeMu);
    const uint64_t first = g_changeCount > kChanges ? g_changeCount - kChanges : 0;
    size_t n = 0;
    for (uint64_t i = first; i < g_changeCount && n < max; ++i) {
        const EnvChange& c = g_changes[i % kChanges];
        if (c.tick >= fromTick && c.tick <= toTick) out[n++] = c;
    }
    return n;
}

std::string Format(const EnvChange& c) {
    std::string key = c.key.type == l5::kTString ? std::string(c.keyText) : "[" + ValueText(c.key) + "]";
    std::string s = "mod." + std::string(c.mod) + ".env " + (c.root == 's' ? "storage." : "global ") + key;
    if (c.kind == 1) return s + " added: " + ValueText(c.after);
    if (c.kind == 2) return s + " removed (was " + ValueText(c.before) + ")";
    return s + ": " + ValueText(c.before) + " -> " + ValueText(c.after);
}

Cost GetCost() {
    Cost c{};
    c.digests = g_digests;
    c.entries = g_entriesTotal;
    c.maxUs10 = g_maxUs10;
    uint64_t acc = 0;
    bool p50 = false;
    for (uint32_t i = 0; i < kBins && g_digests; ++i) {
        acc += g_hist[i];
        if (!p50 && acc * 2 >= g_digests) c.p50Us10 = i, p50 = true;
        if (acc * 100 >= g_digests * 95) {
            c.p95Us10 = i;
            break;
        }
    }
    return c;
}

void ResetCost() {
    memset(g_hist, 0, sizeof g_hist);
    g_digests = g_entriesTotal = 0;
    g_maxUs10 = 0;
}
}  // namespace melange::simhash
