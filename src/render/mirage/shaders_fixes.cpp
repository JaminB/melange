// Built-in shader patches, applied as find/replace text so that no game or NVIDIA file is redistributed.
#include "render/mirage/shaders_source.h"

namespace melange::mirage::shadersrc {
namespace {
// PostProcess.cg defines FXAA_GLSL_120, so Fxaa3_9.h fetches with texture2DLod, which Cg lowers to an explicit-LOD
// tex2D that arbfp1 cannot express (error C3004). The scene target has one mip level, so a plain tex2D samples the
// same texels.
constexpr char kFxaa[] =
    "@@ entry CopyFxaa*\n"
    "@@ find\n"
    "#define FxaaTexTop(t, p) texture2DLod(t, p, 0.0)\n"
    "@@ replace\n"
    "#define FxaaTexTop(t, p) tex2D(t, p)\n"
    "@@ end\n"
    "@@ find\n"
    "#define FxaaTexOff(t, p, o, r) texture2DLod(t, p + (o * r), 0.0)\n"
    "@@ replace\n"
    "#define FxaaTexOff(t, p, o, r) tex2D(t, p + (o * r))\n"
    "@@ end\n";
}  // namespace

const std::vector<Builtin>& Builtins() {
    static const std::vector<Builtin> list = {
        {"fxaa", "Fxaa3_9.h", "CopyFxaa*", &LacksExplicitLod, kFxaa},
    };
    return list;
}
}  // namespace melange::mirage::shadersrc
