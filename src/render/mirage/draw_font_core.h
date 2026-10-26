#pragma once
// Text layout for draw text, free of GL and ImGui: UTF-8, line breaks, kerning, alignment, cutting to a width with an
// ellipsis, and mod fonts in the holo-font-1 format (Melange 0.12). draw_font.cpp feeds it the ImGui bitmap font and the
// loaded mod fonts; font_selftest checks it.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace melange::mirage::drawfont {
struct GlyphQuad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
};
struct Extent {
    float w = 0, h = 0;
};

// One code point from [s, end); malformed input yields U+FFFD and always consumes at least one byte (s < end).
int DecodeUtf8(const char* s, const char* end, uint32_t* out);

// A glyph at a pixel size, relative to the pen at the top of its line.
struct GlyphMetrics {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float advance = 0;
    bool visible = false;
};
class GlyphSource {
  public:
    virtual ~GlyphSource() = default;
    virtual bool Glyph(uint32_t cp, float sizePx, GlyphMetrics* out) const = 0;  // false: the font lacks it
    virtual float Kern(uint32_t, uint32_t, float) const { return 0; }           // advance change between two code points
    virtual float LineHeight(float sizePx) const = 0;
};

enum class Align : uint8_t { Left, Center, Right };
struct LayoutOptions {
    Align align = Align::Left;
    float maxWidth = 0;   // > 0: longer lines are cut to fit
    bool ellipsis = true; // ... ending in U+2026 (or "..." if the font lacks it)
};
constexpr uint32_t kEllipsis = 0x2026;
constexpr size_t kMaxLayoutBytes = 64 * 1024;  // text past this is not drawn or measured

// Lays out `utf8` with the first line's top at y and its anchor (left end, centre or right end by align) at x, one quad
// per visible glyph into *out (may be null to measure only). A code point the font lacks draws as '?', or is skipped
// when it lacks '?' too; control characters other than '\n' are skipped. Returns the widest line's width (after any
// cut) and lines * line height.
Extent LayoutText(const GlyphSource& font, const char* utf8, float sizePx, float x, float y, const LayoutOptions& o,
                  std::vector<GlyphQuad>* out);

// ---- holo-font-1 (docs/lua-api.md "Mod fonts"). Every number but `size` and `scale` is in atlas pixels: glyph
// rectangles, offsets and advances, lineHeight, ascent, outline and kern. A glyph [x, y, w, h, xoff, yoff, adv] is
// drawn with its top-left at (pen + xoff, baseline + yoff), the baseline `ascent` below the line's top; drawn at
// sizePx every atlas pixel is sizePx / (size * scale) window pixels.
struct ModGlyph {
    float x, y, w, h, xoff, yoff, adv;
};
struct ModFontData {
    float size = 0, scale = 1, lineHeight = 0, ascent = 0, outline = 0;
    int atlasW = 0, atlasH = 0;  // set by CheckAtlas
    std::unordered_map<uint32_t, ModGlyph> glyphs;
    std::unordered_map<uint64_t, float> kern;  // (a << 32) | b
};
constexpr int kMaxAtlasSide = 2048;
constexpr size_t kMaxGlyphs = 8192, kMaxKernPairs = 32768;
constexpr size_t kMaxMetricsBytes = 1 << 20;

// Parses and range-checks the metrics; false with a reason.
bool ParseModFont(std::string_view json, ModFontData* out, std::string* why);
// Checks the atlas size (at most kMaxAtlasSide a side) and that every glyph rectangle lies inside it; records the size.
bool CheckAtlas(ModFontData* font, int atlasW, int atlasH, std::string* why);
// ParseModFont + CheckAtlas with std types only (for callers that only validate, e.g. the sandbox self-test's fakes).
bool ValidateModFont(std::string_view json, int atlasW, int atlasH, std::string* why);

class ModFontSource final : public GlyphSource {
  public:
    explicit ModFontSource(const ModFontData& d) : d_(d) {}
    bool Glyph(uint32_t cp, float sizePx, GlyphMetrics* out) const override;
    float Kern(uint32_t a, uint32_t b, float sizePx) const override;
    float LineHeight(float sizePx) const override;

  private:
    float Factor(float sizePx) const { return sizePx / (d_.size * d_.scale); }
    const ModFontData& d_;
};
}  // namespace melange::mirage::drawfont
