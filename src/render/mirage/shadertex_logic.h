#pragma once
// Pure logic for mod textures bound to a GLSL replacement's own samplers (wum.shaders.setTexture): which sampler types
// qualify, the texture units they get, the vertical-strip layout of a texture array, its CPU mip chain, and the table
// of bindings a mod has set. Free of GL and engine access; offline self-tested in tests/shadertex_selftest.cpp.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "render/mirage/draw_sprites.h"
#include "render/mirage/shaders_source.h"

namespace melange::mirage::shadertex {
constexpr uint32_t kTarget2D = 0x0DE1, kTarget2DArray = 0x8C1A;
constexpr uint32_t kSampler2D = 0x8B5E, kSampler2DArray = 0x8DC1;
// Units 8..13: above what the engine's programs use (0..3), below the scene copies (14, 15).
constexpr int kFirstUnit = 8, kMaxSamplers = 6;
constexpr int kMaxLayers = 64;
constexpr uint8_t kNeutral = 128;  // every channel of the 1x1 texture an unset sampler reads (0.502)

// Every GL sampler type: 1D..2DRectShadow (0x8B5D..0x8B64) and the GL 3 array, buffer, cube-shadow and integer ones.
inline bool IsSampler(uint32_t type) { return (type >= 0x8B5D && type <= 0x8B64) || (type >= 0x8DC0 && type <= 0x8DD8); }

enum class Kind { None, Tex2D, Array };
inline Kind SamplerKind(uint32_t type) {
    if (type == kSampler2D) return Kind::Tex2D;
    if (type == kSampler2DArray) return Kind::Array;
    return Kind::None;
}
inline uint32_t KindTarget(Kind k) { return k == Kind::Array ? kTarget2DArray : kTarget2D; }

// The unit of the k-th mod sampler of a program, or -1 past the sixth or past the driver's unit count.
inline int UnitFor(int k, int maxUnits) {
    const int u = kFirstUnit + k;
    return k >= 0 && k < kMaxSamplers && u < maxUnits ? u : -1;
}

// A GLSL identifier, not reserved for GL (gl_) or Melange (mg_), at most 63 characters.
inline bool ValidSamplerName(std::string_view s) {
    if (s.empty() || s.size() > 63) return false;
    if (s.substr(0, 3) == "gl_" || s.substr(0, 3) == "mg_") return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return false;
    }
    return true;
}

// A PNG of w x h holding `layers` equal tiles stacked top to bottom (layer 0 = the top tile = the first rows).
// Its pixels in memory are already layer-major, which is what glTexImage3D takes.
inline bool StripLayout(int w, int h, int layers, int maxSize, int maxLayers, int* layerH, const char** why) {
    *layerH = 0;
    if (layers < 1 || layers > maxLayers) return *why = "layers out of range", false;
    if (w <= 0 || h <= 0) return *why = "empty image", false;
    if (h % layers) return *why = "image height is not a multiple of layers", false;
    const int lh = h / layers;
    if (w > maxSize || lh > maxSize) return *why = "layer larger than the GPU's maximum texture size", false;
    *layerH = lh;
    return true;
}

struct Level {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // layer-major RGBA8
};
// Levels 1..n of a layer-major RGBA8 stack down to 1x1, each layer reduced on its own with a 2x2 box filter that
// averages every channel as stored (alpha is data here, not coverage).
inline std::vector<Level> LayerMips(const uint8_t* base, int w, int h, int layers) {
    std::vector<Level> out;
    const uint8_t* src = base;
    int sw = w, sh = h;
    while (sw > 1 || sh > 1) {
        Level l;
        l.w = drawsprites::MipSize(sw);
        l.h = drawsprites::MipSize(sh);
        l.px.resize(static_cast<size_t>(l.w) * l.h * 4 * layers);
        for (int k = 0; k < layers; ++k)
            drawsprites::Downsample(src + static_cast<size_t>(k) * sw * sh * 4, sw, sh, l.px.data() + static_cast<size_t>(k) * l.w * l.h * 4, true);
        out.push_back(std::move(l));
        src = out.back().px.data();
        sw = out.back().w;
        sh = out.back().h;
    }
    return out;
}

// One wum.shaders.setTexture: `owner`'s texture for sampler `sampler` of its replacements of `file` whose entry
// matches `entryGlob`.
struct Binding {
    std::string owner, file, entryGlob, sampler;
    unsigned tex = 0;
};
inline bool SameSlot(const Binding& a, const Binding& b) {
    return a.owner == b.owner && shadersrc::IEquals(a.file, b.file) && shadersrc::IEquals(a.entryGlob, b.entryGlob) && a.sampler == b.sampler;
}
// Stores b (tex 0 removes its slot). True when the table changed.
inline bool Set(std::vector<Binding>& t, const Binding& b) {
    for (size_t i = 0; i < t.size(); ++i) {
        if (!SameSlot(t[i], b)) continue;
        if (!b.tex) {
            t.erase(t.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        if (t[i].tex == b.tex) return false;
        // Moved to the end, so the latest call wins when two globs both match an entry.
        t.erase(t.begin() + static_cast<std::ptrdiff_t>(i));
        t.push_back(b);
        return true;
    }
    if (!b.tex) return false;
    t.push_back(b);
    return true;
}
// The texture `owner` set for `sampler` of the concrete program (file, entry); 0 if none.
inline unsigned Find(const std::vector<Binding>& t, std::string_view owner, std::string_view file, std::string_view entry, std::string_view sampler) {
    for (size_t i = t.size(); i-- > 0;) {
        const Binding& b = t[i];
        if (b.owner == owner && b.sampler == sampler && shadersrc::IEquals(b.file, file) && shadersrc::Glob(b.entryGlob, entry)) return b.tex;
    }
    return 0;
}
// Drops every binding of a texture that is about to be freed. True when any was dropped.
inline bool Forget(std::vector<Binding>& t, unsigned tex) {
    const size_t n = t.size();
    std::erase_if(t, [tex](const Binding& b) { return b.tex == tex; });
    return t.size() != n;
}
}  // namespace melange::mirage::shadertex
