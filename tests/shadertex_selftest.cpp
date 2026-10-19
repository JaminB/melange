// Offline self-test for mod textures on GLSL replacements (render/mirage/shadertex_logic.h): sampler types and units,
// the texture-array strip layout and its per-layer mip chain, and the wum.shaders.setTexture binding table.
// Exit code 0 = all passed.
#include <cstdio>
#include <vector>

#include "render/mirage/shadertex_logic.h"

namespace {
using namespace melange::mirage::shadertex;
int g_checks = 0, g_failures = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

void TestSamplers() {
    Check(IsSampler(0x8B5E) && IsSampler(0x8B5D) && IsSampler(0x8B64) && IsSampler(0x8DC1) && IsSampler(0x8DC4) && IsSampler(0x8DD8),
          "2D, 1D, 2DRectShadow, 2DArray, 2DArrayShadow and isampler types are samplers");
    Check(!IsSampler(0x8B52) && !IsSampler(0x8B5C) && !IsSampler(0x1406) && !IsSampler(0x8DD9), "vec4, mat4, float are not");
    Check(SamplerKind(0x8B5E) == Kind::Tex2D && SamplerKind(0x8DC1) == Kind::Array, "sampler2D and sampler2DArray are fed");
    Check(SamplerKind(0x8B62) == Kind::None && SamplerKind(0x8B60) == Kind::None && SamplerKind(0x8DC4) == Kind::None,
          "shadow and cube samplers are not");
    Check(KindTarget(Kind::Tex2D) == 0x0DE1 && KindTarget(Kind::Array) == 0x8C1A, "targets");
    Check(UnitFor(0, 16) == 8 && UnitFor(5, 16) == 13 && UnitFor(6, 32) == -1 && UnitFor(-1, 16) == -1, "units 8..13, six at most");
    Check(UnitFor(0, 8) == -1 && UnitFor(1, 10) == 9 && UnitFor(2, 10) == -1, "no unit past the driver's count");
    Check(ValidSamplerName("materials") && ValidSamplerName("_x9") && ValidSamplerName(std::string(63, 'a')), "identifiers");
    Check(!ValidSamplerName("") && !ValidSamplerName("9x") && !ValidSamplerName("a-b") && !ValidSamplerName("a[0]") &&
              !ValidSamplerName(std::string(64, 'a')),
          "non-identifiers");
    Check(!ValidSamplerName("mg_scene") && !ValidSamplerName("gl_Foo") && ValidSamplerName("mgx"), "gl_ and mg_ are reserved");
}

void TestStrip() {
    const char* why = nullptr;
    int lh = 0;
    Check(StripLayout(256, 2048, 8, 4096, 64, &lh, &why) && lh == 256, "8 layers of 256");
    Check(StripLayout(64, 64, 1, 4096, 1, &lh, &why) && lh == 64, "one layer is the whole image");
    Check(!StripLayout(256, 1000, 3, 4096, 64, &lh, &why) && lh == 0, "a height that does not split is refused");
    Check(!StripLayout(256, 2048, 0, 4096, 64, &lh, &why) && !StripLayout(256, 2048, 65, 4096, 64, &lh, &why), "layers range");
    Check(!StripLayout(256, 256, 8, 4096, 4, &lh, &why), "more layers than the GPU has");
    Check(!StripLayout(8192, 256, 1, 4096, 64, &lh, &why) && !StripLayout(0, 0, 1, 4096, 64, &lh, &why), "size limits");

    // 2 layers of 4x2: layer 0 all 200 (rows 0..1), layer 1 a 0/100 checker (rows 2..3) with alpha as data.
    std::vector<uint8_t> px(4 * 4 * 2 * 2);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* p = px.data() + 4 * (y * 4 + x);
            const uint8_t v = y < 2 ? 200 : ((x + y) % 2 ? 100 : 0);
            p[0] = p[1] = p[2] = v;
            p[3] = y < 2 ? 0 : 255;  // transparent texels keep their colour: alpha does not weight the average
        }
    const std::vector<Level> mips = LayerMips(px.data(), 4, 2, 2);
    Check(mips.size() == 2 && mips[0].w == 2 && mips[0].h == 1 && mips[1].w == 1 && mips[1].h == 1, "4x2 -> 2x1 -> 1x1");
    if (mips.size() == 2) {
        Check(mips[0].px.size() == 2 * 1 * 4 * 2 && mips[1].px.size() == 1 * 1 * 4 * 2, "every level holds both layers");
        Check(mips[0].px[0] == 200 && mips[0].px[3] == 0 && mips[0].px[4] == 200, "layer 0 averages as stored (alpha 0)");
        Check(mips[0].px[8] == 50 && mips[0].px[11] == 255 && mips[1].px[4] == 50, "layer 1 averages to its own mean");
        Check(mips[1].px[0] == 200, "layers do not bleed into each other");
    }
    Check(LayerMips(px.data(), 1, 1, 1).empty(), "a 1x1 image has no further levels");
}

void TestTable() {
    std::vector<Binding> t;
    Check(Set(t, {"sun", "FixedFunction.cg", "FFFragmentMainTexLit*", "materials", 7}) && t.size() == 1, "add");
    Check(!Set(t, {"sun", "fixedfunction.CG", "ffFragmentMainTexLit*", "materials", 7}), "same slot, same texture: no change");
    Check(Find(t, "sun", "FixedFunction.cg", "FFFragmentMainTexLitCol", "materials") == 7, "glob match");
    Check(Find(t, "sun", "FixedFunction.cg", "FFFragmentMainLit", "materials") == 0, "glob miss");
    Check(Find(t, "other", "FixedFunction.cg", "FFFragmentMainTexLit", "materials") == 0, "another owner's binding is not found");
    Check(Find(t, "sun", "FixedFunction.cg", "FFFragmentMainTexLit", "Materials") == 0, "sampler names are case-sensitive");
    Check(Set(t, {"sun", "FixedFunction.cg", "FFFragmentMainTexLit", "materials", 9}) && t.size() == 2, "a narrower glob is a new slot");
    Check(Find(t, "sun", "FixedFunction.cg", "FFFragmentMainTexLit", "materials") == 9, "the latest matching call wins");
    Check(Set(t, {"sun", "FixedFunction.cg", "FFFragmentMainTexLit*", "materials", 8}) && t.back().tex == 8, "replace moves to the end");
    Check(Find(t, "sun", "FixedFunction.cg", "FFFragmentMainTexLit", "materials") == 8, "...and wins again");
    Check(Set(t, {"sun", "FixedFunction.cg", "FFFragmentMainTexLit*", "materials", 0}) && t.size() == 1, "texture 0 removes");
    Check(!Set(t, {"sun", "FixedFunction.cg", "Nothing", "materials", 0}), "removing a missing slot is no change");
    Set(t, {"sun", "Landscape.cg", "*", "detail", 9});
    Check(Forget(t, 9) && t.empty(), "Forget drops every binding of a texture");
    Check(!Forget(t, 9), "Forget of an unbound texture is no change");
}
}  // namespace

int main() {
    TestSamplers();
    TestStrip();
    TestTable();
    std::printf("shadertex_selftest: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
