// SimTweak: in-memory weapon container overrides from sim mods (wum.sim.weapon).
#include "lua/sim/tweak.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "lua/sim/bridge_internal.h"
#include "weapons/engine.h"

namespace melange::tweak {
namespace {
Engine g_engine;
char g_str[kMaxString + 1];
char g_errbuf[320];
bool (*g_setAllowed)() = nullptr;

uintptr_t ResolveForRestore(const char* weapon, void*) { return weapons::Container(weapon); }

void OnMatchEvent(bool created, lua50::State*, void*) {
    if (created) return;
    const size_t pending = g_engine.PendingCount();
    if (pending == 0) return;
    g_engine.RestoreAll(&ResolveForRestore, nullptr);
    LOG_INFO("[tweak] match VM closed: restored up to %zu field(s)", pending);
}

bool IsNumber(FieldType t) { return t == FieldType::F32 || t == FieldType::I32 || t == FieldType::U8; }

TweakError Locate(const char* weapon, const char* field, uintptr_t* c, uint32_t* off, FieldType* t) {
    if (!weapon || !*weapon) return TweakError::UnknownWeapon;
    *c = weapons::Container(weapon);
    if (!*c) return TweakError::UnknownWeapon;
    if (!field || !*field) return TweakError::UnknownField;
    *t = weapons::Field(*c, field, off);
    if (*t == FieldType::None) return TweakError::UnknownField;
    return Supported(*t) ? TweakError::Ok : TweakError::Unsupported;
}

TweakError Check(FieldType t, const Value& v) {
    switch (t) {
        case FieldType::F32:
            if (!IsNumber(v.type)) return TweakError::TypeMismatch;
            return std::isfinite(v.num) ? TweakError::Ok : TweakError::OutOfRange;
        case FieldType::I32:
            if (!IsNumber(v.type)) return TweakError::TypeMismatch;
            return std::floor(v.num) == v.num && v.num >= -2147483648.0f && v.num < 2147483648.0f ? TweakError::Ok
                                                                                                   : TweakError::OutOfRange;
        case FieldType::U8:
            if (!IsNumber(v.type)) return TweakError::TypeMismatch;
            return std::floor(v.num) == v.num && v.num >= 0 && v.num <= 255 ? TweakError::Ok : TweakError::OutOfRange;
        case FieldType::Bool: return v.type == FieldType::Bool ? TweakError::Ok : TweakError::TypeMismatch;
        case FieldType::String:
            if (v.type != FieldType::String || !v.str) return TweakError::TypeMismatch;
            return std::strlen(v.str) <= kMaxString ? TweakError::Ok : TweakError::OutOfRange;
        default: return TweakError::Unsupported;
    }
}

uint32_t Encode(FieldType t, const Value& v) {
    uint32_t bits = 0;
    switch (t) {
        case FieldType::F32: std::memcpy(&bits, &v.num, 4); break;
        case FieldType::I32: bits = static_cast<uint32_t>(static_cast<int32_t>(v.num)); break;
        case FieldType::U8: bits = static_cast<uint8_t>(v.num); break;
        case FieldType::Bool: bits = v.b ? 1u : 0u; break;
        default: break;
    }
    return bits;
}

constexpr int UpvalueIndex(int i) { return lua50::kGlobals - i; }

// The field argument follows `self` when called as w:get(field), and comes first as w.get(field).
int FieldArg(lua50::State* L) { return lua50::A().type(L, 1) == lua50::kTTable ? 2 : 1; }

bool DoGet(const char* weapon, const char* field, Value* out) {
    const TweakError e = Get(weapon, field, out);
    if (e == TweakError::Ok) return true;
    std::snprintf(g_errbuf, sizeof g_errbuf, "weapon('%s'):get('%s'): %s", weapon, field, ToString(e));
    return false;
}

bool DoSet(const char* weapon, const char* field, const Value& v) {
    const TweakError e = Set(weapon, field, v);
    if (e == TweakError::Ok) return true;
    std::snprintf(g_errbuf, sizeof g_errbuf, "weapon('%s'):set('%s'): %s", weapon, field, ToString(e));
    return false;
}

int Raise(lua50::State* L, const char* text) {
    lua50::A().pushstring(L, text);
    lua50::A().error(L);
    return 0;
}

int LGet(lua50::State* L) {
    const auto& A = lua50::A();
    const int fi = FieldArg(L);
    if (A.type(L, fi) != lua50::kTString) return Raise(L, "weapon:get(field): field must be a string");
    const char* weapon = A.tostring(L, UpvalueIndex(1));
    Value v;
    if (!DoGet(weapon, A.tostring(L, fi), &v)) return Raise(L, g_errbuf);
    switch (v.type) {
        case FieldType::Bool: A.pushboolean(L, v.b); break;
        case FieldType::String: A.pushstring(L, v.str ? v.str : ""); break;
        default: A.pushnumber(L, v.num); break;
    }
    return 1;
}

int LSet(lua50::State* L) {
    const auto& A = lua50::A();
    const int fi = FieldArg(L);
    if (A.type(L, fi) != lua50::kTString) return Raise(L, "weapon:set(field, value): field must be a string");
    if (g_setAllowed && !g_setAllowed())
        return Raise(L, "weapon:set(field, value): only allowed in the sim script's top-level chunk at match start");
    Value v;
    const int vt = A.type(L, fi + 1);
    if (vt == lua50::kTNumber) {
        v.type = FieldType::F32;
        v.num = A.tonumber(L, fi + 1);
    } else if (vt == lua50::kTBoolean) {
        v.type = FieldType::Bool;
        v.b = A.toboolean(L, fi + 1) != 0;
    } else if (vt == lua50::kTString) {
        v.type = FieldType::String;
        v.str = A.tostring(L, fi + 1);
    } else {
        return Raise(L, "weapon:set(field, value): value must be a number, a boolean or a string");
    }
    const char* weapon = A.tostring(L, UpvalueIndex(1));
    if (!DoSet(weapon, A.tostring(L, fi), v)) return Raise(L, g_errbuf);
    A.pushboolean(L, 1);
    return 1;
}

int LWeapon(lua50::State* L) {
    const auto& A = lua50::A();
    if (A.type(L, 1) != lua50::kTString) return Raise(L, "wum.sim.weapon(name): name must be a string");
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

const char* ToString(TweakError e) {
    switch (e) {
        case TweakError::Ok: return "ok";
        case TweakError::UnknownWeapon: return "no weapon container by that name in this match";
        case TweakError::UnknownField: return "unknown field";
        case TweakError::Unsupported: return "field type not supported (f32, i32, u8, bool and string fields only)";
        case TweakError::TypeMismatch: return "value does not match the field's type";
        case TweakError::OutOfRange: return "value out of range for the field";
        case TweakError::WriteFailed: return "the field could not be written";
    }
    return "unknown error";
}

bool Supported(FieldType t) {
    return t == FieldType::F32 || t == FieldType::I32 || t == FieldType::U8 || t == FieldType::Bool ||
           t == FieldType::String;
}

size_t RawSize(FieldType t) {
    switch (t) {
        case FieldType::F32:
        case FieldType::I32:
        case FieldType::U32: return 4;
        case FieldType::U16: return 2;
        case FieldType::U8:
        case FieldType::Bool: return 1;
        default: return 0;
    }
}

bool Engine::Read(uintptr_t container, uint32_t offset, FieldType t, Value* out) const {
    if (!out || !container || !Supported(t)) return false;
    *out = Value{};
    out->type = t;
    const uintptr_t at = container + offset;
    if (t == FieldType::String) {
        uintptr_t p = 0;
        if (!mem::SafeRead(at, &p, sizeof p) || !p) return false;
        const std::string s = weapons::engine::XStringValue(at);
        std::snprintf(g_str, sizeof g_str, "%s", s.c_str());
        out->str = g_str;
        return true;
    }
    uint32_t bits = 0;
    if (!mem::SafeRead(at, &bits, RawSize(t))) return false;
    switch (t) {
        case FieldType::F32: std::memcpy(&out->num, &bits, 4); break;
        case FieldType::I32: out->num = static_cast<float>(static_cast<int32_t>(bits)); break;
        case FieldType::U8: out->num = static_cast<float>(bits & 0xff); break;
        case FieldType::Bool: out->b = (bits & 0xff) != 0; break;
        default: break;
    }
    return true;
}

bool Engine::Write(uintptr_t container, const std::string& weapon, uint32_t offset, FieldType t, const Value& v) {
    if (!container || !Supported(t) || Check(t, v) != TweakError::Ok) return false;
    const uintptr_t at = container + offset;
    bool known = false;
    for (auto& s : snapshots_)
        if (s.container == container && s.offset == offset) known = true;
    if (!known) {
        Snapshot s{weapon, container, offset, t, 0, {}};
        if (t == FieldType::String) {
            uintptr_t p = 0;
            if (!mem::SafeRead(at, &p, sizeof p) || !p) return false;
            s.text = weapons::engine::XStringValue(at);
        } else if (!mem::SafeRead(at, &s.bits, RawSize(t))) {
            return false;
        }
        snapshots_.push_back(std::move(s));
        touched_.push_back({weapon, container, offset, t});
    }
    if (t == FieldType::String) return weapons::engine::AssignXString(at, v.str);
    const uint32_t bits = Encode(t, v);
    return mem::Write(at, &bits, RawSize(t));
}

void Engine::RestoreAll(ResolveFn resolve, void* user) {
    for (auto& s : snapshots_) {
        const uintptr_t fresh = resolve ? resolve(s.weapon.c_str(), user) : 0;
        if (fresh && fresh == s.container) {
            const uintptr_t at = s.container + s.offset;
            if (s.type == FieldType::String)
                weapons::engine::AssignXString(at, s.text.c_str());
            else
                mem::Write(at, &s.bits, RawSize(s.type));
        } else {
            LOG_WARN("[tweak] skip restore for '%s' (+%#x): container changed or gone", s.weapon.c_str(),
                     static_cast<unsigned>(s.offset));
        }
    }
    snapshots_.clear();
    touched_.clear();
}

Engine& Instance() { return g_engine; }

TweakError Get(const char* weapon, const char* field, Value* value) {
    uintptr_t c = 0;
    uint32_t off = 0;
    FieldType t = FieldType::None;
    const TweakError e = Locate(weapon, field, &c, &off, &t);
    if (e != TweakError::Ok) return e;
    return Instance().Read(c, off, t, value) ? TweakError::Ok : TweakError::WriteFailed;
}

TweakError Set(const char* weapon, const char* field, const Value& v) {
    uintptr_t c = 0;
    uint32_t off = 0;
    FieldType t = FieldType::None;
    TweakError e = Locate(weapon, field, &c, &off, &t);
    if (e != TweakError::Ok) return e;
    e = Check(t, v);
    if (e != TweakError::Ok) return e;
    return Instance().Write(c, weapon, off, t, v) ? TweakError::Ok : TweakError::WriteFailed;
}

TweakError Get(const char* weapon, const char* field, float* value) {
    Value v;
    const TweakError e = Get(weapon, field, &v);
    if (e != TweakError::Ok) return e;
    if (v.type == FieldType::String) return TweakError::TypeMismatch;
    if (value) *value = v.type == FieldType::Bool ? (v.b ? 1.0f : 0.0f) : v.num;
    return TweakError::Ok;
}

TweakError Set(const char* weapon, const char* field, float value) {
    Value v;
    v.type = FieldType::F32;
    v.num = value;
    return Set(weapon, field, v);
}

uintptr_t FindContainer(const char* weaponName) { return weapons::Container(weaponName); }

void Init() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!lua50::Track()) {
        LOG_WARN("[tweak] match VM tracking unavailable: SimTweak stays inert");
        return;
    }
    lua50::OnContext(&OnMatchEvent, nullptr);
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
        melange::tweak::g_setAllowed = &melange::simbridge::InTopLevelChunk;
        if (!melange::simbridge::AddSimFunction("weapon", &melange::tweak::LWeapon))
            LOG_WARN("[tweak] could not add wum.sim.weapon");
        return true;
    }
};
}  // namespace

MELANGE_MODULE(SimTweak);
