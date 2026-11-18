// Melee: the engine side of wum.sim.melee (src/lua/sim/sim_melee.cpp) for build #1077. Notes:
// docs/gravespade-re-input.md.
//
// The four melee weapons (Bat, Fire Punch, Prod, No More Nails) run on MeleeWeaponLogicEntity (vtable 0x85a82c):
// - HandleMessage 0x569930 (slot 7, stdcall(this, msg), ret 8): Input.FirePressed starts the swing when the entity is
//   idle (state +0x50 == 0); Weapon.Delete and message 0x42 end it.
// - Update 0x569b00 (slot 6, stdcall(this, time), ret 8): the state machine. State 0 runs the idle sweep that writes
//   Weapon.Melee.TargetInRange; state 2 waits LaunchDelay, posts Timer.EndTurn, then strikes; 8 returns to 0 after
//   PostLaunchDelay and posts Weapon.Fired and the retreat timer.
// - The per-target impact 0x567a40 (thiscall(this), ret): once per worm caught by the strike sweep; the target is
//   hit record u16[0x95c2a0] (0x5160e0(index), +0x14 flags, bit 0 a worm, +0x18 its task id; the task's logic entity
//   from 0x68e4ca, its worm index at +0x30). It builds the DamageImpulseMessage from the properties container
//   P = *(this+0x58): WormDamageMagnitude, ImpulseMagnitude, DamageIsPercentage, InstantKill.
// The weapon only gets fire input through the active worm's input handler 0x5ac6e0 (forwarded via the worm's
// sub-entity), so swallowing Input.FirePressed/FireReleased there (WormAnchor's hook, simworms::SetInputFilter) stops
// the swing entirely: no ammo, no Timer.EndTurn, no Weapon.Fired. That is the dig.
//
// Every hook is created disabled and is on only in a match where a sim mod claimed a weapon, so vanilla play and
// matches without wum.sim.melee run the engine's code untouched.
#include <windows.h>
#include <safetyhook.hpp>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "game/state/gamestate_internal.h"
#include "gameplay/engine_data.h"
#include "lua/engine50.h"
#include "lua/sim/sim_melee.h"
#include "lua/sim/sim_worms.h"
#include "melange/bus.h"
#include "melange/gamestate.h"
#include "melange/sim.h"
#include "melange/weapons.h"
#include "melange/wormsign.h"

namespace {
namespace sm = melange::simmelee;
namespace sw = melange::simworms;
namespace gd = melange::gameplay::data;
using melange::gamestate::detail::Rd;

constexpr uintptr_t kMeleeVt = 0x85a82c, kMsg = 0x569930, kUpdate = 0x569b00, kImpact = 0x567a40;
constexpr uintptr_t kRecord = 0x5160e0, kTaskEntity = 0x68e4ca, kRecordIndex = 0x95c2a0, kActiveWorm = 0x95fb98;
constexpr uintptr_t kWeaponNames = 0x90c920, kBaseMessageVt = 0x81aa14;
constexpr int kWeaponNameCount = 0x45;
constexpr uint32_t kState = 0x50, kProps = 0x58;
constexpr const char* kMeleeNames[] = {"kWeaponBaseballBat", "kWeaponFirePunch", "kWeaponProd", "kWeaponNoMoreNails"};

SafetyHookInline g_hMsg, g_hUpdate, g_hImpact;
bool g_ready = false, g_on = false, g_log = false;
int g_context = 0, g_contrib = 0, g_tick = 0;
uintptr_t g_live = 0;  // the wielded melee entity (main thread)
uint32_t g_idSerial = ~0u;
melange::bus::MsgId g_press[2], g_release[2], g_delete = melange::bus::kInvalidId;

int SlotOfEntity(uintptr_t e) {
    if (!e) return -1;
    const uint8_t s = Rd<uint8_t>(e + 0x30);
    return s < sw::kMaxWorms && Rd<uint8_t>(e + 0x45) ? s : -1;
}

void ResolveIds() {
    const uint32_t serial = melange::sim::MatchSerial();
    if (serial == g_idSerial) return;
    g_idSerial = serial;
    g_press[0] = melange::bus::IdOf("Input.FirePressed");
    g_press[1] = melange::bus::IdOf("Joypad.Input.FirePressed");
    g_release[0] = melange::bus::IdOf("Input.FireReleased");
    g_release[1] = melange::bus::IdOf("Joypad.Input.FireReleased");
    g_delete = melange::bus::IdOf("Weapon.Delete");
}

bool IsId(const melange::bus::MsgId (&set)[2], uint16_t id) {
    return id != melange::bus::kInvalidId && (set[0] == id || set[1] == id);
}

bool LiveOk() { return g_live && Rd<uintptr_t>(g_live) == kMeleeVt; }

class EngineMelee final : public sm::Backend {
public:
    int WeaponIndex(const char* name) override {
        bool melee = false;
        for (const char* m : kMeleeNames) melee = melee || std::strcmp(m, name) == 0;
        if (!melee) return -1;
        for (int i = 1; i < kWeaponNameCount; ++i) {
            char n[48] = "";
            melange::gamestate::detail::ReadCString(Rd<uintptr_t>(kWeaponNames + 4 * i), n, sizeof n);
            if (std::strcmp(n, name) == 0) return i;
        }
        return -1;
    }
    int ActiveSlot() override { return SlotOfEntity(Rd<uintptr_t>(kActiveWorm)); }
    int WeaponOf(int slot) override {
        sw::WormInfo w;
        sw::Backend* b = sw::GetBackend();
        return b && slot >= 0 && b->Read(slot, &w) ? w.weapon : -1;
    }
    bool TargetInRange() override { return gd::Flag("Weapon.Melee.TargetInRange"); }
    int EntityState() override { return LiveOk() ? static_cast<int>(Rd<uint32_t>(g_live + kState)) : -1; }
    const char* Strike() override;
};
EngineMelee g_backend;

bool CallMsg(uintptr_t e, uint8_t* msg) {
    __try {
        g_hMsg.stdcall<uintptr_t>(e, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A synthetic Input.FirePressed straight to the wielded entity: the engine's own swing start (it checks the
// can-fire rules and the idle state itself).
const char* EngineMelee::Strike() {
    ResolveIds();
    if (!LiveOk() || g_press[0] == melange::bus::kInvalidId) return "not wielded";
    if (Rd<uint32_t>(g_live + kState) != 0) return "busy";
    alignas(4) uint8_t msg[16] = {};
    const uint32_t vt = kBaseMessageVt;
    memcpy(msg, &vt, 4);
    memcpy(msg + 4, &g_press[0], 2);
    if (!CallMsg(g_live, msg)) return "busy";
    if (Rd<uint32_t>(g_live + kState) == 0) return "busy";
    if (g_log) LOG_INFO("[melee] scripted strike, tick %u", melange::sim::Tick());
    return nullptr;
}

// The gate: the worm input hook's filter. Only the active worm's inputs reach the weapon.
bool Filter(uintptr_t entity, int slot, uint16_t id) {
    if (!g_on || entity != Rd<uintptr_t>(kActiveWorm)) return false;
    ResolveIds();
    const bool press = IsId(g_press, id), release = !press && IsId(g_release, id);
    if (!press && !release) return false;
    const bool swallow = sm::OnFire(slot, press);
    if (g_log)
        LOG_INFO("[melee] slot %d: %s %s, tick %u", slot, press ? "press" : "release", swallow ? "swallowed (dig)" : "passed",
                 melange::sim::Tick());
    return swallow;
}

uintptr_t __stdcall HkMsg(uintptr_t self, uintptr_t msg) {
    if (msg && self == g_live) {
        ResolveIds();
        const uint16_t id = Rd<uint16_t>(msg + 4);
        if (id == 0x42 || (g_delete != melange::bus::kInvalidId && id == g_delete)) g_live = 0;
    }
    return g_hMsg.stdcall<uintptr_t>(self, msg);
}

uint32_t __stdcall HkUpdate(uintptr_t self, uint32_t t) {
    g_live = self;
    const uint32_t before = Rd<uint32_t>(self + kState);
    const uint32_t r = g_hUpdate.stdcall<uint32_t>(self, t);
    const uint32_t after = Rd<uint32_t>(self + kState);
    if (before != after) {
        const int slot = g_backend.ActiveSlot();
        if (before == 2) sm::OnStrike(slot);
        if (after == 0) sm::OnDone(slot);
    }
    return r;
}

uintptr_t RecordAt(uint16_t i) {
    __try {
        return reinterpret_cast<uintptr_t(__cdecl*)(uint32_t)>(kRecord)(i);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uintptr_t EntityOfTask(uint32_t task) {
    __try {
        return reinterpret_cast<uintptr_t(__cdecl*)(uint32_t)>(kTaskEntity)(task);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct Offsets {
    uintptr_t props = 0;
    uint32_t damage = 0, impulse = 0, percent = 0, instant = 0;
    bool ok = false;
};
Offsets g_off;

bool Resolve(uintptr_t props) {
    if (g_off.props == props) return g_off.ok;
    g_off = Offsets{};
    g_off.props = props;
    using melange::weapons::FieldType;
    const bool ok = melange::weapons::Field(props, "WormDamageMagnitude", &g_off.damage) == FieldType::F32 &&
                    melange::weapons::Field(props, "ImpulseMagnitude", &g_off.impulse) == FieldType::F32 &&
                    melange::weapons::Field(props, "DamageIsPercentage", &g_off.percent) == FieldType::Bool &&
                    melange::weapons::Field(props, "InstantKill", &g_off.instant) == FieldType::Bool;
    g_off.ok = ok;
    if (!ok) LOG_WARN("[melee] the melee damage fields did not resolve: hit overrides other than cancel are ignored");
    return ok;
}

bool Poke(uintptr_t a, const void* v, size_t n) {
    __try {
        memcpy(reinterpret_cast<void*>(a), v, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall HkImpact(uintptr_t self, void*) {
    const int attacker = g_backend.ActiveSlot();
    if (!g_on || attacker < 0 || !sm::Claimed(g_backend.WeaponOf(attacker))) return g_hImpact.thiscall<void>(self);
    const uintptr_t rec = RecordAt(Rd<uint16_t>(kRecordIndex));
    if (!rec || !(Rd<uint32_t>(rec + 0x14) & 1)) return g_hImpact.thiscall<void>(self);
    const int victim = SlotOfEntity(EntityOfTask(Rd<uint32_t>(rec + 0x18)));
    if (victim < 0 || victim == attacker) return g_hImpact.thiscall<void>(self);
    float pos[3] = {};
    melange::gamestate::detail::Copy(rec, pos, 12);
    const uintptr_t props = Rd<uintptr_t>(self + kProps);
    const bool fields = props && Resolve(props);
    const float damage = fields ? Rd<float>(props + g_off.damage) : 0.f;
    const float impulse = fields ? Rd<float>(props + g_off.impulse) : 0.f;
    const sm::HitResult r = sm::OnHit(attacker, victim, pos, damage, impulse);
    if (g_log)
        LOG_INFO("[melee] hit %d -> %d at (%.1f, %.1f, %.1f): cancel %d kill %d damage %s%.1f impulse %s%.2f, tick %u",
                 attacker, victim, pos[0], pos[1], pos[2], r.cancel, r.kill, r.setDamage ? "=" : "~",
                 r.setDamage ? r.damage : damage, r.setImpulse ? "=" : "~", r.setImpulse ? r.impulse : impulse,
                 melange::sim::Tick());
    if (r.cancel) return;
    if (!fields || (!r.kill && !r.setDamage && !r.setImpulse)) return g_hImpact.thiscall<void>(self);
    uint8_t saved[4][4];
    const uint32_t offs[4] = {g_off.damage, g_off.impulse, g_off.percent, g_off.instant};
    for (int i = 0; i < 4; ++i) melange::gamestate::detail::Copy(props + offs[i], saved[i], i < 2 ? 4 : 1);
    if (r.setDamage) Poke(props + g_off.damage, &r.damage, 4);
    if (r.setImpulse) Poke(props + g_off.impulse, &r.impulse, 4);
    const uint8_t zero = 0, one = 1;
    if (r.setDamage || r.kill) Poke(props + g_off.percent, &zero, 1);
    if (r.kill) Poke(props + g_off.instant, &one, 1);
    else if (r.setDamage) Poke(props + g_off.instant, &zero, 1);
    g_hImpact.thiscall<void>(self);
    for (int i = 0; i < 4; ++i) Poke(props + offs[i], saved[i], i < 2 ? 4 : 1);
}

void Enable(SafetyHookInline& h, bool on, const char* what) {
    if (!h || h.enabled() == on) return;
    const bool ok = on ? h.enable().has_value() : h.disable().has_value();
    if (!ok) LOG_ERROR("[melee] %s the %s hook failed", on ? "enabling" : "disabling", what);
}

void TickHook(uint32_t, void*) { sm::Tick(); }

void SetOn(bool on) {
    if (g_on == on) return;
    g_on = on;
    Enable(g_hMsg, on, "melee message");
    Enable(g_hUpdate, on, "melee update");
    Enable(g_hImpact, on, "melee impact");
    if (on && !g_tick) g_tick = melange::sim::AddTickHook(&TickHook, nullptr, 12);
    if (!on && g_tick) {
        melange::sim::RemoveTickHook(g_tick);
        g_tick = 0;
    }
    g_live = 0;
}

void OnClaim(int claims, void*) {
    if (claims > 0 && g_ready) {
        if (!g_on) LOG_INFO("[melee] a sim mod claimed a melee weapon: gate and hit hooks on for this match");
        SetOn(true);
    }
}

void OnContext(bool created, melange::lua50::State*, void*) {
    if (!created && g_on) LOG_INFO("[melee] match end: %u hit(s) seen", sm::HitCount());
    sm::ResetMatch();
    SetOn(false);
    g_idSerial = ~0u;
}

void Contrib(melange::wormsign::Hasher& h, uint32_t, void*) {
    uint8_t b[sm::kHashBytes];
    h.Bytes(b, sm::HashBytes(b, sizeof b));
}

bool Check() {
    using melange::mem::Expect;
    return Expect(kMsg, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0xeb, 0x47, 0x7d, 0x00}) &&
           Expect(kUpdate, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x3a, 0x48, 0x7d, 0x00}) &&
           Expect(kImpact, {0x6a, 0xff, 0x68, 0x0b, 0x47, 0x7d, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00}) &&
           // the vtable's Update and HandleMessage slots (6 and 7)
           Expect(kMeleeVt + 0x18, {0x00, 0x9b, 0x56, 0x00, 0x30, 0x99, 0x56, 0x00}) &&
           // the impact's record lookups: movzx edx, word [0x95c2a0]; push edx; call 0x5160e0 / the InstantKill path's
           Expect(0x567a95, {0x0f, 0xb7, 0x15, 0xa0, 0xc2, 0x95, 0x00, 0x52, 0xe8}) &&
           Expect(0x567c0c, {0x0f, 0xb7, 0x05, 0xa0, 0xc2, 0x95, 0x00, 0x50, 0xe8}) &&
           Expect(0x567c19, {0x8b, 0x40, 0x18, 0x50, 0xe8}) &&
           // the record helper: mov eax,[0x95c2f0]; sub eax,[0x95c2ec]
           Expect(kRecord, {0xa1, 0xf0, 0xc2, 0x95, 0x00, 0x2b, 0x05, 0xec, 0xc2, 0x95, 0x00}) &&
           Expect(kTaskEntity, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x14}) &&
           // HandleMessage's FirePressed test against the state at +0x50
           Expect(0x569991, {0x83, 0x7f, 0x50, 0x00});
}

class Melee final : public melange::Module {
public:
    const char* Name() const override { return "Melee"; }
    const char* Description() const override { return "wum.sim.melee: melee fire gate, hit events and overrides for sim mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 56; }  // after WormAnchor (54): its input hook carries the gate
    bool Install() override {
        g_log = Bool("Log", false);
        sm::SetBackend(nullptr);
        if (!melange::gamestate::Available() || !Check() || !melange::lua50::Check() || !melange::lua50::Track()) {
            LOG_WARN("[melee] layout differs from build #1077 or no match VM tracking: wum.sim.melee answers \"unavailable\"");
            return true;
        }
        using F = safetyhook::InlineHook::Flags;
        g_hMsg = safetyhook::create_inline(kMsg, &HkMsg, F::StartDisabled);
        g_hUpdate = safetyhook::create_inline(kUpdate, &HkUpdate, F::StartDisabled);
        g_hImpact = safetyhook::create_inline(kImpact, &HkImpact, F::StartDisabled);
        if (!g_hMsg || !g_hUpdate || !g_hImpact) {
            LOG_ERROR("[melee] creating the melee hooks failed: wum.sim.melee answers \"unavailable\"");
            g_hMsg = {};
            g_hUpdate = {};
            g_hImpact = {};
            return true;
        }
        g_ready = true;
        sm::SetBackend(&g_backend);
        sm::SetClaimObserver(&OnClaim, nullptr);
        sw::SetInputFilter(&Filter);
        g_context = melange::lua50::OnContext(&OnContext, nullptr);
        g_contrib = melange::wormsign::AddContributor("melange.melee", &Contrib, nullptr);
        LOG_INFO("[melee] installed: wum.sim.melee, hooks armed in matches with a claim");
        return true;
    }
    void Uninstall() override {
        SetOn(false);
        g_ready = false;
        sw::SetInputFilter(nullptr);
        sm::SetBackend(nullptr);
        sm::SetClaimObserver(nullptr, nullptr);
        if (g_context) melange::lua50::RemoveOnContext(g_context);
        if (g_contrib) melange::wormsign::RemoveContributor(g_contrib);
        g_context = g_contrib = 0;
        g_hMsg = {};
        g_hUpdate = {};
        g_hImpact = {};
    }
};
}  // namespace

MELANGE_MODULE(Melee);
