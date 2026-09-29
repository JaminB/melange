// Offline self-test for src/lua/sim/tweak.*: field lookup by schema name through weapons::Container/Field, typed
// get/set (f32, i32, u8, bool, string), value checks, and snapshot/restore. The weapons layer and the XString helpers
// are replaced below by fakes over in-process "containers".
#include "lua/sim/tweak.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>

#include "weapons/engine.h"

using namespace melange::tweak;
using melange::weapons::FieldType;

namespace {
struct alignas(8) FakeContainer {
    unsigned char bytes[64];
};
FakeContainer g_bazooka, g_clone, g_other;
std::deque<std::string> g_strings;

uintptr_t Addr(FakeContainer& c) { return reinterpret_cast<uintptr_t>(c.bytes); }

struct FakeField {
    const char* name;
    FieldType type;
    uint32_t offset;
};
constexpr FakeField kFields[] = {
    {"WormDamageMagnitude", FieldType::F32, 0x00}, {"LifeTime", FieldType::I32, 0x04},
    {"IsHoming", FieldType::Bool, 0x08},           {"NumBomblets", FieldType::U8, 0x09},
    {"ColliderFlags", FieldType::U16, 0x0a},       {"MaxPowerUp", FieldType::U32, 0x0c},
    {"PayloadGraphicsResourceID", FieldType::String, 0x10},
};

void SetText(uintptr_t field, const char* s) {
    g_strings.emplace_back(s);
    const uintptr_t p = reinterpret_cast<uintptr_t>(g_strings.back().c_str());
    std::memcpy(reinterpret_cast<void*>(field), &p, sizeof p);
}

void Fill(FakeContainer& c, float dmg, const char* mesh) {
    std::memset(c.bytes, 0, sizeof c.bytes);
    std::memcpy(c.bytes, &dmg, 4);
    const int32_t life = 5000;
    std::memcpy(c.bytes + 4, &life, 4);
    c.bytes[8] = 1;
    c.bytes[9] = 3;
    SetText(Addr(c) + 0x10, mesh);
}

int g_pass = 0, g_fail = 0;
void Check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
        return;
    }
    ++g_fail;
    std::printf("FAIL: %s\n", what);
}
void CheckError(TweakError got, TweakError want, const char* what) {
    Check(got == want, what);
    if (got != want) std::printf("  got %s, want %s\n", ToString(got), ToString(want));
}

Value Num(float f) {
    Value v;
    v.type = FieldType::F32;
    v.num = f;
    return v;
}
Value Bool(bool b) {
    Value v;
    v.type = FieldType::Bool;
    v.b = b;
    return v;
}
Value Str(const char* s) {
    Value v;
    v.type = FieldType::String;
    v.str = s;
    return v;
}

float GetF(const char* w, const char* f) {
    Value v;
    return Get(w, f, &v) == TweakError::Ok ? v.num : NAN;
}
std::string GetS(const char* w, const char* f) {
    Value v;
    return Get(w, f, &v) == TweakError::Ok && v.str ? v.str : "<error>";
}

uintptr_t ResolveSame(const char* weapon, void*) { return melange::weapons::Container(weapon); }
uintptr_t ResolveNone(const char*, void*) { return 0; }
}  // namespace

namespace melange::weapons {
uintptr_t Container(const char* name) {
    if (!name) return 0;
    if (std::strcmp(name, "kWeaponBazooka") == 0) return Addr(g_bazooka);
    if (std::strcmp(name, "kWeaponMegaBazooka") == 0) return Addr(g_clone);
    return 0;
}
FieldType Field(uintptr_t container, const char* field, uint32_t* offset) {
    if (!container || !field) return FieldType::None;
    for (auto& f : kFields)
        if (std::strcmp(f.name, field) == 0) {
            if (offset) *offset = f.offset;
            return f.type;
        }
    return FieldType::None;
}
namespace engine {
std::string XStringValue(uintptr_t field) {
    uintptr_t p = 0;
    std::memcpy(&p, reinterpret_cast<void*>(field), sizeof p);
    return p ? reinterpret_cast<const char*>(p) : "";
}
bool AssignXString(uintptr_t field, const char* s) {
    uintptr_t p = 0;
    std::memcpy(&p, reinterpret_cast<void*>(field), sizeof p);
    if (!p || !s) return false;
    SetText(field, s);
    return true;
}
}  // namespace engine
}  // namespace melange::weapons

namespace melange::simbridge {
bool AddSimFunction(const char*, lua50::CFunction) { return true; }
bool InTopLevelChunk() { return true; }
}  // namespace melange::simbridge

int main() {
    Fill(g_bazooka, 50.0f, "Bazooka.Payload");
    Fill(g_clone, 120.0f, "Bazooka.Payload");
    Instance().Reset();

    Check(Supported(FieldType::F32) && Supported(FieldType::I32) && Supported(FieldType::U8) &&
              Supported(FieldType::Bool) && Supported(FieldType::String),
          "the five v1 types are supported");
    Check(!Supported(FieldType::U16) && !Supported(FieldType::U32) && !Supported(FieldType::None),
          "u16, u32 and none are not");
    Check(RawSize(FieldType::F32) == 4 && RawSize(FieldType::I32) == 4 && RawSize(FieldType::U8) == 1 &&
              RawSize(FieldType::Bool) == 1 && RawSize(FieldType::String) == 0,
          "raw sizes");
    for (int e = 0; e <= static_cast<int>(TweakError::WriteFailed); ++e) {
        const char* s = ToString(static_cast<TweakError>(e));
        Check(s && *s, "ToString covers every TweakError");
    }

    // --- lookup errors ---
    Value v;
    CheckError(Get("kWeaponNoSuch", "WormDamageMagnitude", &v), TweakError::UnknownWeapon, "unknown weapon");
    CheckError(Get(nullptr, "WormDamageMagnitude", &v), TweakError::UnknownWeapon, "null weapon");
    CheckError(Get("kWeaponBazooka", "NoSuchField", &v), TweakError::UnknownField, "unknown field");
    CheckError(Get("kWeaponBazooka", "", &v), TweakError::UnknownField, "empty field");
    CheckError(Get("kWeaponBazooka", "ColliderFlags", &v), TweakError::Unsupported, "u16 field not supported");
    CheckError(Set("kWeaponBazooka", "MaxPowerUp", Num(1)), TweakError::Unsupported, "u32 field not supported");
    CheckError(Set("kWeaponNoSuch", "WormDamageMagnitude", Num(1)), TweakError::UnknownWeapon, "set: unknown weapon");

    // --- typed reads ---
    Check(GetF("kWeaponBazooka", "WormDamageMagnitude") == 50.0f, "f32 read");
    Check(GetF("kWeaponBazooka", "LifeTime") == 5000.0f, "i32 read");
    Check(GetF("kWeaponBazooka", "NumBomblets") == 3.0f, "u8 read");
    Check(Get("kWeaponBazooka", "IsHoming", &v) == TweakError::Ok && v.type == FieldType::Bool && v.b, "bool read");
    Check(GetS("kWeaponBazooka", "PayloadGraphicsResourceID") == "Bazooka.Payload", "string read");
    Check(GetF("kWeaponMegaBazooka", "WormDamageMagnitude") == 120.0f, "a clone name resolves");

    // --- value checks ---
    CheckError(Set("kWeaponBazooka", "WormDamageMagnitude", Str("x")), TweakError::TypeMismatch, "string into f32");
    CheckError(Set("kWeaponBazooka", "WormDamageMagnitude", Bool(true)), TweakError::TypeMismatch, "bool into f32");
    CheckError(Set("kWeaponBazooka", "WormDamageMagnitude", Num(NAN)), TweakError::OutOfRange, "nan into f32");
    CheckError(Set("kWeaponBazooka", "WormDamageMagnitude", Num(INFINITY)), TweakError::OutOfRange, "inf into f32");
    CheckError(Set("kWeaponBazooka", "LifeTime", Num(1.5f)), TweakError::OutOfRange, "fraction into i32");
    CheckError(Set("kWeaponBazooka", "LifeTime", Num(3e9f)), TweakError::OutOfRange, "i32 overflow");
    CheckError(Set("kWeaponBazooka", "NumBomblets", Num(256)), TweakError::OutOfRange, "u8 overflow");
    CheckError(Set("kWeaponBazooka", "NumBomblets", Num(-1)), TweakError::OutOfRange, "negative u8");
    CheckError(Set("kWeaponBazooka", "IsHoming", Num(1)), TweakError::TypeMismatch, "number into bool");
    CheckError(Set("kWeaponBazooka", "PayloadGraphicsResourceID", Num(1)), TweakError::TypeMismatch,
               "number into string");
    const std::string longText(kMaxString + 1, 'a');
    CheckError(Set("kWeaponBazooka", "PayloadGraphicsResourceID", Str(longText.c_str())), TweakError::OutOfRange,
               "string over the cap");
    Check(Instance().PendingCount() == 0, "refused writes leave nothing pending");
    Check(GetF("kWeaponBazooka", "WormDamageMagnitude") == 50.0f, "refused writes leave the value");

    // --- writes ---
    CheckError(Set("kWeaponBazooka", "WormDamageMagnitude", Num(75)), TweakError::Ok, "f32 write");
    CheckError(Set("kWeaponBazooka", "WormDamageMagnitude", Num(90)), TweakError::Ok, "second f32 write");
    CheckError(Set("kWeaponBazooka", "LifeTime", Num(-7)), TweakError::Ok, "i32 write");
    CheckError(Set("kWeaponBazooka", "NumBomblets", Num(255)), TweakError::Ok, "u8 write");
    CheckError(Set("kWeaponBazooka", "IsHoming", Bool(false)), TweakError::Ok, "bool write");
    CheckError(Set("kWeaponBazooka", "PayloadGraphicsResourceID", Str("Cow.Payload")), TweakError::Ok, "string write");
    CheckError(Set("kWeaponMegaBazooka", "LandDamageRadius", Num(90)), TweakError::UnknownField,
               "an unknown field on a clone");
    CheckError(Set("kWeaponMegaBazooka", "WormDamageMagnitude", Num(240)), TweakError::Ok, "a clone field write");
    Check(GetF("kWeaponBazooka", "WormDamageMagnitude") == 90.0f, "f32 reads back");
    Check(GetF("kWeaponBazooka", "LifeTime") == -7.0f, "i32 reads back");
    Check(GetF("kWeaponBazooka", "NumBomblets") == 255.0f, "u8 reads back");
    Check(Get("kWeaponBazooka", "IsHoming", &v) == TweakError::Ok && !v.b, "bool reads back");
    Check(g_bazooka.bytes[8] == 0 && g_bazooka.bytes[9] == 255, "bool and u8 write one byte each");
    Check(GetS("kWeaponBazooka", "PayloadGraphicsResourceID") == "Cow.Payload", "string reads back");
    Check(GetF("kWeaponMegaBazooka", "WormDamageMagnitude") == 240.0f, "the clone field reads back");
    Check(GetF("kWeaponMegaBazooka", "LifeTime") == 5000.0f, "the clone's other fields are untouched");
    Check(Instance().PendingCount() == 6, "one snapshot per touched field");
    const auto& touched = Instance().TouchedFields();
    Check(touched.size() == 6 && touched.back().weapon == "kWeaponMegaBazooka" &&
              touched.back().container == Addr(g_clone) && touched.back().offset == 0 &&
              touched.back().type == FieldType::F32,
          "the touched list names container, offset and type in write order");

    // --- restore ---
    Instance().RestoreAll(&ResolveSame, nullptr);
    Check(Instance().PendingCount() == 0 && Instance().TouchedFields().empty(), "RestoreAll clears the bookkeeping");
    Check(GetF("kWeaponBazooka", "WormDamageMagnitude") == 50.0f, "f32 restored to the pre-match value");
    Check(GetF("kWeaponBazooka", "LifeTime") == 5000.0f, "i32 restored");
    Check(GetF("kWeaponBazooka", "NumBomblets") == 3.0f, "u8 restored");
    Check(Get("kWeaponBazooka", "IsHoming", &v) == TweakError::Ok && v.b, "bool restored");
    Check(GetS("kWeaponBazooka", "PayloadGraphicsResourceID") == "Bazooka.Payload", "string restored");
    Check(GetF("kWeaponMegaBazooka", "WormDamageMagnitude") == 120.0f, "clone field restored");

    Set("kWeaponBazooka", "WormDamageMagnitude", Num(75));
    Instance().RestoreAll(&ResolveNone, nullptr);
    Check(GetF("kWeaponBazooka", "WormDamageMagnitude") == 75.0f, "no restore when the container is gone");
    Check(Instance().PendingCount() == 0, "the snapshot is dropped anyway");

    // --- the Engine on its own ---
    {
        Fill(g_other, 10.0f, "X");
        Engine eng;
        const uintptr_t c = Addr(g_other);
        Check(!eng.Write(c, "W", 0, FieldType::F32, Str("x")), "Engine refuses a mismatched value");
        Check(!eng.Write(c, "W", 0, FieldType::U16, Num(1)), "Engine refuses an unsupported type");
        Check(!eng.Write(0, "W", 0, FieldType::F32, Num(1)), "Engine refuses a null container");
        Check(eng.PendingCount() == 0, "nothing recorded for refused writes");
        Check(eng.Write(c, "W", 0, FieldType::F32, Num(11)), "Engine write");
        uintptr_t other = Addr(g_bazooka);
        eng.RestoreAll([](const char*, void* u) { return *static_cast<uintptr_t*>(u); }, &other);
        Value r;
        Check(eng.Read(c, 0, FieldType::F32, &r) && r.num == 11.0f, "a stale container is not restored");
        Check(!eng.Read(c, 0x30, FieldType::String, &r), "reading a string field with no XString fails");
    }

    // --- numeric shorthands ---
    {
        Fill(g_bazooka, 50.0f, "Bazooka.Payload");
        Instance().Reset();
        float f = 0;
        Check(Get("kWeaponBazooka", "WormDamageMagnitude", &f) == TweakError::Ok && f == 50.0f, "float Get");
        Check(Get("kWeaponBazooka", "IsHoming", &f) == TweakError::Ok && f == 1.0f, "float Get of a bool");
        CheckError(Get("kWeaponBazooka", "PayloadGraphicsResourceID", &f), TweakError::TypeMismatch,
                   "float Get of a string");
        CheckError(Set("kWeaponBazooka", "LifeTime", 12.0f), TweakError::Ok, "float Set of an i32");
        Check(GetF("kWeaponBazooka", "LifeTime") == 12.0f, "float Set reads back");
        Check(FindContainer("kWeaponMegaBazooka") == Addr(g_clone) && FindContainer("kWeaponNoSuch") == 0,
              "FindContainer is the weapons lookup");
        Instance().RestoreAll(&ResolveSame, nullptr);
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
