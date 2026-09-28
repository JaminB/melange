#pragma once
// Glyph atlas for draw text, separate from the overlay's; built at 13/16/24 px, other sizes scale from the nearest.
#include <cstdint>
#include <vector>

namespace melange::mirage::drawfont {
struct GlyphQuad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
};
struct Extent {
    float w = 0, h = 0;
};

bool Init();
unsigned Texture();

// Appends one quad per visible glyph with the text's top-left at (originX, originY); returns the text's extent.
Extent Layout(const char* utf8, float sizePx, float originX, float originY, std::vector<GlyphQuad>& out);
}  // namespace melange::mirage::drawfont
