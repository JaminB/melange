#include "render/mirage/draw_font.h"

#include <windows.h>
#include <GL/gl.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <unordered_map>

namespace melange::mirage::drawfont {
namespace {
constexpr float kSizes[3] = {13.f, 16.f, 24.f};
constexpr ImWchar kFirstChar = 0x20, kLastChar = 0xFF;

struct State {
    ImFontAtlas* atlas = nullptr;
    ImFont* font = nullptr;
    ImFontBaked* baked[3] = {nullptr, nullptr, nullptr};
    unsigned texture = 0;
    bool attempted = false, built = false;  // Build
    bool uploadTried = false, ready = false;  // Init
};
State g_state;

int NearestSizeIndex(float sizePx) {
    int best = 0;
    float bestDelta = 1e30f;
    for (int i = 0; i < 3; ++i) {
        float d = std::fabs(kSizes[i] - sizePx);
        if (d < bestDelta) {
            bestDelta = d;
            best = i;
        }
    }
    return best;
}
}  // namespace

bool Build() {
    if (g_state.attempted) return g_state.built;
    g_state.attempted = true;

    g_state.atlas = new ImFontAtlas();
    ImFontConfig cfg;
    cfg.SizePixels = kSizes[0];
    cfg.FontDataOwnedByAtlas = true;
    snprintf(cfg.Name, sizeof cfg.Name, "MirageDraw");
    g_state.font = g_state.atlas->AddFontDefaultBitmap(&cfg);
    if (!g_state.font || !g_state.atlas->Build()) return false;
    // ImGui 1.92 bakes glyphs on first lookup, so every glyph Layout can use is loaded before the one upload.
    for (int i = 0; i < 3; ++i) {
        g_state.baked[i] = g_state.font->GetFontBaked(kSizes[i]);
        if (!g_state.baked[i]) return false;
        for (ImWchar c = kFirstChar; c <= kLastChar; ++c) g_state.baked[i]->FindGlyph(c);
    }
    g_state.built = true;
    return true;
}

bool Init() {
    if (g_state.uploadTried) return g_state.ready;
    if (!Build()) return false;
    g_state.uploadTried = true;

    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    g_state.atlas->GetTexDataAsRGBA32(&pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0) return false;

    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    glGenTextures(1, &g_state.texture);
    glBindTexture(GL_TEXTURE_2D, g_state.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));

    g_state.ready = true;
    return true;
}

unsigned Texture() { return g_state.ready ? g_state.texture : 0; }

Extent Layout(const char* utf8, float sizePx, float originX, float originY, std::vector<GlyphQuad>& out) {
    Extent extent;
    if (!g_state.ready || !utf8) return extent;
    ImFontBaked* baked = g_state.baked[NearestSizeIndex(sizePx)];
    float scale = sizePx / baked->Size;
    float x = originX;
    float maxH = 0;

    const char* end = utf8 + strlen(utf8);
    for (const char* p = utf8; p < end;) {
        uint32_t cp = 0;
        p += DecodeUtf8(p, end, &cp);
        if (cp == '\n') {
            extent.w = (x - originX > extent.w) ? x - originX : extent.w;
            x = originX;
            originY += baked->Size * scale;
            continue;
        }
        if (cp < kFirstChar) continue;
        if (cp > kLastChar) cp = '?';
        const ImFontGlyph* g = baked->FindGlyph(static_cast<ImWchar>(cp));
        if (!g) continue;
        if (g->Visible) {
            GlyphQuad q;
            q.x0 = x + g->X0 * scale;
            q.y0 = originY + g->Y0 * scale;
            q.x1 = x + g->X1 * scale;
            q.y1 = originY + g->Y1 * scale;
            q.u0 = g->U0;
            q.v0 = g->V0;
            q.u1 = g->U1;
            q.v1 = g->V1;
            out.push_back(q);
            if (q.y1 - originY > maxH) maxH = q.y1 - originY;
        }
        x += g->AdvanceX * scale;
    }
    extent.w = (x - originX > extent.w) ? x - originX : extent.w;
    extent.h = (maxH > 0) ? maxH : baked->Size * scale;
    return extent;
}
namespace {
// The bitmap font as a GlyphSource: Latin-1 only, the bake nearest to the size scaled.
class BitmapSource final : public GlyphSource {
  public:
    bool Glyph(uint32_t cp, float sizePx, GlyphMetrics* out) const override {
        if (!g_state.built || cp < kFirstChar || cp > kLastChar) return false;
        ImFontBaked* baked = g_state.baked[NearestSizeIndex(sizePx)];
        const ImFontGlyph* g = baked->FindGlyph(static_cast<ImWchar>(cp));
        if (!g) return false;
        const float scale = sizePx / baked->Size;
        out->x0 = g->X0 * scale;
        out->y0 = g->Y0 * scale;
        out->x1 = g->X1 * scale;
        out->y1 = g->Y1 * scale;
        out->u0 = g->U0;
        out->v0 = g->V0;
        out->u1 = g->U1;
        out->v1 = g->V1;
        out->advance = g->AdvanceX * scale;
        out->visible = g->Visible;
        return true;
    }
    float LineHeight(float sizePx) const override { return sizePx; }
};

struct ModFont {
    unsigned texture = 0;
    ModFontData data;
    std::unique_ptr<ModFontSource> source;
};
std::unordered_map<int, ModFont> g_modFonts;
int g_nextFont = 1;
}  // namespace

int AddModFont(unsigned texture, ModFontData&& data) {
    const int id = g_nextFont++;
    ModFont& f = g_modFonts[id];
    f.texture = texture;
    f.data = std::move(data);
    f.source = std::make_unique<ModFontSource>(f.data);
    return id;
}

unsigned RemoveModFont(int font) {
    auto it = g_modFonts.find(font);
    if (it == g_modFonts.end()) return 0;
    const unsigned tex = it->second.texture;
    g_modFonts.erase(it);
    return tex;
}

unsigned TextureFor(int font) {
    if (font == 0) return Texture();
    auto it = g_modFonts.find(font);
    return it == g_modFonts.end() ? 0 : it->second.texture;
}

Extent LayoutEx(int font, const char* utf8, float sizePx, float x, float y, const LayoutOptions& o, std::vector<GlyphQuad>* out) {
    if (font == 0) {
        static const BitmapSource kBitmap;
        if (!Build()) return {};
        return LayoutText(kBitmap, utf8, sizePx, x, y, o, out);
    }
    auto it = g_modFonts.find(font);
    if (it == g_modFonts.end()) return {};
    return LayoutText(*it->second.source, utf8, sizePx, x, y, o, out);
}
}  // namespace melange::mirage::drawfont
