// Event bus (component B): hooks on the engine's message Post (0x6910e4) and Deliver (0x68cb82), the subscriber
// table, stats, and the EventBus module with its Automation test verbs. Public API: src/sdk/wumfix/bus.h.
// Spec: docs/m0-design.md §1.2 and §3 "B: event bus".
//
// Hot path (every Post/Deliver, ~4000/s in a match): read the u16 id, bump counters, test a 64K-bit "has
// subscribers" bitmap plus the SubscribeAll count, forward. No lock, no allocation, no SEH frame.
// Slow path (someone listens): build a MessageView and call each handler under an SEH guard.
//
// Subscribers live in an immutable Table that is replaced copy-on-write under a mutex and published with an
// atomic pointer swap, so Subscribe/Unsubscribe work from any thread and from inside a handler. A replaced
// table is freed only when no dispatch is reading any table (g_readers == 0); otherwise it waits for the
// next rebuild or the per-frame Tick.
//
// MESSAGE LIFETIME: the engine's message arena (0x691705) is reset when its dispatch depth returns to 0, so a
// MessageView (and m.raw) is valid only during the callback. Copy anything you keep.
#include <windows.h>

#include <intrin.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/bus_internal.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "wumfix/testcmd.h"

namespace wf::bus {
namespace {
constexpr int kPaths = 2;
constexpr uint32_t kMaxFaults = 3;
constexpr uint16_t kMaxPostStack = 32;
constexpr uint64_t kSampleMask = 15;  // time one hook call in 16 for Stats::avgHookUs

int PathIndex(Path p) { return p == Path::Deliver ? 1 : 0; }
const char* PathName(Path p) { return p == Path::Deliver ? "deliver" : "post"; }

struct Sub {
    SubId id = 0;
    Path path = Path::Post;
    bool all = false;
    std::atomic<MsgId> msg{kInvalidId};
    std::string pendingName;  // SubscribeName: the name to (re)resolve while msg == kInvalidId
    bool warnedUnresolved = false;
    Handler fn = nullptr;
    void* user = nullptr;
    std::atomic<bool> live{true};  // false once unsubscribed or disabled after kMaxFaults
    std::atomic<uint32_t> faults{0};
};

struct Table {
    std::vector<std::shared_ptr<Sub>> keep;           // owns the subs this table points at
    std::vector<std::pair<MsgId, Sub*>> byId[kPaths];  // sorted by id (subscription order within an id)
    std::vector<Sub*> all[kPaths];
};

// ---- subscriber state (writers hold g_mu)
std::mutex g_mu;
std::vector<std::shared_ptr<Sub>> g_subs;
SubId g_nextId = 1;
std::vector<Table*> g_retired;
std::atomic<Table*> g_table{nullptr};
std::atomic<int> g_readers{0};
std::atomic<uint32_t> g_pending{0};

// ---- hot-path state (written on the main thread only; racy reads from other threads are acceptable for stats)
std::atomic<uint32_t> g_bits[kPaths][0x10000 / 32];
std::atomic<uint32_t> g_allCount[kPaths];
std::atomic<uint32_t> g_count[kPaths][0x10000];
uint64_t g_seq = 0;
uint64_t g_calls[kPaths] = {};
uint64_t g_handlerCalls = 0;
uint64_t g_handlerFaults = 0;
uint64_t g_sampleCycles = 0;
uint64_t g_samples = 0;
std::atomic<bool> g_installed{false};

// TSC calibration for avgHookUs: the rate is measured over the whole run (start stamp taken at load).
struct Calibration {
    LARGE_INTEGER qpc0{}, freq{};
    uint64_t tsc0 = 0;
    Calibration() {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&qpc0);
        tsc0 = __rdtsc();
    }
    double CyclesPerUs() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        uint64_t tsc = __rdtsc();
        double us = static_cast<double>(now.QuadPart - qpc0.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
        if (us < 1000.0) {  // too early to measure; spin a little for a usable estimate
            Sleep(2);
            QueryPerformanceCounter(&now);
            tsc = __rdtsc();
            us = static_cast<double>(now.QuadPart - qpc0.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart);
        }
        return us > 0 ? static_cast<double>(tsc - tsc0) / us : 0.0;
    }
};
const Calibration g_cal;

// ---- per-thread hook state: nesting depth and the stack of messages currently inside Post (fromPost)
struct Tls {
    uint16_t depth;
    uint16_t postN;  // may exceed kMaxPostStack; only the first kMaxPostStack entries are stored
    const void* post[kMaxPostStack];
};
thread_local Tls t_tls;

bool OnPostStack(const Tls& t, const void* msg) {
    const uint16_t n = std::min(t.postN, kMaxPostStack);
    for (uint16_t i = 0; i < n; ++i)
        if (t.post[i] == msg) return true;
    return false;
}

// ---- table maintenance (g_mu held)
void FreeRetiredLocked() {
    if (g_retired.empty() || g_readers.load(std::memory_order_seq_cst) != 0) return;
    for (Table* t : g_retired) delete t;
    g_retired.clear();
}

void RebuildLocked() {
    auto* nt = new Table;
    uint32_t pending = 0;
    for (const auto& s : g_subs) {
        if (!s->live.load()) continue;
        const int p = PathIndex(s->path);
        if (s->all) {
            nt->all[p].push_back(s.get());
        } else if (MsgId id = s->msg.load(); id != kInvalidId) {
            nt->byId[p].push_back({id, s.get()});
        } else {
            ++pending;
            continue;
        }
        nt->keep.push_back(s);
    }
    for (auto& v : nt->byId)
        std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    static uint32_t words[kPaths][0x10000 / 32];  // scratch, guarded by g_mu
    memset(words, 0, sizeof(words));
    for (int p = 0; p < kPaths; ++p)
        for (const auto& [id, s] : nt->byId[p]) words[p][id >> 5] |= 1u << (id & 31);

    Table* old = g_table.exchange(nt, std::memory_order_seq_cst);
    for (int p = 0; p < kPaths; ++p) {
        for (size_t w = 0; w < 0x10000 / 32; ++w)
            if (g_bits[p][w].load(std::memory_order_relaxed) != words[p][w])
                g_bits[p][w].store(words[p][w], std::memory_order_relaxed);
        g_allCount[p].store(static_cast<uint32_t>(nt->all[p].size()), std::memory_order_relaxed);
    }
    g_pending.store(pending);
    if (old) g_retired.push_back(old);
    FreeRetiredLocked();
}

bool ResolvePendingLocked() {
    if (!g_pending.load() || !RegistryReady()) return false;
    bool changed = false;
    for (const auto& s : g_subs) {
        if (s->all || !s->live.load() || s->msg.load() != kInvalidId || s->pendingName.empty()) continue;
        MsgId id = IdOf(s->pendingName);
        if (id != kInvalidId) {
            s->msg.store(id);
            changed = true;
            WF_INFO("[bus] SubscribeName('%s') resolved to id %04x (sub %u)", s->pendingName.c_str(), id, s->id);
        } else if (!s->warnedUnresolved) {
            s->warnedUnresolved = true;
            WF_WARN("[bus] SubscribeName('%s'): name not in the registry yet (sub %u); still waiting for it",
                    s->pendingName.c_str(), s->id);
        }
    }
    if (changed) RebuildLocked();
    return changed;
}

SubId Add(std::shared_ptr<Sub> s) {
    std::lock_guard lk(g_mu);
    s->id = g_nextId++;
    if (g_nextId == 0) g_nextId = 1;
    g_subs.push_back(s);
    RebuildLocked();
    return s->id;
}

// ---- dispatch
bool CallGuarded(Handler fn, const MessageView& m, void* user, DWORD* code, uintptr_t* addr) {
    __try {
        fn(m, user);
        return true;
    } __except (*code = GetExceptionInformation()->ExceptionRecord->ExceptionCode,
                *addr = reinterpret_cast<uintptr_t>(GetExceptionInformation()->ExceptionRecord->ExceptionAddress),
                EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void OnFault(Sub* s, const MessageView& m, DWORD code, uintptr_t addr) {
    ++g_handlerFaults;
    const uint32_t n = s->faults.fetch_add(1) + 1;
    if (n <= kMaxFaults)
        WF_WARN("[bus] handler %p (sub %u) faulted on %s (%s): exception %08lx at %s [%u/%u]",
                reinterpret_cast<void*>(s->fn), s->id, m.name, PathName(m.path), code,
                game::DescribeAddress(addr).c_str(), n, kMaxFaults);
    if (n == kMaxFaults) {
        s->live.store(false);
        WF_WARN("[bus] handler %p (sub %u) DISABLED after %u faults", reinterpret_cast<void*>(s->fn), s->id, kMaxFaults);
        std::lock_guard lk(g_mu);
        RebuildLocked();  // drop it from the table; the table we are iterating stays alive (g_readers > 0)
    }
}

void Call(Sub* s, const MessageView& m) {
    if (!s->live.load(std::memory_order_relaxed)) return;
    ++g_handlerCalls;
    DWORD code = 0;
    uintptr_t addr = 0;
    if (!CallGuarded(s->fn, m, s->user, &code, &addr)) OnFault(s, m, code, addr);
}

__declspec(noinline) void Slow(Path path, const uint8_t* raw, MsgId id, int32_t handle, uintptr_t caller,
                               const Tls& t, uint64_t seq) {
    g_readers.fetch_add(1, std::memory_order_seq_cst);
    if (const Table* tb = g_table.load(std::memory_order_seq_cst)) {
        MessageView m{};
        m.raw = raw;
        m.id = id;
        m.size = detail::ObjectSize(raw);
        uint32_t vt = 0;
        mem::SafeRead(reinterpret_cast<uintptr_t>(raw), &vt, sizeof(vt));
        m.vtable = vt;
        m.name = NameOf(id);
        m.className = detail::ClassNameOf(vt);
        m.path = path;
        m.handle = path == Path::Post ? detail::PostTarget() : handle;
        m.fromPost = path == Path::Deliver && OnPostStack(t, raw);
        m.depth = t.depth;
        m.caller = caller;
        m.seq = seq;
        m.frame = events::FrameCount();

        const int p = PathIndex(path);
        const auto& v = tb->byId[p];
        auto it = std::lower_bound(v.begin(), v.end(), id, [](const auto& e, MsgId x) { return e.first < x; });
        for (; it != v.end() && it->first == id; ++it) Call(it->second, m);
        for (Sub* s : tb->all[p]) Call(s, m);
    }
    g_readers.fetch_sub(1, std::memory_order_seq_cst);
}

inline void Observe(Path path, const uint8_t* raw, int32_t handle, uintptr_t caller, const Tls& t) {
    const MsgId id = *reinterpret_cast<const uint16_t*>(raw + 4);
    const uint64_t seq = ++g_seq;
    const int p = PathIndex(path);
    auto& c = g_count[p][id];
    c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    ++g_calls[p];
    if (!(g_bits[p][id >> 5].load(std::memory_order_relaxed) & (1u << (id & 31))) &&
        g_allCount[p].load(std::memory_order_relaxed) == 0)
        return;
    Slow(path, raw, id, handle, caller, t, seq);
}

void AddSample(uint64_t cycles) {
    g_sampleCycles += cycles;
    ++g_samples;
}
}  // namespace

// ================================================================ internal seams
namespace detail {
uint32_t ObjectSize(const uint8_t* raw) {
    // Arena header at raw-4 = align4(object size) + 4 (allocator 0x691705). An object is at least vtable + id.
    uint32_t h = 0;
    if (!raw || !mem::SafeRead(reinterpret_cast<uintptr_t>(raw) - 4, &h, sizeof(h))) return 0;
    if (h < 12 || h > 0x4000 || (h & 3)) return 0;
    return h - 4;
}

int RunPost(void* msg, uintptr_t caller, PostForward forward, void* ctx) {
    Tls& t = t_tls;
    const bool sample = (g_seq & kSampleMask) == 0;
    const uint64_t c0 = sample ? __rdtsc() : 0;
    ++t.depth;
    if (msg) Observe(Path::Post, static_cast<const uint8_t*>(msg), -1, caller, t);
    if (t.postN < kMaxPostStack) t.post[t.postN] = msg;
    ++t.postN;
    if (sample) AddSample(__rdtsc() - c0);
    const int r = forward(msg, ctx);
    --t.postN;
    --t.depth;
    return r;
}

int RunDeliver(void* table, void* msg, int handle, char bcast, uintptr_t caller, DeliverForward forward, void* ctx) {
    Tls& t = t_tls;
    const bool sample = (g_seq & kSampleMask) == 0;
    const uint64_t c0 = sample ? __rdtsc() : 0;
    ++t.depth;
    if (msg) Observe(Path::Deliver, static_cast<const uint8_t*>(msg), handle, caller, t);
    if (sample) AddSample(__rdtsc() - c0);
    const int r = forward(table, msg, handle, bcast, ctx);
    --t.depth;
    return r;
}

void Tick() {
    std::lock_guard lk(g_mu);
    ResolvePendingLocked();
    FreeRetiredLocked();
}

size_t PendingNames() { return g_pending.load(); }
}  // namespace detail

// ================================================================ public API
bool MessageView::Read(uint32_t offset, void* out, uint32_t n) const {
    if (!raw || !out || offset > size || n > size - offset) return false;
    if (n == 0) return true;
    return mem::SafeRead(reinterpret_cast<uintptr_t>(raw) + offset, out, n);
}

SubId Subscribe(MsgId id, Path path, Handler fn, void* user) {
    if (!fn || id == kInvalidId || (path != Path::Post && path != Path::Deliver)) return 0;
    auto s = std::make_shared<Sub>();
    s->path = path;
    s->msg = id;
    s->fn = fn;
    s->user = user;
    return Add(std::move(s));
}

SubId SubscribeName(const char* name, Path path, Handler fn, void* user) {
    if (!fn || !name || !*name || (path != Path::Post && path != Path::Deliver)) return 0;
    auto s = std::make_shared<Sub>();
    s->path = path;
    s->pendingName = name;
    s->fn = fn;
    s->user = user;
    // Resolve now if we can; otherwise the per-frame Tick keeps trying (registry not ready, or a name that a
    // later registration adds).
    s->msg = IdOf(name);
    return Add(std::move(s));
}

SubId SubscribeAll(Path path, Handler fn, void* user) {
    if (!fn || (path != Path::Post && path != Path::Deliver)) return 0;
    auto s = std::make_shared<Sub>();
    s->path = path;
    s->all = true;
    s->fn = fn;
    s->user = user;
    return Add(std::move(s));
}

void Unsubscribe(SubId id) {
    if (!id) return;
    std::lock_guard lk(g_mu);
    auto it = std::find_if(g_subs.begin(), g_subs.end(), [id](const auto& s) { return s->id == id; });
    if (it == g_subs.end()) return;
    (*it)->live.store(false);
    g_subs.erase(it);
    RebuildLocked();
}

Stats GetStats() {
    Stats s{};
    s.posts = g_calls[0];
    s.deliveries = g_calls[1];
    s.handlerCalls = g_handlerCalls;
    s.handlerFaults = g_handlerFaults;
    const uint64_t n = g_samples, cyc = g_sampleCycles;
    const double perUs = g_cal.CyclesPerUs();
    s.avgHookUs = n && perUs > 0 ? static_cast<double>(cyc) / static_cast<double>(n) / perUs : 0.0;
    return s;
}

uint32_t CountOf(MsgId id, Path path) { return g_count[PathIndex(path)][id].load(std::memory_order_relaxed); }

bool Installed() { return g_installed.load(); }

// ================================================================ module, hooks and test verbs
namespace {
constexpr uintptr_t kPost = 0x6910e4;     // int __cdecl Post(Message*)
constexpr uintptr_t kDeliver = 0x68cb82;  // int __thiscall EntityTable::Deliver(Message*, int handle, char bcast)
SafetyHookInline g_postHook;
SafetyHookInline g_deliverHook;

int FwdPost(void* msg, void*) { return g_postHook.ccall<int>(msg); }
int FwdDeliver(void* table, void* msg, int handle, char bcast, void*) {
    return g_deliverHook.thiscall<int>(table, msg, handle, bcast);
}
int __cdecl HookPost(void* msg) {
    return detail::RunPost(msg, reinterpret_cast<uintptr_t>(_ReturnAddress()), &FwdPost, nullptr);
}
int __fastcall HookDeliver(void* table, void* /*edx*/, void* msg, int handle, char bcast) {
    return detail::RunDeliver(table, msg, handle, bcast, reinterpret_cast<uintptr_t>(_ReturnAddress()), &FwdDeliver,
                              nullptr);
}

// ---- JsonOut into a flat text line (test verbs only)
class TextOut final : public JsonOut {
public:
    std::string text;
    bool nonFinite = false;
    void Int(const char* k, int64_t v) override { Add(k, std::to_string(v)); }
    void Uint(const char* k, uint64_t v) override { Add(k, std::to_string(v)); }
    void Hex(const char* k, uint64_t v) override {
        char b[24];
        snprintf(b, sizeof(b), "\"0x%llx\"", static_cast<unsigned long long>(v));
        Add(k, b);
    }
    void Float(const char* k, double v) override { Add(k, Num(v)); }
    void Str(const char* k, std::string_view v) override { Add(k, "\"" + std::string(v) + "\""); }
    void Vec3(const char* k, const float v[3]) override { Add(k, "[" + Num(v[0]) + "," + Num(v[1]) + "," + Num(v[2]) + "]"); }

private:
    std::string Num(double v) {
        if (!std::isfinite(v)) nonFinite = true;
        char b[32];
        snprintf(b, sizeof(b), "%g", v);
        return b;
    }
    void Add(const char* k, const std::string& v) {
        if (!text.empty()) text += ',';
        text += '"';
        text += k;
        text += "\":";
        text += v;
    }
};

// ---- counting subscribers for "bus.sub <name> [post|deliver]"
struct Counter {
    std::string name;
    Path path = Path::Post;
    SubId sub = 0;
    std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> logged{0};
    uint32_t lastReported = 0;
};
std::mutex g_verbMu;
std::map<std::string, Counter*> g_counters;  // key: "<lower name>/<path>"; Counters are never freed (a retired
                                             // table may still hold their pointer as `user`)
std::map<std::string, SubId> g_faulters;
uint64_t g_lastCounterLog = 0;
int* volatile g_nullTarget = nullptr;

void CountHandler(const MessageView& m, void* user) {
    auto* c = static_cast<Counter*>(user);
    const uint32_t n = c->count.fetch_add(1) + 1;
    if (c->logged.fetch_add(1) >= 3) return;  // decode the first three to the log
    TextOut out;
    const bool decoded = Decode(m, out);
    WF_INFO("[bus] sub %s #%u: id=%04x class=%s size=%u %s handle=%d fromPost=%d depth=%u caller=%s seq=%llu frame=%llu "
            "payload={%s}%s%s",
            m.name, n, m.id, m.className, m.size, PathName(m.path), m.handle, m.fromPost, m.depth,
            game::DescribeAddress(m.caller).c_str(), static_cast<unsigned long long>(m.seq),
            static_cast<unsigned long long>(m.frame), out.text.c_str(), decoded ? "" : " (no decoder)",
            out.nonFinite ? " NON-FINITE" : "");
}

void FaultHandler(const MessageView&, void*) {
    *g_nullTarget = 1;  // deliberate access violation (bus.fault)
}

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& ch : r) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return r;
}

// Splits "<name> [post|deliver]".
bool ParseNamePath(std::string_view args, std::string& name, Path& path) {
    auto trim = [](std::string_view s) {
        while (!s.empty() && isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
        while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
        return s;
    };
    args = trim(args);
    size_t sp = args.find_first_of(" \t");
    name = std::string(args.substr(0, sp));
    path = Path::Post;
    if (sp != std::string_view::npos) {
        std::string p = Lower(trim(args.substr(sp)));
        if (p == "deliver") path = Path::Deliver;
        else if (p != "post") return false;
    }
    return !name.empty();
}

bool VerbSub(std::string_view args, void*) {
    std::string name;
    Path path;
    if (!ParseNamePath(args, name, path)) {
        WF_WARN("[bus] usage: bus.sub <name> [post|deliver]");
        return false;
    }
    std::lock_guard lk(g_verbMu);
    std::string key = Lower(name) + "/" + PathName(path);
    if (g_counters.count(key)) {
        WF_INFO("[bus] sub %s (%s) already active", name.c_str(), PathName(path));
        return true;
    }
    auto* c = new Counter;
    c->name = name;
    c->path = path;
    c->sub = SubscribeName(name.c_str(), path, &CountHandler, c);
    if (!c->sub) {
        delete c;
        return false;
    }
    g_counters[key] = c;
    WF_INFO("[bus] sub %s (%s): counting subscriber %u added (id %04x%s)", name.c_str(), PathName(path), c->sub,
            IdOf(name), IdOf(name) == kInvalidId ? ", pending" : "");
    return true;
}

bool VerbUnsub(std::string_view args, void*) {
    std::string name;
    Path path;
    if (!ParseNamePath(args, name, path)) return false;
    std::lock_guard lk(g_verbMu);
    auto it = g_counters.find(Lower(name) + "/" + PathName(path));
    if (it == g_counters.end()) {
        WF_WARN("[bus] unsub %s (%s): no such counting subscriber", name.c_str(), PathName(path));
        return false;
    }
    Unsubscribe(it->second->sub);
    WF_INFO("[bus] unsub %s (%s): final count=%u", name.c_str(), PathName(path), it->second->count.load());
    g_counters.erase(it);  // the Counter itself is leaked on purpose (see g_counters)
    return true;
}

bool VerbFault(std::string_view args, void*) {
    std::string name;
    Path path;
    if (!ParseNamePath(args, name, path)) return false;
    SubId id = SubscribeName(name.c_str(), path, &FaultHandler, nullptr);
    if (!id) return false;
    std::lock_guard lk(g_verbMu);
    g_faulters[Lower(name)] = id;
    WF_INFO("[bus] fault %s (%s): faulting subscriber %u added; expect %u faults then DISABLED", name.c_str(),
            PathName(path), id, kMaxFaults);
    return true;
}

// "bus.stats [name ...]": totals, plus per-id Post/Deliver deltas since the previous bus.stats (top 10 by
// posts when no names are given).
uint32_t g_prevCount[kPaths][0x10000];
uint64_t g_prevStatsTick = 0;
bool VerbStats(std::string_view args, void*) {
    const uint64_t now = GetTickCount64();
    const double secs = g_prevStatsTick ? static_cast<double>(now - g_prevStatsTick) / 1000.0 : 0.0;
    const Stats s = GetStats();
    WF_INFO("[bus] stats: posts=%llu deliveries=%llu handlerCalls=%llu handlerFaults=%llu avgHookUs=%.3f "
            "registry=%s names=%zu capacity=%zu pendingNames=%zu interval=%.1fs",
            static_cast<unsigned long long>(s.posts), static_cast<unsigned long long>(s.deliveries),
            static_cast<unsigned long long>(s.handlerCalls), static_cast<unsigned long long>(s.handlerFaults), s.avgHookUs,
            RegistryReady() ? "ready" : "not-ready", detail::UsedSlots(), Capacity(), detail::PendingNames(), secs);
    auto line = [&](MsgId id) {
        const uint32_t p = CountOf(id, Path::Post) - g_prevCount[0][id];
        const uint32_t d = CountOf(id, Path::Deliver) - g_prevCount[1][id];
        WF_INFO("[bus] stats %-32s id=%04x posts=%u deliveries=%u ratio=%.2f (totals %u/%u)", NameOf(id), id, p, d,
                p ? static_cast<double>(d) / p : 0.0, CountOf(id, Path::Post), CountOf(id, Path::Deliver));
    };
    std::string_view rest = args;
    bool named = false;
    while (!rest.empty()) {
        size_t a = rest.find_first_not_of(" \t,");
        if (a == std::string_view::npos) break;
        rest.remove_prefix(a);
        size_t b = rest.find_first_of(" \t,");
        std::string_view tok = rest.substr(0, b);
        rest.remove_prefix(b == std::string_view::npos ? rest.size() : b);
        named = true;
        MsgId id = IdOf(tok);
        if (id == kInvalidId) {
            WF_WARN("[bus] stats: '%.*s' is not a registered name", static_cast<int>(tok.size()), tok.data());
            continue;
        }
        line(id);
    }
    if (!named) {
        std::vector<std::pair<uint32_t, MsgId>> top;
        for (uint32_t id = 0; id < 0x10000; ++id) {
            const uint32_t p = CountOf(static_cast<MsgId>(id), Path::Post) - g_prevCount[0][id];
            if (p) top.push_back({p, static_cast<MsgId>(id)});
        }
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < top.size() && i < 10; ++i) line(top[i].second);
    }
    for (int p = 0; p < kPaths; ++p)
        for (uint32_t id = 0; id < 0x10000; ++id) g_prevCount[p][id] = g_count[p][id].load(std::memory_order_relaxed);
    g_prevStatsTick = now;
    return true;
}

// Registry round trip: IdOf(NameOf(id)) == id for every non-null slot (acceptance B1).
bool VerbSelfTest(std::string_view, void*) {
    const bool ready = RegistryReady();
    size_t names = 0, mismatches = 0;
    std::string first;
    ForEachName([&](MsgId id, const char* name) {
        ++names;
        const char* n = NameOf(id);
        const MsgId back = IdOf(n);
        if (back != id || n != name) {
            if (!mismatches) {
                char b[160];
                snprintf(b, sizeof(b), "%04x '%s' -> %04x", id, name, back);
                first = b;
            }
            ++mismatches;
        }
    });
    size_t sysBad = 0;
    for (MsgId id : {MsgId{0x40}, MsgId{0x103}, MsgId{0x104}, MsgId{0x1004}})
        if (IdOf(NameOf(id)) != id) ++sysBad;
    const bool pass = ready && names > 0 && mismatches == 0 && sysBad == 0;
    WF_INFO("[bus] selftest %s: ready=%d names=%zu (expect >=1227) capacity=%zu (expect 1300) roundtrip mismatches=%zu%s%s "
            "sysName mismatches=%zu",
            pass ? "PASS" : "FAIL", ready, names, Capacity(), mismatches, mismatches ? " first: " : "", first.c_str(),
            sysBad);
    return pass;
}

size_t WriteRegistryTsv() {
    std::wstring path = game::DataDir() + L"\\messages.tsv";
    FILE* f = _wfopen(path.c_str(), L"w");
    if (!f) {
        WF_WARN("[bus] cannot write %s", game::Narrow(path).c_str());
        return 0;
    }
    size_t n = 0;
    fprintf(f, "id\tname\n");
    ForEachName([&](MsgId id, const char* name) {
        fprintf(f, "%04x\t%s\n", id, name);
        ++n;
    });
    fclose(f);
    WF_INFO("[bus] wrote %zu names (capacity %zu) to %s", n, Capacity(), game::Narrow(path).c_str());
    return n;
}

bool VerbDump(std::string_view, void*) { return WriteRegistryTsv() > 0; }

// ---- per-frame work
bool g_dumpRegistry = false;
bool g_dumped = false;
std::atomic<bool> g_matchStarted{false};
uint64_t g_readyTick = 0;
MsgId g_turnStarted = kInvalidId;
uint64_t g_frameN = 0;

void OnFrame() {
    ++g_frameN;
    if ((g_frameN % 30) == 0) detail::Tick();
    const uint64_t now = GetTickCount64();
    if (!g_readyTick && RegistryReady()) {
        g_readyTick = now;
        g_turnStarted = IdOf("GameLogic.Turn.Started");
        WF_INFO("[bus] registry ready at frame %llu: %zu names, capacity %zu",
                static_cast<unsigned long long>(events::FrameCount()), detail::UsedSlots(), Capacity());
    }
    // DumpRegistry: once, at the first match start (MatchStart or the first turn) or after 5 s in the menu.
    if (g_dumpRegistry && !g_dumped && g_readyTick) {
        const bool matched = g_matchStarted.load() || (g_turnStarted != kInvalidId && CountOf(g_turnStarted, Path::Post));
        if (matched || now - g_readyTick >= 5000) {
            g_dumped = true;
            WriteRegistryTsv();
        }
    }
    // Counting subscribers report every 5 s.
    if (now - g_lastCounterLog >= 5000) {
        g_lastCounterLog = now;
        std::lock_guard lk(g_verbMu);
        for (auto& [key, c] : g_counters) {
            const uint32_t n = c->count.load();
            WF_INFO("[bus] sub %s (%s): count=%u (+%u)", c->name.c_str(), PathName(c->path), n, n - c->lastReported);
            c->lastReported = n;
        }
    }
}

class EventBus final : public Module {
public:
    const char* Name() const override { return "EventBus"; }
    const char* Description() const override { return "engine message bus: Post/Deliver hooks, subscribers, names"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 25; }

    bool Install() override {
        g_dumpRegistry = Bool("DumpRegistry", false);
        // 0x6910e4: push ebp; mov ebp,esp; push ecx; push ecx; cmp dword [0x96d090],0
        if (!mem::Expect(kPost, {0x55, 0x8b, 0xec, 0x51, 0x51, 0x83, 0x3d, 0x90, 0xd0, 0x96, 0x00, 0x00})) {
            WF_ERROR("[bus] Post 0x6910e4: unexpected bytes (another mod hooked it?)");
            return false;
        }
        // 0x68cb82: push ebp; mov ebp,esp; sub esp,0x4a4 ... ret 0xc
        if (!mem::Expect(kDeliver, {0x55, 0x8b, 0xec, 0x81, 0xec, 0xa4, 0x04, 0x00, 0x00})) {
            WF_ERROR("[bus] Deliver 0x68cb82: unexpected bytes (another mod hooked it?)");
            return false;
        }
        g_postHook = safetyhook::create_inline(kPost, &HookPost);
        g_deliverHook = safetyhook::create_inline(kDeliver, &HookDeliver);
        if (!g_postHook || !g_deliverHook) {
            WF_ERROR("[bus] hook install failed: Post %s, Deliver %s", g_postHook ? "ok" : "FAILED",
                     g_deliverHook ? "ok" : "FAILED");
            g_deliverHook = {};
            g_postHook = {};
            return false;
        }
        g_installed = true;
        WF_INFO("[bus] hooks installed: Post 0x6910e4, Deliver 0x68cb82 (DumpRegistry=%d)", g_dumpRegistry);

        events::Subscribe(events::Event::Frame, [] { OnFrame(); });
        events::Subscribe(events::Event::MatchStart, [] { g_matchStarted = true; });
        testcmd::Register("bus.sub", &VerbSub);
        testcmd::Register("bus.unsub", &VerbUnsub);
        testcmd::Register("bus.fault", &VerbFault);
        testcmd::Register("bus.stats", &VerbStats);
        testcmd::Register("bus.selftest", &VerbSelfTest);
        testcmd::Register("bus.dump", &VerbDump);
        return true;
    }
    void Uninstall() override {
        g_installed = false;
        g_deliverHook = {};
        g_postHook = {};
    }
};
}  // namespace

WUMFIX_MODULE(EventBus);
}  // namespace wf::bus
