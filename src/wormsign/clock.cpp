#include "wormsign/clock.h"

#include <safetyhook.hpp>

#include <atomic>
#include <cstring>

#include "core/log.h"
#include "core/mem.h"
#include "lua/engine50.h"
#include "wormsign/hash_engine.h"
#include "wormsign/session.h"

namespace melange::wormsign::clock {
namespace {
constexpr uintptr_t kTM = 0x96d030;
constexpr uintptr_t kTmUpdate = 0x68d4a8;  // int __stdcall TaskManager::Update(this, int* now)
constexpr uintptr_t kPreTask = 0x68d85c;   // in 0x68d6f1: [ebp-0x2c] task, [ebp-0x10] time, [ebp-0xc] category
constexpr uintptr_t kGetInt = 0x50b790;

SafetyHookInline g_hTm;
SafetyHookMid g_mPre;
std::atomic<bool> g_installed{false}, g_on{false};
std::atomic<uint32_t> g_bucket{0};
bool g_haveB = false;
uint32_t g_curB = 0, g_lastT = 0;
std::atomic<PreTickFn> g_preTick{nullptr};
std::atomic<NowFn> g_nowFilter{nullptr};

void OnPreTask(safetyhook::Context& c) {
    if (!session::Open()) return;
    const uintptr_t task = *reinterpret_cast<uintptr_t*>(c.ebp - 0x2c);
    const uint32_t t = *reinterpret_cast<uint32_t*>(c.ebp - 0x10);
    const int cat = *reinterpret_cast<int*>(c.ebp - 0xc);
    const uint32_t b = (t + kTickMs - 1) / kTickMs;
    if (g_haveB && b == g_curB) {
        if (t == g_lastT) return;
        g_lastT = t;
        if (PreTickFn fn = g_preTick.load(std::memory_order_relaxed)) fn(b, t);
        return;
    }
    if (g_haveB && b > g_curB) session::EndTick(g_curB, PoppedTask{cat, t, task});
    g_curB = b;
    g_haveB = true;
    g_lastT = t;
    g_bucket.store(b, std::memory_order_relaxed);
    if (PreTickFn fn = g_preTick.load(std::memory_order_relaxed)) fn(b, t);
}

int __stdcall HkTmUpdate(uintptr_t tm, int* now) {
    const bool in = lua50::MatchState() != nullptr;
    if (in && !session::Open()) {
        g_haveB = false;
        g_bucket = 0;
        session::Begin();
    } else if (!in && session::Open()) {
        session::End("match-end");
        g_bucket = 0;
    }
    const NowFn filter = g_nowFilter.load(std::memory_order_relaxed);
    if (!filter || !now || !session::Open()) return g_hTm.stdcall<int>(tm, now);
    const int real = *now;
    int v = filter(real);
    const int given = v;
    const int r = g_hTm.stdcall<int>(tm, &v);
    *now = real + (v - given);
    return r;
}

bool Toggle(bool on) {
    bool ok = true;
    if (on) {
        ok &= g_hTm.enable().has_value();
        ok &= g_mPre.enable().has_value();
    } else {
        ok &= g_mPre.disable().has_value();
        ok &= g_hTm.disable().has_value();
    }
    return ok;
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    const bool bytes = mem::Expect(kTmUpdate, {0x55, 0x8b, 0xec, 0x51, 0xff, 0x75, 0x0c, 0x8b, 0x45, 0x08, 0x8b, 0x48, 0x1c, 0xe8}) &&
                       mem::Expect(kPreTask, {0xff, 0x75, 0xf0, 0x8b, 0x45, 0xd4, 0x8b, 0x00, 0xff, 0x75, 0xd4, 0xff, 0x50, 0x18}) &&
                       mem::Expect(kGetInt, {0xe8, -1, -1, -1, -1, 0x8b, 0x08, 0x8b, 0x51, 0x54, 0x68});
    if (!bytes) {
        LOG_ERROR("[wormsign] code bytes differ from build #1077: the tick clock stays off");
        return false;
    }
    using F = safetyhook::InlineHook::Flags;
    g_hTm = safetyhook::create_inline(kTmUpdate, &HkTmUpdate, F::StartDisabled);
    g_mPre = safetyhook::create_mid(kPreTask, &OnPreTask, safetyhook::MidHook::StartDisabled);
    if (!g_hTm || !g_mPre) {
        LOG_ERROR("[wormsign] creating the scheduler hooks failed: the tick clock stays off");
        g_mPre = {};
        g_hTm = {};
        return false;
    }
    g_installed = true;
    return SetHooksEnabled(true);
}

void Uninstall() {
    SetHooksEnabled(false);
}

bool Installed() { return g_installed; }

bool SetHooksEnabled(bool on) {
    if (!g_installed || g_on == on) return g_installed;
    if (!on) {
        session::End("hooks off");
        g_haveB = false;
        g_bucket = 0;
    }
    const bool ok = Toggle(on);
    g_on = on && ok;
    if (!ok) LOG_ERROR("[wormsign] %s the scheduler hooks failed", on ? "enabling" : "disabling");
    return ok;
}

bool HooksEnabled() { return g_on; }

bool ProloguesOriginal() {
    uint8_t a[5] = {}, b[5] = {};
    static constexpr uint8_t kA[5] = {0x55, 0x8b, 0xec, 0x51, 0xff}, kB[5] = {0xff, 0x75, 0xf0, 0x8b, 0x45};
    return mem::SafeRead(kTmUpdate, a, 5) && mem::SafeRead(kPreTask, b, 5) && !memcmp(a, kA, 5) && !memcmp(b, kB, 5);
}

void SetPreTick(PreTickFn fn) { g_preTick = fn; }
void SetNowFilter(NowFn fn) { g_nowFilter = fn; }

int TimeBase() {
    uintptr_t tm = 0, inner = 0;
    int base = 0;
    if (!mem::SafeRead(kTM, &tm, sizeof tm) || !tm || !mem::SafeRead(tm + 0x1c, &inner, sizeof inner) || !inner) return 0;
    return mem::SafeRead(inner + 4, &base, sizeof base) ? base : 0;
}
uint32_t CurrentBucket() { return session::Open() ? g_bucket.load(std::memory_order_relaxed) : 0; }

bool Paused() {
    uintptr_t tm = 0;
    uint8_t p = 0;
    return mem::SafeRead(kTM, &tm, sizeof tm) && tm && mem::SafeRead(tm + 0x3c, &p, 1) && p != 0;
}
}  // namespace melange::wormsign::clock
