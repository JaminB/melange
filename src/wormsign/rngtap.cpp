// Taps on the four RNG draw wrappers and two seed setters. Record-only until a replay player installs a forced
// slot to drive them instead.
#include "wormsign/rngtap.h"
#include "wormsign/rngtap_internal.h"

#include <intrin.h>
#include <safetyhook.hpp>

#include <atomic>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "melange/wormsign.h"
#include "wormsign/session.h"

namespace melange::wormsign::rngtap {
namespace {
constexpr uintptr_t kSeedLogic = 0x68c053, kSeed2 = 0x68c0b9;
constexpr uintptr_t kDrawU = 0x68c015, kDrawF = 0x68c024, kDraw2F = 0x68c07b, kDraw2U = 0x68c0aa;
constexpr uintptr_t kRngStateLogic = 0x96d034, kRngState2 = 0x96d040;
// 0x4eded0's two call sites: menu entry, then match start (both reseed with the same QPC-ms value).
constexpr uintptr_t kMenuEntrySeed = 0x4ee250;
constexpr size_t kPreDrawCap = 200000;

SafetyHookInline g_seedL, g_seed2, g_drawU, g_drawF, g_draw2F, g_draw2U;
std::atomic<ForcedDrawFn> g_forcedDraw{nullptr};
std::atomic<ForcedSeedFn> g_forcedSeed{nullptr};
std::atomic<bool> g_installed{false};

std::mutex g_mu;
std::vector<DrawEvent> g_pre;
bool g_preOverflow = false;
std::vector<SeedEvent> g_seeds;
// Both the menu-entry and the match-start seed come from 0x4ee250: the draws and seeds kept for a match start at the
// one before the last of them. [prev, last) is that window's first segment in g_pre / g_seeds.
size_t g_drawPrev = 0, g_drawLast = 0, g_seedPrev = 0, g_seedLast = 0;

template <class T>
uint32_t ToBits(T v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    return b;
}

template <class T>
T DoDraw(int rng, uintptr_t ret, SafetyHookInline& h) {
    uint32_t bits;
    T v;
    if (ForcedDrawFn fn = g_forcedDraw.load(std::memory_order_relaxed);
        fn && fn(rng, static_cast<uint32_t>(ret), &bits)) {
        memcpy(&v, &bits, 4);
    } else {
        v = h.ccall<T>();
        bits = ToBits(v);
    }
    if (!session::Open()) {
        std::lock_guard<std::mutex> lk(g_mu);
        const uint32_t state = *reinterpret_cast<volatile uint32_t*>(rng ? kRngState2 : kRngStateLogic);
        if (g_pre.size() < kPreDrawCap) g_pre.push_back(DrawEvent{rng, static_cast<uint32_t>(ret), bits, state});
        else g_preOverflow = true;
    }
    return v;
}
uint32_t __cdecl HkDrawU() { return DoDraw<uint32_t>(0, reinterpret_cast<uintptr_t>(_ReturnAddress()), g_drawU); }
float __cdecl HkDrawF() { return DoDraw<float>(0, reinterpret_cast<uintptr_t>(_ReturnAddress()), g_drawF); }
float __cdecl HkDraw2F() { return DoDraw<float>(1, reinterpret_cast<uintptr_t>(_ReturnAddress()), g_draw2F); }
uint32_t __cdecl HkDraw2U() { return DoDraw<uint32_t>(1, reinterpret_cast<uintptr_t>(_ReturnAddress()), g_draw2U); }

void OnSeed(int kind, uint32_t& v, uintptr_t ret) {
    if (kind == 0 && ret == kMenuEntrySeed) {
        std::lock_guard<std::mutex> lk(g_mu);
        g_pre.erase(g_pre.begin(), g_pre.begin() + static_cast<std::ptrdiff_t>(g_drawPrev));
        g_seeds.erase(g_seeds.begin(), g_seeds.begin() + static_cast<std::ptrdiff_t>(g_seedPrev));
        g_drawPrev = g_drawLast - g_drawPrev;
        g_seedPrev = g_seedLast - g_seedPrev;
        g_drawLast = g_pre.size();
        g_seedLast = g_seeds.size();
        g_preOverflow = false;
    }
    if (ForcedSeedFn fn = g_forcedSeed.load(std::memory_order_relaxed)) {
        uint32_t forced = v;
        if (fn(kind, static_cast<uint32_t>(ret), &forced)) v = forced;
    }
    std::lock_guard<std::mutex> lk(g_mu);
    g_seeds.push_back(SeedEvent{kind, v, static_cast<uint32_t>(ret), melange::wormsign::LogicTimeMs()});
}
void __cdecl HkSeedL(uint32_t v) {
    OnSeed(0, v, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    g_seedL.ccall<void>(v);
}
void __cdecl HkSeed2(uint32_t v) {
    OnSeed(1, v, reinterpret_cast<uintptr_t>(_ReturnAddress()));
    g_seed2.ccall<void>(v);
}

template <class F>
bool Mk(SafetyHookInline& h, uintptr_t a, F fn, const char* what) {
    h = safetyhook::create_inline(a, fn);
    if (!h) LOG_ERROR("[wormsign] rngtap hook %s at %08x failed", what, static_cast<unsigned>(a));
    return static_cast<bool>(h);
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    bool ok = true;
    ok &= Mk(g_seedL, kSeedLogic, &HkSeedL, "seed logic");
    ok &= Mk(g_seed2, kSeed2, &HkSeed2, "seed second");
    ok &= Mk(g_drawU, kDrawU, &HkDrawU, "draw logic uint");
    ok &= Mk(g_drawF, kDrawF, &HkDrawF, "draw logic float");
    ok &= Mk(g_draw2F, kDraw2F, &HkDraw2F, "draw second float");
    ok &= Mk(g_draw2U, kDraw2U, &HkDraw2U, "draw second uint");
    if (!ok) {
        g_seedL = {};
        g_seed2 = {};
        g_drawU = {};
        g_drawF = {};
        g_draw2F = {};
        g_draw2U = {};
        return false;
    }
    g_installed = true;
    return true;
}
bool Installed() { return g_installed; }
void SetForcedDraw(ForcedDrawFn fn) { g_forcedDraw = fn; }
void SetForcedSeed(ForcedSeedFn fn) { g_forcedSeed = fn; }

std::vector<SeedEvent> TakeSessionSeeds() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<SeedEvent> out(g_seeds.begin() + static_cast<std::ptrdiff_t>(g_seedPrev), g_seeds.end());
    g_seeds.clear();
    g_seedPrev = g_seedLast = 0;
    return out;
}
std::vector<DrawEvent> PreMatchDraws(bool* overflow) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (overflow) *overflow = g_preOverflow;
    std::vector<DrawEvent> out(g_pre.begin() + static_cast<std::ptrdiff_t>(g_drawPrev), g_pre.end());
    g_pre.clear();
    g_drawPrev = g_drawLast = 0;
    return out;
}
}  // namespace melange::wormsign::rngtap
