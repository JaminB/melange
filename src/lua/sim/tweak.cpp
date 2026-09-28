// SimTweak: in-memory WEAPTWK field overrides from sim mods (component F, m2-design.md §3.F).
#include "lua/sim/tweak.h"

#include <cstdio>
#include <cstring>
#include <iterator>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"

namespace melange::tweak {
namespace {

// PayloadWeaponPropertiesContainer, src/xom/xom_schema.inc (firstField 959, 108 fields). Only the
// numeric (F32) fields are candidates for get/set; the rest are listed so a real-but-unsupported
// field name gets a precise error instead of looking like a typo.
constexpr SchemaField kPayloadFields[] = {
    {"IsAimedWeapon", false}, {"IsPoweredWeapon", false}, {"IsTargetingWeapon", false},
    {"IsControlledBomber", false}, {"IsBomberWeapon", false}, {"IsDirectionalWeapon", false},
    {"IsHoming", false}, {"IsLowGravity", false}, {"IsLaunchedFromWorm", false},
    {"HasAdjustableFuse", false}, {"HasAdjustableBounce", false}, {"HasAdjustableHerd", false},
    {"IsAffectedByGravity", false}, {"IsAffectedByWind", false}, {"EndTurnImmediate", false},
    {"UseParabolicRetical", false}, {"ColliderFlags", false}, {"CameraId", false},
    {"PayloadGraphicsResourceID", false}, {"Payload2ndGraphicsResourceID", false}, {"Scale", true},
    {"Radius", true}, {"AnimTravel", false}, {"AnimSmallJump", false}, {"AnimBigJump", false},
    {"AnimArm", false}, {"AnimSplashdown", false}, {"AnimSink", false}, {"AnimIntermediate", false},
    {"AnimImpact", false}, {"DirectionBlend", true}, {"FuseTimerGraphicOffset", true},
    {"FuseTimerScale", true}, {"BasePower", true}, {"MaxPower", true}, {"MinTerminalVelocity", true},
    {"MaxTerminalVelocity", true}, {"LogicalLaunchZOffset", true}, {"LogicalLaunchYOffset", true},
    {"OrientationOption", false}, {"SpinSpeed", true}, {"InterPayloadDelay", false},
    {"MinAimAngle", true}, {"MaxAimAngle", true}, {"DetonatesOnLandImpact", false},
    {"DetonatesOnExpiry", false}, {"DetonatesOnObjectImpact", false}, {"DetonatesOnWormImpact", false},
    {"DetonatesAtRest", false}, {"DetonatesOnFirePress", false}, {"DetonatesWhenCantJump", false},
    {"DetonateMultiEffect", false}, {"WormCollideResponse", false}, {"WormDamageMagnitude", true},
    {"ImpulseMagnitude", true}, {"WormDamageRadius", true}, {"LandDamageRadius", true},
    {"ImpulseRadius", true}, {"ImpulseOffset", true}, {"Mass", true}, {"WormImpactDamage", true},
    {"MaxPowerUp", false}, {"TangentialMinBounceDamping", true}, {"ParallelMinBounceDamping", true},
    {"TangentialMaxBounceDamping", true}, {"ParallelMaxBounceDamping", true}, {"SkimsOnWater", false},
    {"MinSpeedForSkim", true}, {"MaxAngleForSkim", true}, {"SkimDamping", false}, {"SinkDepth", true},
    {"NumStrikeBombs", false}, {"NumBomblets", false}, {"BombletMaxConeAngle", true},
    {"BombletMaxSpeed", true}, {"BombletMinSpeed", true}, {"BombletWeaponName", false},
    {"FxLocator", false}, {"ArielFx", false}, {"DetonationFx", false}, {"DetonationSfx", false},
    {"ExpiryFx", false}, {"SplashFx", false}, {"SplishFx", false}, {"SinkingFx", false},
    {"BounceFx", false}, {"StopFxAtRest", false}, {"BounceSfx", false}, {"PreDetonationSfx", false},
    {"ArmSfx1Shot", false}, {"ArmSfxLoop", false}, {"LaunchSfx", false}, {"LoopSfx", false},
    {"BigJumpSfx", false}, {"WalkSfx", false}, {"TrailBitmap", false}, {"TrailLocator1", false},
    {"TrailLocator2", false}, {"TrailLength", false}, {"AttachedMesh", false},
    {"AttachedMeshScale", true}, {"StartsArmed", false}, {"ArmOnImpact", false},
    {"ArmingCourtesyTime", false}, {"PreDetonationTime", false}, {"ArmingRadius", true},
    {"LifeTime", false}, {"IsFuseDisplayed", false},
};

constexpr WeaponClass kWeaponClasses[] = {
    {"kWeaponBazooka", "PayloadWeaponPropertiesContainer", kPayloadFields,
     static_cast<int>(std::size(kPayloadFields))},
};

// RE-verified byte offsets only (the F0/U6 spike, m2-design.md §2.7). Extending this to more
// fields or weapons needs the same runtime verification; do not guess an offset here.
constexpr VerifiedOffset kVerifiedOffsets[] = {
    {"PayloadWeaponPropertiesContainer", "WormDamageMagnitude", 0x15c},
};

constexpr uintptr_t kContainerLookup = 0x50b8b0;  // cdecl(const char** name, uintptr_t* out)

Engine g_engine;

uintptr_t ResolveForRestore(const char* weapon, void*) { return FindContainer(weapon); }

void OnMatchEvent(bool created, lua50::State*, void*) {
    if (created) return;  // nothing to do until a mod actually calls set()
    const size_t pending = g_engine.PendingCount();
    if (pending == 0) return;
    g_engine.RestoreAll(&ResolveForRestore, nullptr);
    LOG_INFO("[tweak] match VM closed: restored up to %zu field(s)", pending);
}

constexpr int UpvalueIndex(int i) { return lua50::kGlobals - i; }

thread_local char g_errbuf[256];

bool DoGet(const char* weapon, const char* field, float* out) {
    const TweakError e = Get(weapon, field, out);
    if (e == TweakError::Ok) return true;
    std::snprintf(g_errbuf, sizeof g_errbuf, "weapon('%s'):get('%s'): %s", weapon, field, ToString(e));
    return false;
}

bool DoSet(const char* weapon, const char* field, float value) {
    const TweakError e = Set(weapon, field, value);
    if (e == TweakError::Ok) return true;
    std::snprintf(g_errbuf, sizeof g_errbuf, "weapon('%s'):set('%s', %g): %s", weapon, field, value, ToString(e));
    return false;
}

int LGet(lua50::State* L) {
    const auto& A = lua50::A();
    if (!A.isstring(L, 2)) {
        A.pushstring(L, "weapon:get(field): field must be a string");
        A.error(L);
    }
    const char* weapon = A.tostring(L, UpvalueIndex(1));
    const char* field = A.tostring(L, 2);
    float value = 0;
    if (!DoGet(weapon, field, &value)) {
        A.pushstring(L, g_errbuf);
        A.error(L);
    }
    A.pushnumber(L, value);
    return 1;
}

int LSet(lua50::State* L) {
    const auto& A = lua50::A();
    if (!A.isstring(L, 2)) {
        A.pushstring(L, "weapon:set(field, value): field must be a string");
        A.error(L);
    }
    if (!A.isnumber(L, 3)) {
        A.pushstring(L, "weapon:set(field, value): value must be a number");
        A.error(L);
    }
    const char* weapon = A.tostring(L, UpvalueIndex(1));
    const char* field = A.tostring(L, 2);
    const float value = A.tonumber(L, 3);
    if (!DoSet(weapon, field, value)) {
        A.pushstring(L, g_errbuf);
        A.error(L);
    }
    A.pushboolean(L, 1);
    return 1;
}

int LWeapon(lua50::State* L) {
    const auto& A = lua50::A();
    if (!A.isstring(L, 1)) {
        A.pushstring(L, "wum.sim.weapon(name): name must be a string");
        A.error(L);
    }
    A.newtable(L);
    A.pushstring(L, "get");
    A.pushvalue(L, 1);
    A.pushcclosure(L, &LGet, 1);
    A.settable(L, -3);
    A.pushstring(L, "set");
    A.pushvalue(L, 1);
    A.pushcclosure(L, &LSet, 1);
    A.settable(L, -3);
    return 1;
}

}  // namespace

const WeaponClass* FindWeaponClass(const char* weapon) {
    if (!weapon) return nullptr;
    for (auto& w : kWeaponClasses)
        if (std::strcmp(w.weapon, weapon) == 0) return &w;
    return nullptr;
}

const VerifiedOffset* FindVerifiedOffset(const char* containerClass, const char* field) {
    if (!containerClass || !field) return nullptr;
    for (auto& v : kVerifiedOffsets)
        if (std::strcmp(v.containerClass, containerClass) == 0 && std::strcmp(v.field, field) == 0) return &v;
    return nullptr;
}

uintptr_t FindContainer(const char* weaponName) {
    if (!weaponName || !*weaponName || !game::IsKnownBuild()) return 0;
    // First bytes of 0x50b8b0 on build #1077 (the private F0/U6 spike verified this call).
    if (!mem::Expect(kContainerLookup, {0xe8, 0x68, 0xe2, 0x12, 0x00, 0x8b, 0x08})) return 0;
    uintptr_t obj = 0;
    reinterpret_cast<void(__cdecl*)(const char**, uintptr_t*)>(kContainerLookup)(&weaponName, &obj);
    return obj;
}

const char* ToString(TweakError e) {
    switch (e) {
        case TweakError::Ok: return "ok";
        case TweakError::UnknownWeapon: return "no verified schema mapping for this weapon yet";
        case TweakError::UnknownField: return "unknown field";
        case TweakError::NotNumeric: return "field is not numeric";
        case TweakError::NoVerifiedOffset: return "field has no verified memory offset yet";
        case TweakError::NoContainer: return "container not found (unknown build, or not loaded yet)";
    }
    return "unknown error";
}

bool Engine::ReadField(uintptr_t container, uint32_t offset, float* out) const {
    return out && mem::SafeRead(container + offset, out, sizeof(float));
}

bool Engine::WriteField(uintptr_t container, const std::string& weapon, uint32_t offset, float value) {
    bool known = false;
    for (auto& s : snapshots_)
        if (s.container == container && s.offset == offset) known = true;
    if (!known) {
        uint32_t originalBits = 0;
        if (!mem::SafeRead(container + offset, &originalBits, sizeof originalBits)) return false;
        snapshots_.push_back({weapon, offset, container, originalBits});
    }
    return mem::Put<float>(container + offset, value);
}

void Engine::RestoreAll(ResolveFn resolve, void* user) {
    for (auto& s : snapshots_) {
        const uintptr_t fresh = resolve ? resolve(s.weapon.c_str(), user) : 0;
        if (fresh && fresh == s.container) {
            mem::Put<uint32_t>(s.container + s.offset, s.originalBits);
        } else {
            LOG_WARN("[tweak] skip restore for '%s' (+%#x): container changed or gone", s.weapon.c_str(),
                     static_cast<unsigned>(s.offset));
        }
    }
    snapshots_.clear();
}

Engine& Instance() { return g_engine; }

TweakError Get(const char* weapon, const char* field, float* value) {
    const WeaponClass* w = FindWeaponClass(weapon);
    if (!w) return TweakError::UnknownWeapon;
    const SchemaField* f = nullptr;
    for (int i = 0; i < w->fieldCount; ++i)
        if (std::strcmp(w->fields[i].name, field) == 0) f = &w->fields[i];
    if (!f) return TweakError::UnknownField;
    if (!f->numeric) return TweakError::NotNumeric;
    const VerifiedOffset* v = FindVerifiedOffset(w->containerClass, field);
    if (!v) return TweakError::NoVerifiedOffset;
    const uintptr_t container = FindContainer(weapon);
    if (!container) return TweakError::NoContainer;
    if (!Instance().ReadField(container, v->offset, value)) return TweakError::NoContainer;
    return TweakError::Ok;
}

TweakError Set(const char* weapon, const char* field, float value) {
    const WeaponClass* w = FindWeaponClass(weapon);
    if (!w) return TweakError::UnknownWeapon;
    const SchemaField* f = nullptr;
    for (int i = 0; i < w->fieldCount; ++i)
        if (std::strcmp(w->fields[i].name, field) == 0) f = &w->fields[i];
    if (!f) return TweakError::UnknownField;
    if (!f->numeric) return TweakError::NotNumeric;
    const VerifiedOffset* v = FindVerifiedOffset(w->containerClass, field);
    if (!v) return TweakError::NoVerifiedOffset;
    const uintptr_t container = FindContainer(weapon);
    if (!container) return TweakError::NoContainer;
    if (!Instance().WriteField(container, weapon, v->offset, value)) return TweakError::NoContainer;
    return TweakError::Ok;
}

void Init() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!lua50::Track()) {
        LOG_WARN("[tweak] match VM tracking unavailable: SimTweak stays inert");
        return;
    }
    lua50::OnContext(&OnMatchEvent, nullptr);  // never removed: SimTweak lives for the process
}

int OpenLibrary(lua50::State* L) {
    lua50::A().pushcclosure(L, &LWeapon, 0);
    return 1;
}

}  // namespace melange::tweak

namespace {
class SimTweak final : public melange::Module {
public:
    const char* Name() const override { return "SimTweak"; }
    const char* Description() const override { return "weapon data tweaks from sim mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 53; }
    bool Install() override {
        if (!melange::lua50::Check()) {
            LOG_WARN("[tweak] engine Lua check failed: SimTweak stays inert");
            return true;
        }
        melange::tweak::Init();
        return true;
    }
};
}  // namespace

MELANGE_MODULE(SimTweak);
