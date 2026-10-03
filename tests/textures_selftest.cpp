// Offline self-test of the MirageTextures and MirageShadows merge logic (render/mirage/textures_logic.h,
// render/mirage/shadows_logic.h).
#include <cstdio>

#include "render/mirage/shadows_logic.h"
#include "render/mirage/supersample_logic.h"
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
void TestShadows(Ctx& c) {
    namespace s = melange::mirage::shadows::logic;
    c.Check(s::Merge(-1, {}).size == 0, "shadows: vanilla with no request and auto ini");
    c.Check(s::Merge(-1, {0, 0}).size == 0, "shadows: mods without a request stay vanilla");
    s::Effective e = s::Merge(-1, {2048, 1024});
    c.Check(e.size == 2048 && e.anyModRequest, "shadows: the largest request wins");
    c.Check(s::Merge(-1, {8192}).size == 4096, "shadows: capped at 4096");
    c.Check(s::Merge(0, {4096}).size == 0, "shadows: ini vanilla beats a request");
    c.Check(s::Merge(1024, {4096}).size == 1024, "shadows: an ini size beats a request");
    c.Check(s::Merge(2048, {}).size == 2048, "shadows: an ini size works with no mod");
    c.Check(s::ModRequest(2048, -1) == 2048 && s::ModRequest(2048, 4096) == 4096 && s::ModRequest(2048, 0) == 0,
            "shadows: a runtime request replaces the manifest's");
    c.Check(s::Target(s::Merge(-1, {}), 1024) == 1024 && s::Target(e, 1024) == 2048, "shadows: target falls back to the cfg value");
    int v = 7;
    c.Check(s::ParseIni("auto", &v) && v == -1, "shadows: ini auto");
    c.Check(s::ParseIni("Vanilla", &v) && v == 0, "shadows: ini vanilla");
    c.Check(s::ParseIni("4096", &v) && v == 4096, "shadows: ini size");
    c.Check(!s::ParseIni("3000", &v) && !s::ParseIni("8192", &v) && !s::ParseIni("x", &v), "shadows: bad ini values rejected");
}
void TestSupersample(Ctx& c) {
    namespace s = melange::mirage::supersample::logic;
    c.Check(s::Merge(-1, {}).samples == 0 && !s::Merge(-1, {}).anyModRequest, "supersample: vanilla with no request");
    s::Effective e = s::Merge(-1, {2, 4});
    c.Check(e.samples == 4 && e.anyModRequest, "supersample: the largest request wins");
    c.Check(s::Merge(-1, {16, 8, 3}).samples == 0, "supersample: mods cannot ask for more than 4 samples");
    c.Check(s::Merge(0, {4}).samples == 0, "supersample: ini vanilla beats a request");
    c.Check(s::Merge(1, {4}).samples == 1 && s::Merge(16, {}).samples == 16, "supersample: an ini count beats a request");
    int v = 7;
    c.Check(s::ParseIni("auto", &v) && v == -1 && s::ParseIni("Vanilla", &v) && v == 0 && s::ParseIni("off", &v) && v == 1,
            "supersample: ini words");
    c.Check(s::ParseIni("4", &v) && v == 4 && !s::ParseIni("3", &v) && !s::ParseIni("2x2", &v), "supersample: ini counts");
    const s::EngineAa vanilla{1, 1, true, true};
    c.Check(s::Same(s::Target(s::Merge(-1, {}), vanilla), vanilla), "supersample: no request keeps the engine's state");
    s::EngineAa t = s::Target(e, vanilla);
    c.Check(t.x == 2 && t.y == 2 && !t.fxaa && !t.hardware, "supersample: 4 samples is true 2x2 with FXAA off");
    t = s::Target(s::Merge(-1, {2}), vanilla);
    c.Check(t.x == 1 && t.y == 2, "supersample: 2 samples is 1x2 like /SSAA:2");
    c.Check(s::Same({2, 2, false, true}, {2, 2, false, true}) && !s::Same({2, 2, false, true}, {2, 2, false, false}) &&
                s::Same({1, 1, false, true}, {1, 1, false, false}),
            "supersample: hardware AA only matters above 1x1");
    bool landed = true;
    for (s::EngineAa want : {s::EngineAa{1, 1, false, true}, s::EngineAa{1, 1, true, true}, s::EngineAa{1, 2, false, false},
                             s::EngineAa{2, 2, false, false}, s::EngineAa{2, 4, false, true}, s::EngineAa{4, 4, false, false}})
        landed &= s::Same(s::Step(s::Before(want)), want);
    c.Check(landed, "supersample: one engine step from Before() lands on every target");
    s::EngineAa cyc{1, 1, false, true};
    for (int i = 0; i < 6; ++i) cyc = s::Step(cyc);
    c.Check(cyc.x == 1 && cyc.y == 1 && !cyc.fxaa, "supersample: the engine cycle has six states");
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
    TestShadows(c);
    TestSupersample(c);
    std::printf("textures: %d/%d checks passed\n", c.checks - c.failed, c.checks);
    return c.failed ? 1 : 0;
}
