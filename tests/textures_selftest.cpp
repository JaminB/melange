// Offline self-test of the MirageTextures (L1 "texture clarity") merge logic (render/mirage/textures_logic.h).
#include <cstdio>

#include "render/mirage/textures_logic.h"

namespace {
using namespace melange::mirage::textures::logic;

struct Ctx {
    int checks = 0, failed = 0;
    void Check(bool ok, const char* what) {
        ++checks;
        if (ok) return;
        ++failed;
        std::printf("FAIL: %s\n", what);
    }
};

void TestVanillaDefault(Ctx& c) {
    Effective e = Merge(IniOverride{}, {}, 16);
    c.Check(e.anisotropy == 0 && !e.trilinear && e.lodBias == 0.f && !e.anyModRequest,
            "no mods, auto ini: vanilla (off) by default");
}

void TestModRequestAppliesAutomatically(Ctx& c) {
    ModRequest r;
    r.present = true;
    r.anisotropy = 16;
    r.trilinear = true;
    Effective e = Merge(IniOverride{}, {r}, 16);
    c.Check(e.anisotropy == 16 && e.trilinear && e.anyModRequest, "a mod's request takes effect under auto ini");
}

void TestIniOverrideWinsOverMod(Ctx& c) {
    ModRequest r;
    r.present = true;
    r.anisotropy = 16;
    r.trilinear = true;
    IniOverride ini;
    ini.anisotropy = 0;
    ini.trilinear = 0;
    Effective e = Merge(ini, {r}, 16);
    c.Check(e.anisotropy == 0 && !e.trilinear, "an explicit ini override beats a mod's request (vanilla behaviour unchanged unless requested)");
}

void TestIniOverrideWithNoMod(Ctx& c) {
    IniOverride ini;
    ini.anisotropy = 8;
    ini.trilinear = 1;
    Effective e = Merge(ini, {}, 16);
    c.Check(e.anisotropy == 8 && e.trilinear && !e.anyModRequest, "an ini override works with no plugin installed");
}

void TestMergeAcrossMods(Ctx& c) {
    ModRequest a, b;
    a.present = true;
    a.anisotropy = 4;
    b.present = true;
    b.anisotropy = 16;
    b.trilinear = true;
    b.lodBiasSet = true;
    b.lodBias = -0.25f;
    Effective e = Merge(IniOverride{}, {a, b}, 16);
    c.Check(e.anisotropy == 16, "max anisotropy wins across mods");
    c.Check(e.trilinear, "trilinear is OR'd across mods");
    c.Check(e.lodBias == -0.25f, "the only mod that sets lodBias wins");
}

void TestLodBiasTakesSharpest(Ctx& c) {
    ModRequest a, b;
    a.present = true;
    a.lodBiasSet = true;
    a.lodBias = -0.25f;
    b.present = true;
    b.lodBiasSet = true;
    b.lodBias = -0.75f;
    Effective e = Merge(IniOverride{}, {a, b}, 16);
    c.Check(e.lodBias == -0.75f, "the most negative (sharpest) requested lodBias wins");
}

void TestAnisotropyClampedToDriverMax(Ctx& c) {
    ModRequest r;
    r.present = true;
    r.anisotropy = 16;
    Effective e = Merge(IniOverride{}, {r}, 8);  // a low-end driver only offers 8x
    c.Check(e.anisotropy == 8, "the effective anisotropy never exceeds the driver's reported max");
}

void TestIgnoresAbsentModRequest(Ctx& c) {
    ModRequest r;  // present == false: no graphics block in this mod's spice.json
    r.anisotropy = 16;
    Effective e = Merge(IniOverride{}, {r}, 16);
    c.Check(e.anisotropy == 0 && !e.anyModRequest, "a mod with no graphics block never contributes");
}

void TestParseAnisotropy(Ctx& c) {
    int v = -99;
    c.Check(ParseAnisotropy("auto", 16, &v) && v == -1, "anisotropy: 'auto' parses to the auto sentinel");
    c.Check(ParseAnisotropy("AUTO", 16, &v) && v == -1, "anisotropy: 'auto' is case-insensitive");
    c.Check(ParseAnisotropy("16", 16, &v) && v == 16, "anisotropy: a plain integer parses");
    c.Check(ParseAnisotropy("0", 16, &v) && v == 0, "anisotropy: 0 (off) parses");
    c.Check(!ParseAnisotropy("17", 16, &v), "anisotropy: above the cap is rejected");
    c.Check(!ParseAnisotropy("-1", 16, &v), "anisotropy: negative is rejected");
    c.Check(!ParseAnisotropy("bogus", 16, &v), "anisotropy: garbage is rejected");
}

void TestParseTriState(Ctx& c) {
    int v = -99;
    c.Check(ParseTriState("auto", &v) && v == -1, "tristate: auto");
    c.Check(ParseTriState("on", &v) && v == 1, "tristate: on");
    c.Check(ParseTriState("Off", &v) && v == 0, "tristate: off, case-insensitive");
    c.Check(!ParseTriState("yes", &v), "tristate: anything else is rejected");
}

void TestParseLodBias(Ctx& c) {
    bool set = true;
    float v = 99.f;
    c.Check(ParseLodBias("auto", 8.f, &set, &v) && !set, "lodBias: 'auto' clears the override");
    c.Check(ParseLodBias("-0.5", 8.f, &set, &v) && set && v == -0.5f, "lodBias: a number sets the override");
    c.Check(!ParseLodBias("9", 8.f, &set, &v), "lodBias: outside the limit is rejected");
}
}  // namespace

int main() {
    Ctx c;
    TestVanillaDefault(c);
    TestModRequestAppliesAutomatically(c);
    TestIniOverrideWinsOverMod(c);
    TestIniOverrideWithNoMod(c);
    TestMergeAcrossMods(c);
    TestLodBiasTakesSharpest(c);
    TestAnisotropyClampedToDriverMax(c);
    TestIgnoresAbsentModRequest(c);
    TestParseAnisotropy(c);
    TestParseTriState(c);
    TestParseLodBias(c);
    std::printf("textures: %d/%d checks passed\n", c.checks - c.failed, c.checks);
    return c.failed ? 1 : 0;
}
