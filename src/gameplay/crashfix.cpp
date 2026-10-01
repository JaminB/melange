#include "gameplay/crashfix.h"

#include <atomic>
#include <cstring>

namespace melange::crashfix {
bool NullToSink(uintptr_t& reg, Sink& sink) {
    if (reg) return false;
    reg = reinterpret_cast<uintptr_t>(sink.bytes);
    return true;
}

namespace {
constexpr uintptr_t kPostProcess = 0x5C;
constexpr int kSetSepia = 5;

std::atomic<bool> g_sepia{false};
uintptr_t g_sepiaDone = 0;

// Stands in for the thiscall SetSepia: `this` in ecx, the flag on the stack, popped by the callee.
void __fastcall RecordSepia(void*, void*, int on) {
    if (on) g_sepia = true;
}

void* g_standInVtbl[8] = {nullptr, nullptr, nullptr, nullptr, nullptr, reinterpret_cast<void*>(&RecordSepia)};
void** g_standInPp = g_standInVtbl;
struct alignas(16) {
    uint8_t bytes[0x60];
} g_standInApp{};

uintptr_t Read32(uintptr_t addr) {
    uint32_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(addr), sizeof(v));
    return v;
}
}  // namespace

bool SepiaToStandIn(uintptr_t& appData) {
    if (appData && Read32(appData + kPostProcess)) return false;
    const uint32_t pp = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_standInPp));
    std::memcpy(g_standInApp.bytes + kPostProcess, &pp, sizeof(pp));
    appData = reinterpret_cast<uintptr_t>(g_standInApp.bytes);
    return true;
}

bool SepiaRequested() { return g_sepia; }

bool ApplySepia(uintptr_t pp) {
    if (!g_sepia || !pp || pp == g_sepiaDone) return false;
    using SetSepia = void(__thiscall*)(void*, int);
    auto set = reinterpret_cast<SetSepia>(reinterpret_cast<void**>(Read32(pp))[kSetSepia]);
    set(reinterpret_cast<void*>(pp), 1);
    g_sepiaDone = pp;
    return true;
}

void ResetSepia() {
    g_sepia = false;
    g_sepiaDone = 0;
}
}  // namespace melange::crashfix
