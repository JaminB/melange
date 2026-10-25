#pragma once
// Glyph atlas for draw text, separate from the overlay's; built at 13/16/24 px, other sizes scale from the nearest.
// Melange 0.12 adds mod fonts (holo-font-1, draw_font_core.h) and the layout behind HudTextEx and MeasureText.
#include <cstdint>
#include <vector>

#include "render/mirage/draw_font_core.h"

namespace melange::mirage::drawfont {
bool Init();  // builds (Build) and uploads the bitmap font's atlas; GL context current, outside glBegin
unsigned Texture();

// Appends one quad per visible glyph with the text's top-left at (originX, originY); returns the text's extent.
Extent Layout(const char* utf8, float sizePx, float originX, float originY, std::vector<GlyphQuad>& out);

// CPU half of Init: the bitmap font's glyph metrics, enough to measure text. No GL.
bool Build();

// Mod fonts: ids > 0. AddModFont takes the atlas texture (the caller frees it after RemoveModFont).
int AddModFont(unsigned texture, ModFontData&& data);
unsigned RemoveModFont(int font);  // the font's texture, or 0 if the id is unknown
// The texture that LayoutEx's quads sample: the bitmap font's (font 0, after Init) or a mod font's; 0 if unknown.
unsigned TextureFor(int font);
// LayoutText with the bitmap font (font 0) or a mod font; an unknown font lays out nothing.
Extent LayoutEx(int font, const char* utf8, float sizePx, float x, float y, const LayoutOptions& o, std::vector<GlyphQuad>* out);
}  // namespace melange::mirage::drawfont
