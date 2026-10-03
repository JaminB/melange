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

namespace {
// Laid out like the game's vector and float tweak resources: the value sits at details+0x1C. Release (slot 2) and the
// other slots the game may call are no-ops on these static holders.
struct TweakHolder {
    void** vtbl;
    const float* details;
};

uint32_t __fastcall HolderNoOp(void*, void*) { return 0; }

void* g_holderVtbl[8] = {&HolderNoOp, &HolderNoOp, &HolderNoOp, &HolderNoOp,
                         &HolderNoOp, &HolderNoOp, &HolderNoOp, &HolderNoOp};
const float g_colourDetails[10] = {0, 0, 0, 0, 0, 0, 0, 1.0f, 0.8f, 0.6f};
const float g_weightDetails[8] = {0, 0, 0, 0, 0, 0, 0, 0.4f};
TweakHolder g_colourHolder{g_holderVtbl, g_colourDetails};
TweakHolder g_weightHolder{g_holderVtbl, g_weightDetails};

Tint Fill(uintptr_t slot, const char* name, ResolveTweak resolve, TweakHolder& holder) {
    if (Read32(slot)) return Tint::Present;
    auto* out = reinterpret_cast<uint32_t*>(slot);
    if (resolve && resolve(&name, out) >= 0 && *out) return Tint::Resolved;
    *out = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&holder));
    return Tint::Fallback;
}
}  // namespace

Tint EnsureSepiaTint(uintptr_t pp, ResolveTweak colour, ResolveTweak weight) {
    if (!pp) return Tint::Present;
    const Tint c = Fill(pp + 0x34, "Sepia.Color", colour, g_colourHolder);
    const Tint w = Fill(pp + 0x38, "Sepia.LerpWeight", weight, g_weightHolder);
    return c > w ? c : w;
}

void ResetSepia() {
    g_sepia = false;
    g_sepiaDone = 0;
}
}  // namespace melange::crashfix
