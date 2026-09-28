// Offline self-test for component F (src/lua/sim/tweak.*). No game needed: FindContainer() must
// safely return 0 outside the game (game::IsKnownBuild() is false here), so every Get/Set case
// below either stops at a schema error or ends at TweakError::NoContainer. The Engine class is
// exercised directly with a fake in-process "container" buffer.
#include "lua/sim/tweak.h"

#include <cstdio>
#include <cstring>

using namespace melange::tweak;

namespace {
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
}  // namespace

int main() {
    // --- schema lookups ---
    const WeaponClass* bazooka = FindWeaponClass("kWeaponBazooka");
    Check(bazooka != nullptr, "kWeaponBazooka has a schema mapping");
    if (bazooka) {
        Check(std::strcmp(bazooka->containerClass, "PayloadWeaponPropertiesContainer") == 0,
              "kWeaponBazooka maps to PayloadWeaponPropertiesContainer");
        Check(bazooka->fieldCount == 108, "PayloadWeaponPropertiesContainer has 108 fields");
    }
    Check(FindWeaponClass("kWeaponNoSuchThing") == nullptr, "unmapped weapon has no schema mapping");
    Check(FindWeaponClass(nullptr) == nullptr, "null weapon name is handled");

    Check(FindVerifiedOffset("PayloadWeaponPropertiesContainer", "WormDamageMagnitude") != nullptr,
          "WormDamageMagnitude has a verified offset");
    Check(FindVerifiedOffset("PayloadWeaponPropertiesContainer", "WormDamageMagnitude")->offset == 0x15c,
          "the verified offset is +0x15c (F0/U6)");
    Check(FindVerifiedOffset("PayloadWeaponPropertiesContainer", "Scale") == nullptr,
          "a schema-numeric field with no spike result has no verified offset");
    Check(FindVerifiedOffset("GunWeaponPropertiesContainer", "WormDamageMagnitude") == nullptr,
          "the verified offset does not leak to a different container class");

    // --- FindContainer must be inert (and never crash) outside the game ---
    Check(FindContainer("kWeaponBazooka") == 0, "FindContainer is 0 on an unrecognised build (this binary)");
    Check(FindContainer(nullptr) == 0, "FindContainer handles a null name");
    Check(FindContainer("") == 0, "FindContainer handles an empty name");

    // --- Get/Set error precedence, all offline (real container lookup always fails here) ---
    float v = 0;
    CheckError(Get("kWeaponBazookaTypo", "WormDamageMagnitude", &v), TweakError::UnknownWeapon,
               "Get: unknown weapon");
    CheckError(Get("kWeaponBazooka", "NoSuchField", &v), TweakError::UnknownField, "Get: unknown field");
    CheckError(Get("kWeaponBazooka", "IsAimedWeapon", &v), TweakError::NotNumeric,
               "Get: a real but non-numeric field");
    CheckError(Get("kWeaponBazooka", "Scale", &v), TweakError::NoVerifiedOffset,
               "Get: a numeric field with no verified offset");
    CheckError(Get("kWeaponBazooka", "WormDamageMagnitude", &v), TweakError::NoContainer,
               "Get: a fully valid request still needs a live container");

    CheckError(Set("kWeaponBazookaTypo", "WormDamageMagnitude", 1.0f), TweakError::UnknownWeapon,
               "Set: unknown weapon");
    CheckError(Set("kWeaponBazooka", "NoSuchField", 1.0f), TweakError::UnknownField, "Set: unknown field");
    CheckError(Set("kWeaponBazooka", "IsAimedWeapon", 1.0f), TweakError::NotNumeric,
               "Set: a real but non-numeric field");
    CheckError(Set("kWeaponBazooka", "Scale", 1.0f), TweakError::NoVerifiedOffset,
               "Set: a numeric field with no verified offset");
    CheckError(Set("kWeaponBazooka", "WormDamageMagnitude", 75.0f), TweakError::NoContainer,
               "Set: a fully valid request still needs a live container");

    for (int e = 0; e <= static_cast<int>(TweakError::NoContainer); ++e) {
        const char* s = ToString(static_cast<TweakError>(e));
        Check(s != nullptr && *s, "ToString covers every TweakError");
    }

    // --- Engine snapshot/restore, with a fake container so no real memory is touched ---
    {
        alignas(4) unsigned char fake[64] = {};
        const auto container = reinterpret_cast<uintptr_t>(fake);
        constexpr uint32_t kOff = 0x10;
        float original = 50.0f;
        std::memcpy(fake + kOff, &original, sizeof original);

        Engine eng;
        Check(eng.PendingCount() == 0, "a fresh Engine has nothing pending");

        Check(eng.WriteField(container, "TestWeapon", kOff, 75.0f), "WriteField succeeds on a fake container");
        float got = 0;
        Check(eng.ReadField(container, kOff, &got) && got == 75.0f, "ReadField sees the new value");
        Check(eng.PendingCount() == 1, "one field is pending after one WriteField");

        Check(eng.WriteField(container, "TestWeapon", kOff, 90.0f), "a second WriteField on the same field");
        Check(eng.ReadField(container, kOff, &got) && got == 90.0f, "ReadField sees the latest value");
        Check(eng.PendingCount() == 1, "the second WriteField does not add a new snapshot");

        eng.RestoreAll([](const char*, void*) -> uintptr_t { return 0; }, nullptr);
        Check(eng.ReadField(container, kOff, &got) && got == 90.0f,
              "RestoreAll leaves the value alone when the container cannot be re-resolved (U6)");
        Check(eng.PendingCount() == 0, "RestoreAll always drops the snapshot, restored or not");
    }
    {
        alignas(4) unsigned char fake[64] = {};
        auto container = reinterpret_cast<uintptr_t>(fake);
        constexpr uint32_t kOff = 0x20;
        float original = 50.0f;
        std::memcpy(fake + kOff, &original, sizeof original);

        Engine eng;
        eng.WriteField(container, "TestWeapon", kOff, 75.0f);
        eng.RestoreAll([](const char*, void* user) -> uintptr_t { return *static_cast<uintptr_t*>(user); },
                       &container);
        float got = 0;
        Check(eng.ReadField(container, kOff, &got) && got == 50.0f,
              "RestoreAll restores the original bits when the container still matches");
        Check(eng.PendingCount() == 0, "nothing pending after a successful restore");
    }
    {
        alignas(4) unsigned char fakeA[64] = {}, fakeB[64] = {};
        auto containerA = reinterpret_cast<uintptr_t>(fakeA);
        auto containerB = reinterpret_cast<uintptr_t>(fakeB);
        constexpr uint32_t kOff = 0x30;
        float original = 50.0f;
        std::memcpy(fakeA + kOff, &original, sizeof original);

        Engine eng;
        eng.WriteField(containerA, "TestWeapon", kOff, 75.0f);
        eng.RestoreAll([](const char*, void* user) -> uintptr_t { return *static_cast<uintptr_t*>(user); },
                       &containerB);
        float got = 0;
        eng.ReadField(containerA, kOff, &got);
        Check(got == 75.0f,
              "RestoreAll skips the write when the re-resolved container differs (a stale-pointer guard, U6)");
    }

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
