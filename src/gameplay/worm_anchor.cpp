// WormAnchor: the engine side of wum.sim.worm (src/lua/sim/sim_worms.cpp) for build #1077.
//
// The worm data containers (WormDataContainer, 16 resource handles at 0x95b4a8) are the simulation's own state: the
// physics writes them, Wormsign hashes them (position +0x38, velocity +0x50, state +0xf0 among others), so reading
// them from a sim script is deterministic, and pinning them is too as long as every machine pins at the same tick.
//
// The anchor reuses the engine's own "nailed" worm (No More Nails, DirtBallLogicEntity 0x5ce320): bits 0x30 of
// container +0xec. With 0x10 set, WXWormLogicEntity::ChangeState 0x5aa7f0 refuses the Ballistic and Sliding
// transitions and the explosion handler 0x5ae4f0 skips the impulse 0x5ad010; with 0x20 the analog movement input
// is ignored (0x5ac390) and the turn UI knows the worm is nailed. The engine clears the nail on small hits, so three
// hooks keep the anchor whole:
// - WXWormLogicEntity::Update 0x5b1d60 (vtable slot 6, stdcall(this, time)): re-pins the position, zeroes the
//   velocity and re-asserts the flags before and after the worm's own step.
// - ChangeState 0x5aa7f0 (thiscall(this; container, state)): refuses Ballistic/Sliding for an anchored worm even
//   when the engine has just cleared its nail (that is how a knocked worm would otherwise fly).
// - The input handler 0x5ac6e0 (thiscall(this; message), only reached for the active worm): swallows the walk,
//   jump, backflip and jetpack inputs and the fire inputs of the movement utilities (rope, parachute, jetpack,
//   teleport). Turning (Input.MoveLeft/Right on the keyboard: A and D rotate the worm in place) and aiming pass.
//   Inputs arrive on every machine through the same path (live or NetStored replay), so swallowing them here keeps
//   the machines in step.
// Dying and drowning worms (states 7 and 8) are left to the engine: its drowning check reads the pinned position, so
// a worm the water rises over (feet + Worm.Drown.HeightOffset) enters DrownFloat by itself and the pin lets go
// (sim_worms.cpp, Repin). Verified in game on 2026-10-24: walk and jump inputs swallowed, A/D turned the worm with
// its position fixed, a bazooka at its feet took 53 hp and refused the Ballistic transition, the turn ended normally,
// and water raised 25 units over its feet drowned it.
#include <windows.h>
#include <safetyhook.hpp>

#include <cstring>
#include <mutex>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "game/state/gamestate_internal.h"
#include "lua/engine50.h"
#include "lua/sim/sim_worms.h"
#include "melange/bus.h"
#include "melange/gamestate.h"
#include "melange/sim.h"
#include "melange/wormsign.h"

namespace {
using melange::gamestate::detail::Rd;
namespace sw = melange::simworms;

constexpr uintptr_t kWormHandles = 0x95b4a8, kWormVt = 0x8747d4;
constexpr uintptr_t kWormUpdate = 0x5b1d60, kWormInput = 0x5ac6e0, kChangeState = 0x5aa7f0;
constexpr uintptr_t kXomRoot = 0x966d98, kDataGuid = 0x888288;
constexpr uintptr_t kWeaponNames = 0x90c920;
constexpr int kWeaponNameCount = 0x45;
constexpr uint32_t kEntitySlot = 0x30;  // WXWormLogicEntity +0x30: the worm index (m_bWormIndexSet at +0x45)
constexpr int kDataVector = 3;

SafetyHookInline g_hUpdate, g_hInput, g_hChangeState;
bool g_installed = false, g_hooksOn = false, g_logInputs = false;
int g_tickHook = 0, g_contrib = 0;
uint32_t g_idsSerial = 0;

// Guarded container writes: the containers live on the heap, so a plain store under SEH is enough.
bool Poke(uintptr_t addr, const void* in, size_t n) {
    __try {
        memcpy(reinterpret_cast<void*>(addr), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's data store, as GetData 0x696b44 / the Rm.cpp getters reach it: root service -> data resource manager
// (GUID 0x888288) -> descriptor by name. The descriptor's value lives at *(desc+4)+0x1c; it is released after the copy.
template <class F>
F Method(uintptr_t obj, size_t off) {
    return reinterpret_cast<F>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off));
}

bool ReadData(const char* name, int wantType, void* out, size_t n) {
    const uintptr_t root = Rd<uintptr_t>(kXomRoot);
    if (!root) return false;
    uintptr_t desc = 0;
    bool ok = false;
    __try {
        const uintptr_t svc = Method<uintptr_t(__stdcall*)(uintptr_t, uintptr_t)>(root, 0x54)(root, kDataGuid);
        if (!svc) return false;
        const char* nm = name;
        const int rc = Method<int(__stdcall*)(uintptr_t, const char**, uintptr_t*)>(svc, 0x74)(svc, &nm, &desc);
        if (rc >= 0 && desc) {
            const int type = Method<int(__stdcall*)(uintptr_t)>(desc, 0x10)(desc);
            if (type == wantType) ok = melange::gamestate::detail::Copy(Rd<uintptr_t>(desc + 4) + 0x1c, out, n);
        }
        if (desc) Method<uintptr_t(__stdcall*)(uintptr_t)>(desc, 0x8)(desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

uintptr_t ContainerOf(int slot) {
    return slot >= 0 && slot < sw::kMaxWorms ? melange::gamestate::detail::Container(kWormHandles, slot, kWormVt) : 0;
}

class EngineBackend final : public sw::Backend {
public:
    bool Read(int slot, sw::WormInfo* w) override {
        const uintptr_t c = ContainerOf(slot);
        if (!c) return false;
        *w = sw::WormInfo{};
        w->active = Rd<uint8_t>(c + 0x124) != 0;
        w->state = Rd<uint32_t>(c + 0xf0);
        w->flags = Rd<uint32_t>(c + 0xec);
        w->health = Rd<uint16_t>(c + 0x11e);
        w->team = Rd<uint8_t>(c + 0x127);
        w->posInTeam = Rd<uint8_t>(c + 0x128);
        const int32_t wi = Rd<int32_t>(c + 0xf4);
        w->weapon = wi >= 1 && wi < kWeaponNameCount ? wi : -1;
        melange::gamestate::detail::Copy(c + 0x38, w->pos, 12);
        melange::gamestate::detail::Copy(c + 0x50, w->vel, 12);
        w->yaw = Rd<float>(c + 0x90);
        melange::gamestate::detail::ReadCString(Rd<uintptr_t>(c + 0xb0), w->name, sizeof w->name);
        return true;
    }
    bool Write(int slot, const float pos[3], const float vel[3], uint32_t set, uint32_t clear) override {
        const uintptr_t c = ContainerOf(slot);
        if (!c) return false;
        bool ok = Poke(c + 0x38, pos, 12) && Poke(c + 0x50, vel, 12);
        if (set || clear) {
            const uint32_t f = (Rd<uint32_t>(c + 0xec) | set) & ~clear;
            ok = Poke(c + 0xec, &f, 4) && ok;
        }
        return ok;
    }
    bool Land(float out[6]) override {
        return ReadData("Land.MinBounds", kDataVector, out, 12) && ReadData("Land.MaxBounds", kDataVector, out + 3, 12);
    }
};
EngineBackend g_backend;

// ---------------------------------------------------------------- input block
// Resolved once per match from the message registry (names the engine did not register stay unresolved).
constexpr const char* kAlwaysBlocked[] = {
    "Input.MoveForwardPressed", "Input.MoveBackwardPressed", "Input.JumpPressed", "Input.JumpBackPressed",
    "Input.MoveY",
    "Input.Jetpack.ForwardPressed", "Input.Jetpack.BackPressed", "Input.Jetpack.LeftPressed", "Input.Jetpack.RightPressed",
    "Joypad.Input.MoveForwardPressed", "Joypad.Input.MoveBackwardPressed", "Joypad.Input.JumpPressed",
};
constexpr const char* kFireBlocked[] = {"Input.FirePressed", "Input.FireUtilPressed", "Joypad.Input.FirePressed",
                                        "Joypad.Input.Fire.SecondPressed"};
// Weapons whose fire moves the worm (kNames at 0x90c920: 35, 36, 37, 55).
constexpr const char* kMovementWeapons[] = {"kUtilityNinjaRope", "kUtilityParachute", "kUtilityJetpack", "kMysteryTeleport"};
constexpr size_t kAlwaysN = sizeof kAlwaysBlocked / sizeof kAlwaysBlocked[0];
constexpr size_t kFireN = sizeof kFireBlocked / sizeof kFireBlocked[0];
melange::bus::MsgId g_always[kAlwaysN], g_fire[kFireN];
bool g_movementWeapon[kWeaponNameCount];

void ResolveIds() {
    const uint32_t serial = melange::sim::MatchSerial();
    if (serial == g_idsSerial) return;
    g_idsSerial = serial;
    for (size_t i = 0; i < kAlwaysN; ++i) g_always[i] = melange::bus::IdOf(kAlwaysBlocked[i]);
    for (size_t i = 0; i < kFireN; ++i) g_fire[i] = melange::bus::IdOf(kFireBlocked[i]);
    memset(g_movementWeapon, 0, sizeof g_movementWeapon);
    for (int i = 1; i < kWeaponNameCount; ++i) {
        char name[48] = "";
        melange::gamestate::detail::ReadCString(Rd<uintptr_t>(kWeaponNames + 4 * i), name, sizeof name);
        for (const char* m : kMovementWeapons)
            if (strcmp(name, m) == 0) g_movementWeapon[i] = true;
    }
}

bool Blocked(uint16_t id, int slot) {
    if (id == melange::bus::kInvalidId) return false;
    for (melange::bus::MsgId b : g_always)
        if (b == id) return true;
    for (melange::bus::MsgId b : g_fire) {
        if (b != id) continue;
        sw::WormInfo w;
        return g_backend.Read(slot, &w) && w.weapon >= 1 && w.weapon < kWeaponNameCount && g_movementWeapon[w.weapon];
    }
    return false;
}

// ---------------------------------------------------------------- hooks
int SlotOfEntity(uintptr_t e) {
    const uint8_t s = Rd<uint8_t>(e + kEntitySlot);
    return s < sw::kMaxWorms && Rd<uint8_t>(e + 0x45) ? s : -1;
}

int __stdcall HkUpdate(uintptr_t e, uint32_t t) {
    const int slot = SlotOfEntity(e);
    const bool anchored = slot >= 0 && sw::Anchored(slot);
    if (anchored) sw::Repin(slot);
    const int r = g_hUpdate.stdcall<int>(e, t);
    if (anchored) sw::Repin(slot);
    return r;
}

int __fastcall HkInput(uintptr_t e, void*, uintptr_t msg) {
    const int slot = SlotOfEntity(e);
    if (slot >= 0 && msg && sw::Pinned(slot)) {
        ResolveIds();
        const uint16_t id = Rd<uint16_t>(msg + 4);
        if (Blocked(id, slot)) {
            if (g_logInputs) LOG_INFO("[worms] slot %d: input %s swallowed (anchored)", slot, melange::bus::NameOf(id));
            return 1;
        }
    }
    return g_hInput.thiscall<int>(e, msg);
}

void __fastcall HkChangeState(uintptr_t e, void*, uintptr_t container, int state) {
    const int slot = SlotOfEntity(e);
    if (slot >= 0 && sw::Pinned(slot) && (state == sw::kBallistic || state == sw::kSliding)) {
        if (g_logInputs) LOG_INFO("[worms] slot %d: state %d refused (anchored)", slot, state);
        return;
    }
    g_hChangeState.thiscall<void>(e, container, state);
}

void Enable(SafetyHookInline& h, bool on, const char* what) {
    if (!h || h.enabled() == on) return;
    const bool ok = on ? h.enable().has_value() : h.disable().has_value();
    if (!ok) LOG_ERROR("[worms] %s the %s hook failed", on ? "enabling" : "disabling", what);
}

void SetHooks(bool on) {
    if (g_hooksOn == on) return;
    g_hooksOn = on;
    Enable(g_hUpdate, on, "worm Update");
    Enable(g_hInput, on, "worm input");
    Enable(g_hChangeState, on, "ChangeState");
}

void TickHook(uint32_t, void*) { sw::RepinAll(); }

void OnContext(bool created, melange::lua50::State*, void*) {
    if (created) {
        sw::ResetMatch();
        g_idsSerial = 0;
        SetHooks(true);
        return;
    }
    if (sw::AnchoredCount()) LOG_INFO("[worms] match VM closed: %d anchor(s) dropped", sw::AnchoredCount());
    sw::ResetMatch();
    SetHooks(false);
    if (g_tickHook) melange::sim::RemoveTickHook(g_tickHook);
    g_tickHook = 0;
}

// The sim tick hook (RepinAll after the script service's Update) exists only in a match that anchored a worm, so a
// match without one keeps the sim bridge's hooks as they were. Anchors come from sim scripts, so the bridge's Update
// hook is already on when the first one is set.
void OnAnchor(int slot, bool on, bool released, void*) {
    if (on) {
        float p[3] = {0, 0, 0};
        sw::PinOf(slot, p);
        LOG_INFO("[worms] slot %d anchored at (%.1f, %.1f, %.1f), tick %u", slot, p[0], p[1], p[2], melange::sim::Tick());
        if (!g_tickHook) g_tickHook = melange::sim::AddTickHook(&TickHook, nullptr, 10);
    } else if (released) {
        LOG_INFO("[worms] slot %d: pin released (dying or drowning), tick %u", slot, melange::sim::Tick());
    } else {
        LOG_INFO("[worms] slot %d: anchor cleared, tick %u", slot, melange::sim::Tick());
    }
}

void Contrib(melange::wormsign::Hasher& h, uint32_t, void*) {
    uint8_t b[sw::kHashBytes];
    h.Bytes(b, sw::HashBytes(b, sizeof b));
}

bool Check() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        using melange::mem::Expect;
        ok = melange::gamestate::Available() &&
             // WXWormLogicEntity::Update: push ebp; mov ebp,esp; and esp,-0x40; push -1; push handler
             Expect(kWormUpdate, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xc0, 0x6a, 0xff, 0x68, 0xde, 0x6d, 0x7d, 0x00}) &&
             // mov esi,[ebp+8] (this, stdcall); movzx ecx, byte [esi+0x30] (the worm index)
             Expect(0x5b1d93, {0x8b, 0x75, 0x08, 0x0f, 0xb6, 0x4e, 0x30}) &&
             // the input handler's prologue
             Expect(kWormInput, {0x6a, 0xff, 0x68, 0x48, 0x72, 0x7d, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00}) &&
             // ChangeState: prologue; mov esi,ecx (this); mov ecx,[edi+0xf0]; cmp 7; cmp 8; then
             // mov eax,[edi+0xec]; mov ebp,[esp+0x24] (the new state); test al,0x10; jz; cmp ebp,2; je; cmp ebp,3
             Expect(kChangeState, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xd8, 0x96, 0x7c, 0x00}) &&
             Expect(0x5aa80d, {0x8b, 0xf1, 0x8b, 0x8f, 0xf0, 0x00, 0x00, 0x00, 0x83, 0xf9, 0x07}) &&
             Expect(0x5aa827, {0x8b, 0x87, 0xec, 0x00, 0x00, 0x00, 0x8b, 0x6c, 0x24, 0x24, 0xa8, 0x10, 0x74, 0x12,
                               0x83, 0xfd, 0x02}) &&
             // DirtBall's nail: the worm index from the entity's +0x30, then the container's +0xec
             Expect(0x5ce37b, {0x8a, 0x46, 0x30, 0x8d, 0x4c, 0x24, 0x14, 0x0f, 0xb6, 0xd0, 0x51, 0x52, 0xe8}) &&
             Expect(0x5ce38c, {0x8b, 0x88, 0xec, 0x00, 0x00, 0x00});
        char name[64];
        bool payload = false;
        ok = ok && melange::gamestate::detail::Rtti(kWormVt, name, sizeof name, &payload) &&
             strcmp(name, "WormDataContainer") == 0;
        if (!ok) LOG_WARN("[worms] layout differs from build #1077: wum.sim.worm and the anchor are off");
    });
    return ok;
}

class WormAnchor final : public melange::Module {
public:
    const char* Name() const override { return "WormAnchor"; }
    const char* Description() const override { return "wum.sim.worm: worm state for sim scripts, and the anchor"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 54; }  // after SimBridge (52) and SimTweak (53)
    bool Install() override {
        g_logInputs = Bool("LogInputs", false);
        if (!Check()) return false;
        if (!melange::lua50::Check() || !melange::lua50::Track()) {
            LOG_WARN("[worms] match VM tracking unavailable: WormAnchor stays inert");
            return true;
        }
        using F = safetyhook::InlineHook::Flags;
        g_hUpdate = safetyhook::create_inline(kWormUpdate, &HkUpdate, F::StartDisabled);
        g_hInput = safetyhook::create_inline(kWormInput, &HkInput, F::StartDisabled);
        g_hChangeState = safetyhook::create_inline(kChangeState, &HkChangeState, F::StartDisabled);
        if (!g_hUpdate || !g_hInput || !g_hChangeState) {
            LOG_ERROR("[worms] creating the worm hooks failed: WormAnchor stays inert");
            g_hUpdate = {};
            g_hInput = {};
            g_hChangeState = {};
            return true;
        }
        sw::SetBackend(&g_backend);
        sw::SetAnchorObserver(&OnAnchor, nullptr);
        if (!sw::AddSimFunctions()) LOG_WARN("[worms] could not add every wum.sim worm function");
        melange::lua50::OnContext(&OnContext, nullptr);
        g_contrib = melange::wormsign::AddContributor("melange.wormanchor", &Contrib, nullptr);
        g_installed = true;
        LOG_INFO("[worms] installed: wum.sim.worm/worms/land, anchor hooks armed per match");
        return true;
    }
    void Uninstall() override {
        g_installed = false;
        SetHooks(false);
        sw::SetBackend(nullptr);
        sw::SetAnchorObserver(nullptr, nullptr);
        if (g_tickHook) melange::sim::RemoveTickHook(g_tickHook);
        if (g_contrib) melange::wormsign::RemoveContributor(g_contrib);
        g_hUpdate = {};
        g_hInput = {};
        g_hChangeState = {};
    }
};
}  // namespace

MELANGE_MODULE(WormAnchor);
