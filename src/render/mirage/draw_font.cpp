#include "render/mirage/draw_font.h"

#include <windows.h>
#include <GL/gl.h>
#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace melange::mirage::drawfont {
namespace {
constexpr float kSizes[3] = {13.f, 16.f, 24.f};

struct State {
    ImFontAtlas* atlas = nullptr;
    ImFont* font = nullptr;
    ImFontBaked* baked[3] = {nullptr, nullptr, nullptr};
    unsigned texture = 0;
    bool attempted = false, ready = false;
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

// BMP only (16-bit ImWchar); malformed input yields U+FFFD and always consumes at least one byte.
int DecodeUtf8(const char* s, uint32_t* out) {
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80) {
        *out = c0;
        return 1;
    }
    int len = (c0 & 0xE0) == 0xC0 ? 2 : (c0 & 0xF0) == 0xE0 ? 3 : (c0 & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0) {
        *out = 0xFFFD;
        return 1;
    }
    uint32_t cp = c0 & (0xFF >> (len + 1));
    for (int i = 1; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) {
            *out = 0xFFFD;
            return i;
        }
        cp = (cp << 6) | (c & 0x3F);
    }
    *out = (cp > 0xFFFF) ? 0xFFFD : cp;
    return len;
}
}  // namespace

bool Init() {
    if (g_state.attempted) return g_state.ready;
    g_state.attempted = true;

    g_state.atlas = new ImFontAtlas();
    ImFontConfig cfg;
    cfg.SizePixels = kSizes[0];
    cfg.FontDataOwnedByAtlas = true;
    snprintf(cfg.Name, sizeof cfg.Name, "MirageDraw");
    g_state.font = g_state.atlas->AddFontDefaultBitmap(&cfg);
    if (!g_state.font) return false;
    for (int i = 0; i < 3; ++i) g_state.baked[i] = g_state.font->GetFontBaked(kSizes[i]);
    if (!g_state.baked[0] || !g_state.baked[1] || !g_state.baked[2] || !g_state.atlas->Build()) return false;

    unsigned char* pixels = nullptr;
    int w = 0, h = 0;
    g_state.atlas->GetTexDataAsRGBA32(&pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0) return false;

    glGenTextures(1, &g_state.texture);
    glBindTexture(GL_TEXTURE_2D, g_state.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);

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

    for (const char* p = utf8; *p;) {
        uint32_t cp = 0;
        p += DecodeUtf8(p, &cp);
        if (cp == '\n') {
            extent.w = (x - originX > extent.w) ? x - originX : extent.w;
            x = originX;
            originY += baked->Size * scale;
            continue;
        }
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
}  // namespace melange::mirage::drawfont
