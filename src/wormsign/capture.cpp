// The six ReplayMessageStore sender hooks and five insert hooks that feed the recorder's input stream, matching
// the addresses verified at runtime against build #1077 (0x542740..0x543130 senders, 0x53e970..0x53ec00 inserts,
// remote callers 0x68a9f0..0x68b200).
#include "wormsign/capture.h"
#include "wormsign/capture_internal.h"

#include <intrin.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstring>

#include "core/log.h"
#include "core/mem.h"
#include "melange/bus.h"
#include "melange/wormsign.h"
#include "wormsign/clock.h"
#include "wormsign/inject.h"
#include "wormsign/session.h"

namespace melange::wormsign::capture {
namespace {
// __cdecl(id, [a], [b], time); type 5 (string) takes an XString* in place of a/value.
constexpr uintptr_t kSend[6] = {0x542740, 0x5427f0, 0x5428b0, 0x542970, 0x542a30, 0x543130};
// __thiscall(ReplayMessageStore*, Info*) ret 4, one per NetStored{,Int,TwoInt,Float,TwoFloat} type (the string
// type has no insert: it never replicates over the network).
constexpr uintptr_t kIns[5] = {0x53e970, 0x53ea10, 0x53eae0, 0x53eb80, 0x53ec00};
// NetStored*Array::Handle callers: an insert whose return address falls here delivered a remote record.
constexpr uintptr_t kRemoteLo = 0x68a9f0, kRemoteHi = 0x68b200;
// The local-only exclusion list behind 0x539c00: 55 pointers to message names (HUD/menu/chat/camera), resolved to
// ids through the bus registry.
constexpr uintptr_t kLocalOnlyTable = 0x91eef8;
constexpr size_t kLocalOnlyCount = 55;

std::atomic<GateFn> g_gate{nullptr};
std::atomic<SendSink> g_sendSink{nullptr};
std::atomic<InsertSink> g_insertSink{nullptr};
std::atomic<void*> g_sendUser{nullptr}, g_insertUser{nullptr};
thread_local bool t_injecting = false;
std::atomic<bool> g_installed{false};

SafetyHookInline g_send[6];
SafetyHookInline g_ins[5];

bool PassGate(int type, uint16_t id) {
    if (GateFn fn = g_gate.load(std::memory_order_relaxed)) return fn(type, id, t_injecting) == Gate::Pass;
    return true;
}

void Emit(int type, uint16_t id, uint32_t a, uint32_t b, uint32_t time, uintptr_t caller, const char* str) {
    if (session::Open()) session::CountInput();
    if (SendSink fn = g_sendSink.load(std::memory_order_relaxed))
        fn(SendEvent{type, id, a, b, time, melange::wormsign::LogicTimeMs(), static_cast<uint32_t>(caller),
                     str ? str : "", t_injecting},
           g_sendUser.load(std::memory_order_relaxed));
}

void __cdecl OnSend0(uint32_t id, uint32_t time) {
    if (!PassGate(0, static_cast<uint16_t>(id))) return;
    Emit(0, static_cast<uint16_t>(id), 0, 0, time, reinterpret_cast<uintptr_t>(_ReturnAddress()), nullptr);
    g_send[0].ccall<void>(id, time);
}
void __cdecl OnSend1(uint32_t id, uint32_t a, uint32_t time) {
    if (!PassGate(1, static_cast<uint16_t>(id))) return;
    Emit(1, static_cast<uint16_t>(id), a, 0, time, reinterpret_cast<uintptr_t>(_ReturnAddress()), nullptr);
    g_send[1].ccall<void>(id, a, time);
}
void __cdecl OnSend2(uint32_t id, uint32_t a, uint32_t b, uint32_t time) {
    if (!PassGate(2, static_cast<uint16_t>(id))) return;
    Emit(2, static_cast<uint16_t>(id), a, b, time, reinterpret_cast<uintptr_t>(_ReturnAddress()), nullptr);
    g_send[2].ccall<void>(id, a, b, time);
}
void __cdecl OnSend3(uint32_t id, uint32_t a, uint32_t time) {
    if (!PassGate(3, static_cast<uint16_t>(id))) return;
    Emit(3, static_cast<uint16_t>(id), a, 0, time, reinterpret_cast<uintptr_t>(_ReturnAddress()), nullptr);
    g_send[3].ccall<void>(id, a, time);
}
void __cdecl OnSend4(uint32_t id, uint32_t a, uint32_t b, uint32_t time) {
    if (!PassGate(4, static_cast<uint16_t>(id))) return;
    Emit(4, static_cast<uint16_t>(id), a, b, time, reinterpret_cast<uintptr_t>(_ReturnAddress()), nullptr);
    g_send[4].ccall<void>(id, a, b, time);
}
void __cdecl OnSend5(uint32_t id, uintptr_t xstr, uint32_t time) {
    if (!PassGate(5, static_cast<uint16_t>(id))) return;
    char s[128] = "";
    uintptr_t p = 0;
    if (mem::SafeRead(xstr, &p, sizeof p) && p) mem::SafeRead(p, s, sizeof s - 1);
    Emit(5, static_cast<uint16_t>(id), static_cast<uint32_t>(xstr), 0, time, reinterpret_cast<uintptr_t>(_ReturnAddress()), s);
    g_send[5].ccall<void>(id, xstr, time);
}

template <int N>
void __fastcall OnInsert(uintptr_t store, void* /*edx*/, uintptr_t info) {
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (ret >= kRemoteLo && ret < kRemoteHi && session::Open()) {
        InsertEvent e{};
        mem::SafeRead(info + 4, &e.id, sizeof e.id);
        mem::SafeRead(info + 8, &e.time, sizeof e.time);
        mem::SafeRead(info + 0xc, &e.a, sizeof e.a);
        e.arrivedT = melange::wormsign::LogicTimeMs();
        if (InsertSink fn = g_insertSink.load(std::memory_order_relaxed))
            fn(e, g_insertUser.load(std::memory_order_relaxed));
    }
    g_ins[N].thiscall<void>(store, info);
}

// Prologues of the six senders and five inserts, verified at runtime against build #1077 (same rule and method
// as the scheduler hooks in clock.cpp: refuse rather than hook on top of a byte pattern that isn't this build's,
// for instance because another ASI already patched the same address first).
bool ProloguesOriginal() {
    return mem::Expect(kSend[0], {0xa1, 0x30, 0xd0, 0x96, 0x00, 0x83, 0xec, 0x0c, 0x56, 0x8b, 0x74, 0x24, 0x18}) &&
           mem::Expect(kSend[1], {0xa1, 0x30, 0xd0, 0x96, 0x00, 0x83, 0xec, 0x10, 0x56, 0x8b, 0x74, 0x24, 0x20}) &&
           mem::Expect(kSend[2], {0xa1, 0x30, 0xd0, 0x96, 0x00, 0x83, 0xec, 0x14, 0x56, 0x8b, 0x74, 0x24, 0x28}) &&
           mem::Expect(kSend[3], {0xa1, 0x30, 0xd0, 0x96, 0x00, 0x83, 0xec, 0x10, 0x56, 0x8b, 0x74, 0x24, 0x20}) &&
           mem::Expect(kSend[4], {0xa1, 0x30, 0xd0, 0x96, 0x00, 0x83, 0xec, 0x14, 0x56, 0x8b, 0x74, 0x24, 0x28}) &&
           mem::Expect(kSend[5], {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x28, 0x2b, 0x7d, 0x00}) &&
           mem::Expect(kIns[0], {0x83, 0xec, 0x08, 0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x5e, 0x0c, 0x57, 0x85, 0xdb, 0x75}) &&
           mem::Expect(kIns[1], {0x83, 0xec, 0x08, 0x56, 0x8b, 0xf1, 0x8b, 0x4e, 0x0c, 0x57, 0x85, 0xc9, 0x75, 0x04}) &&
           mem::Expect(kIns[2], {0x83, 0xec, 0x08, 0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x5e, 0x0c, 0x57, 0x85, 0xdb, 0x75}) &&
           mem::Expect(kIns[3], {0x83, 0xec, 0x08, 0x56, 0x8b, 0xf1, 0x8b, 0x4e, 0x0c, 0x57, 0x85, 0xc9, 0x75, 0x04}) &&
           mem::Expect(kIns[4], {0x83, 0xec, 0x08, 0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x5e, 0x0c, 0x57, 0x85, 0xdb, 0x75});
}

template <class F>
bool Inline(SafetyHookInline& h, uintptr_t a, F fn, const char* what) {
    h = safetyhook::create_inline(a, fn);
    if (!h) LOG_ERROR("[wormsign] capture hook %s at %08x failed", what, static_cast<unsigned>(a));
    return static_cast<bool>(h);
}
}  // namespace

void SetGate(GateFn fn) { g_gate = fn; }

// Through the hooked senders, so an injected input is gated, counted and recorded like a live one.
bool InjectSend(int type, uint16_t id, uint32_t a, uint32_t b, const char* str, uint32_t time) {
    using F2 = void(__cdecl*)(uint32_t, uint32_t);
    using F3 = void(__cdecl*)(uint32_t, uint32_t, uint32_t);
    using F4 = void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t);
    if (!g_installed || type < 0 || type > 5) return false;
    // Every id the game itself sends through these senders is a registered message name (0x8000 | slot), never a
    // raw system id below it; refuse anything else rather than trust a recording's id unchecked.
    if (!(id & 0x8000) || !bus::detail::RegistrySlot(id & 0x7fff)) return false;
    if (type == 5) {
        inject::EngineString s(str);
        if (!s.Ok()) return false;
        t_injecting = true;
        using F5 = void(__cdecl*)(uint32_t, uintptr_t, uint32_t);
        reinterpret_cast<F5>(kSend[5])(id, reinterpret_cast<uintptr_t>(s.Arg()), time);
        t_injecting = false;
        return true;
    }
    t_injecting = true;
    switch (type) {
        case 0: reinterpret_cast<F2>(kSend[0])(id, time); break;
        case 1:
        case 3: reinterpret_cast<F3>(kSend[type])(id, a, time); break;
        case 2:
        case 4: reinterpret_cast<F4>(kSend[type])(id, a, b, time); break;
        default: break;
    }
    t_injecting = false;
    return true;
}

bool LocalOnly(uint16_t id) {
    static std::vector<uint16_t> ids;
    static std::atomic<bool> ready{false};
    static std::mutex mu;
    if (!ready.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(mu);
        if (!ready) {
            uint32_t names[kLocalOnlyCount];
            if (!mem::SafeRead(kLocalOnlyTable, names, sizeof names)) return false;
            std::vector<uint16_t> v;
            for (uint32_t p : names) {
                char s[64] = "";
                if (!mem::SafeRead(p, s, sizeof s - 1)) continue;
                const bus::MsgId m = bus::IdOf(s);
                if (m != bus::kInvalidId) v.push_back(static_cast<uint16_t>(m));
            }
            if (v.empty()) return false;  // the bus registry is not ready yet
            std::sort(v.begin(), v.end());
            ids = std::move(v);
            ready.store(true, std::memory_order_release);
        }
    }
    return std::binary_search(ids.begin(), ids.end(), id);
}

void SetSendSink(SendSink fn, void* user) {
    g_sendUser = user;
    g_sendSink = fn;
}
void SetInsertSink(InsertSink fn, void* user) {
    g_insertUser = user;
    g_insertSink = fn;
}

bool Install() {
    if (g_installed) return true;
    // The scheduler's own #1077 prologue check already guards the tick clock; require it installed first, since
    // the send/insert sites are only ever exercised together with it.
    if (!clock::Installed()) {
        LOG_ERROR("[wormsign] capture: the tick clock is not installed, refusing to hook the input path");
        return false;
    }
    if (!ProloguesOriginal()) {
        LOG_ERROR("[wormsign] capture: code bytes differ from build #1077 at a sender or insert site, refusing to "
                  "hook the input path");
        return false;
    }
    bool ok = true;
    ok &= Inline(g_send[0], kSend[0], &OnSend0, "send msg");
    ok &= Inline(g_send[1], kSend[1], &OnSend1, "send int");
    ok &= Inline(g_send[2], kSend[2], &OnSend2, "send int2");
    ok &= Inline(g_send[3], kSend[3], &OnSend3, "send float");
    ok &= Inline(g_send[4], kSend[4], &OnSend4, "send float2");
    ok &= Inline(g_send[5], kSend[5], &OnSend5, "send string");
    ok &= Inline(g_ins[0], kIns[0], &OnInsert<0>, "insert msg");
    ok &= Inline(g_ins[1], kIns[1], &OnInsert<1>, "insert int");
    ok &= Inline(g_ins[2], kIns[2], &OnInsert<2>, "insert int2");
    ok &= Inline(g_ins[3], kIns[3], &OnInsert<3>, "insert float");
    ok &= Inline(g_ins[4], kIns[4], &OnInsert<4>, "insert float2");
    if (!ok) {
        for (auto& h : g_send) h = {};
        for (auto& h : g_ins) h = {};
        return false;
    }
    g_installed = true;
    return true;
}
bool Installed() { return g_installed; }
}  // namespace melange::wormsign::capture
