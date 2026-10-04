#include "wormsign/session.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/mem.h"
#include "melange/jlog.h"
#include "wormsign/contrib.h"
#include "wormsign/detail.h"
#include "wormsign/fpu.h"
#include "wormsign/ring.h"

namespace melange::wormsign {
namespace {
constexpr uintptr_t kTM = 0x96d030;

std::atomic<bool> g_enabled{false}, g_open{false};
std::atomic<uint32_t> g_serial{0}, g_tick{0};
const char* g_endReason = "match-end";
uint32_t g_ticks = 0;
uint16_t g_inputs = 0;
TickRing g_ring;

std::atomic<uint32_t> g_seq{0};
TickHash g_last{};
bool g_haveLast = false;

template <class Fn>
struct Obs {
    int handle, order;
    Fn fn;
    void* user;
};

// Third-party modules subscribe here too (OnTickEnd is public SDK), so one observer's fault or bad_alloc must not
// take the whole tick-end call chain (and the game) down with it. A plain function, not a member or lambda, so
// the __try lives in a scope with no C++ objects needing unwinding.
template <class Fn, class... A>
bool CallGuarded(Fn fn, A... a) {
    __try {
        fn(a...);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class Fn>
struct ObsList {
    std::vector<Obs<Fn>> v;
    int next = 1, depth = 0;
    bool dirty = false;
    int Add(Fn fn, void* user, int order) {
        if (!fn) return 0;
        const int h = next++;
        auto it = std::upper_bound(v.begin(), v.end(), order, [](int o, const Obs<Fn>& e) { return o < e.order; });
        v.insert(it, Obs<Fn>{h, order, fn, user});
        return h;
    }
    void Remove(int h) {
        for (auto& e : v)
            if (e.handle == h) e.fn = nullptr, dirty = true;
        if (!depth) Compact();
    }
    void Compact() {
        if (!dirty) return;
        v.erase(std::remove_if(v.begin(), v.end(), [](const Obs<Fn>& e) { return !e.fn; }), v.end());
        dirty = false;
    }
    template <class... A>
    void Call(A... a) {
        ++depth;
        for (size_t i = 0; i < v.size(); ++i) {
            const Obs<Fn> e = v[i];
            if (e.fn && !CallGuarded(e.fn, a..., e.user))
                LOG_ERROR("[wormsign] an observer (handle %d) faulted and was skipped this tick", e.handle);
        }
        if (!--depth) Compact();
    }
};
ObsList<TickEndFn> g_tickObs;
ObsList<SessionFn> g_sessObs;

// Cost in integer QPC units; converted only when read.
constexpr uint32_t kBins = 4000;  // 0.1 us bins up to 400 us
uint32_t g_hashHist[kBins];
uint64_t g_costTicks = 0, g_hashSum = 0, g_hashMax = 0, g_tickSum = 0;
int64_t g_freq = 0;

int64_t Qpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
uint32_t Bin(int64_t d) {
    const int64_t b = d * 10000000 / g_freq;
    return static_cast<uint32_t>((std::min<int64_t>)(b, kBins - 1));
}

uint16_t Fpucw() {
    uint16_t cw;
    __asm fnstcw cw;
    return cw;
}

void Publish(const TickHash& h) {
    const uint32_t s = g_seq.load(std::memory_order_relaxed);
    g_seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_last = h;
    g_haveLast = true;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.store(s + 2, std::memory_order_release);
}
void Unpublish() {
    const uint32_t s = g_seq.load(std::memory_order_relaxed);
    g_seq.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    g_haveLast = false;
    std::atomic_thread_fence(std::memory_order_release);
    g_seq.store(s + 2, std::memory_order_release);
}
}  // namespace

namespace session {
void SetEnabled(bool on) {
    g_enabled = on;
    if (!g_freq) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_freq = f.QuadPart;
    }
}

bool Open() { return g_open.load(std::memory_order_acquire); }
const char* EndReason() { return g_endReason; }

void Begin() {
    if (g_open) return;
    const uint32_t serial = ++g_serial;
    g_ring.Reset();
    contrib::ResetSession();
    detail::Reset();
    g_tick = 0;
    g_ticks = 0;
    g_inputs = 0;
    Unpublish();
    g_open = true;
    LOG_INFO("[wormsign] session %u begins", serial);
    jlog::Rec("wormsign", jlog::Level::Info, "session begin").Uint("serial", serial);
    g_sessObs.Call(true, serial);
}

void End(const char* reason) {
    if (!g_open) return;
    const uint32_t serial = g_serial;
    g_endReason = reason;
    g_open = false;
    Unpublish();
    g_sessObs.Call(false, serial);
    LOG_INFO("[wormsign] session %u ends (%s): %u ticks, last tick %u", serial, reason, g_ticks, g_tick.load());
    jlog::Rec("wormsign", jlog::Level::Info, "session end")
        .Uint("serial", serial).Str("reason", reason).Uint("ticks", g_ticks).Uint("lastTick", g_tick);
}

void EndTick(uint32_t bucket, const PoppedTask& popped) {
    const int64_t t0 = Qpc();
    TickHash h{};
    detail::DetailRec* rec = detail::Scratch();
    ComputeEngine(bucket * kTickMs, &h, popped, rec);
    const int64_t t1 = Qpc();
    h.mods = contrib::HashTick(bucket);
    h.fpucw = Fpucw();
    fpu::Tick(g_serial, bucket, h.fpucw);
    detail::Commit(*rec);
    h.inputs = g_inputs;
    g_inputs = 0;
    g_ring.Put(h);
    g_tick = bucket;
    ++g_ticks;
    Publish(h);
    g_tickObs.Call(h);
    if (bucket % 50 == 0 && jlog::Enabled("wormsign", jlog::Level::Debug))
        jlog::Rec("wormsign", jlog::Level::Debug, "tick")
            .Uint("tick", bucket).Hex("engine", h.engine).Hex("mods", h.mods).Hex("rng", h.rngLogic);
    const int64_t t2 = Qpc();
    ++g_costTicks;
    g_hashSum += static_cast<uint64_t>(t1 - t0);
    g_hashMax = (std::max)(g_hashMax, static_cast<uint64_t>(t1 - t0));
    g_tickSum += static_cast<uint64_t>(t2 - t0);
    ++g_hashHist[Bin(t1 - t0)];
}

void CountInput() {
    if (g_open && g_inputs != 0xffff) ++g_inputs;
}

Cost GetCost() {
    Cost c{};
    c.ticks = g_costTicks;
    if (!g_costTicks || !g_freq) return c;
    const double us = 1e6 / static_cast<double>(g_freq);
    c.hashUsMean = static_cast<double>(g_hashSum) * us / static_cast<double>(g_costTicks);
    c.hashUsMax = static_cast<double>(g_hashMax) * us;
    c.tickUsMean = static_cast<double>(g_tickSum) * us / static_cast<double>(g_costTicks);
    uint64_t acc = 0;
    for (uint32_t i = 0; i < kBins; ++i) {
        acc += g_hashHist[i];
        if (acc * 100 >= g_costTicks * 95) {
            c.hashUsP95 = (i + 1) / 10.0;
            break;
        }
    }
    return c;
}

void ResetCost() {
    memset(g_hashHist, 0, sizeof g_hashHist);
    g_costTicks = g_hashSum = g_hashMax = g_tickSum = 0;
}
}  // namespace session

bool Enabled() { return g_enabled; }
bool InMatch() { return session::Open(); }
uint32_t MatchSerial() { return g_serial; }
uint32_t Tick() { return g_tick; }
uint32_t LogicTimeMs() {
    uintptr_t tm = 0;
    uint32_t t = 0;
    if (mem::SafeRead(kTM, &tm, sizeof tm) && tm) mem::SafeRead(tm + 0x38, &t, sizeof t);
    return t;
}

bool LastTick(TickHash* out) {
    for (int i = 0; i < 64; ++i) {
        const uint32_t s1 = g_seq.load(std::memory_order_acquire);
        if (s1 & 1) {
            YieldProcessor();
            continue;
        }
        const TickHash h = g_last;
        const bool have = g_haveLast;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_seq.load(std::memory_order_relaxed) != s1) continue;
        if (!have) return false;
        if (out) *out = h;
        return true;
    }
    return false;
}

bool TickAt(uint32_t tick, TickHash* out) { return g_open && g_ring.Get(tick, out); }

int OnTickEnd(TickEndFn fn, void* user, int order) { return g_tickObs.Add(fn, user, order); }
void RemoveOnTickEnd(int handle) { g_tickObs.Remove(handle); }
int OnSession(SessionFn fn, void* user) { return g_sessObs.Add(fn, user, 0); }
void RemoveOnSession(int handle) { g_sessObs.Remove(handle); }
}  // namespace melange::wormsign
