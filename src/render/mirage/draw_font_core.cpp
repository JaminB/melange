#include "render/mirage/draw_font_core.h"

#include <cmath>
#include <cstring>

#include "tools/json_read.h"

namespace melange::mirage::drawfont {
int DecodeUtf8(const char* s, const char* end, uint32_t* out) {
    const unsigned char c0 = static_cast<unsigned char>(s[0]);
    if (c0 < 0x80) {
        *out = c0;
        return 1;
    }
    const int len = (c0 & 0xE0) == 0xC0 ? 2 : (c0 & 0xF0) == 0xE0 ? 3 : (c0 & 0xF8) == 0xF0 ? 4 : 0;
    if (len == 0) {
        *out = 0xFFFD;
        return 1;
    }
    uint32_t cp = c0 & (0xFF >> (len + 1));
    for (int i = 1; i < len; ++i) {
        if (s + i >= end || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            *out = 0xFFFD;
            return i;
        }
        cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    }
    // Overlong forms, surrogates and values past U+10FFFF are malformed too.
    static const uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};
    if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    *out = cp;
    return len;
}

namespace {
struct Item {
    uint32_t cp;
    GlyphMetrics m;
    float kernBefore;
};

// The glyph for cp, or the font's '?' in its place; false if the font has neither.
bool Resolve(const GlyphSource& font, uint32_t* cp, float sizePx, GlyphMetrics* m) {
    if (font.Glyph(*cp, sizePx, m)) return true;
    *cp = '?';
    return font.Glyph('?', sizePx, m);
}

float Width(const std::vector<Item>& items, size_t count) {
    float w = 0;
    for (size_t i = 0; i < count; ++i) w += items[i].kernBefore + items[i].m.advance;
    return w;
}

// Cuts the line to fit maxWidth, ending it with the ellipsis when that is wanted and fits.
void Cut(const GlyphSource& font, float sizePx, const LayoutOptions& o, std::vector<Item>& items) {
    std::vector<Item> tail;
    if (o.ellipsis) {
        GlyphMetrics m;
        if (font.Glyph(kEllipsis, sizePx, &m)) {
            tail.push_back({kEllipsis, m, 0});
        } else if (font.Glyph('.', sizePx, &m)) {
            const float k = font.Kern('.', '.', sizePx);
            tail = {{'.', m, 0}, {'.', m, k}, {'.', m, k}};
        }
    }
    const float tailW = Width(tail, tail.size());
    if (tailW > o.maxWidth) {
        items.clear();
        return;
    }
    size_t keep = 0;
    float w = 0;
    while (keep < items.size()) {
        const float next = w + items[keep].kernBefore + items[keep].m.advance;
        if (next + tailW > o.maxWidth) break;
        w = next;
        ++keep;
    }
    if (!tail.empty())
        while (keep > 0 && items[keep - 1].cp == ' ') --keep;
    items.resize(keep);
    items.insert(items.end(), tail.begin(), tail.end());
}
}  // namespace

Extent LayoutText(const GlyphSource& font, const char* utf8, float sizePx, float x, float y, const LayoutOptions& o,
                  std::vector<GlyphQuad>* out) {
    Extent extent;
    if (!utf8 || !*utf8 || !(sizePx > 0) || !std::isfinite(sizePx)) return extent;
    const char* p = utf8;
    const char* end = utf8 + strnlen(utf8, kMaxLayoutBytes);
    const float lineH = font.LineHeight(sizePx);
    std::vector<Item> items;
    int lines = 0;
    while (true) {
        items.clear();
        uint32_t prev = 0;
        bool newline = false;
        while (p < end) {
            uint32_t cp = 0;
            p += DecodeUtf8(p, end, &cp);
            if (cp == '\n') {
                newline = true;
                break;
            }
            if (cp < 0x20 || cp == 0x7F) continue;
            GlyphMetrics m;
            if (!Resolve(font, &cp, sizePx, &m)) continue;
            items.push_back({cp, m, prev ? font.Kern(prev, cp, sizePx) : 0.f});
            prev = cp;
        }
        if (o.maxWidth > 0 && Width(items, items.size()) > o.maxWidth) Cut(font, sizePx, o, items);
        const float w = Width(items, items.size());
        float pen = o.align == Align::Center ? x - w * 0.5f : o.align == Align::Right ? x - w : x;
        const float top = y + lines * lineH;
        if (out)
            for (const Item& it : items) {
                pen += it.kernBefore;
                const GlyphMetrics& m = it.m;
                if (m.visible) out->push_back({pen + m.x0, top + m.y0, pen + m.x1, top + m.y1, m.u0, m.v0, m.u1, m.v1});
                pen += m.advance;
            }
        if (w > extent.w) extent.w = w;
        ++lines;
        if (!newline) break;
    }
    extent.h = lines * lineH;
    return extent;
}

// ---------------------------------------------------------------- holo-font-1
namespace {
bool Fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

// A decimal code point key ("65"), no sign, no leading zeros beyond "0".
bool ParseCodePoint(std::string_view s, uint32_t* out) {
    if (s.empty() || s.size() > 7 || (s.size() > 1 && s[0] == '0')) return false;
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    if (v > 0x10FFFF) return false;
    *out = v;
    return true;
}

bool Number(const json::Value& obj, const char* key, double lo, double hi, bool required, double def, float* out,
            std::string* why) {
    const json::Value* v = obj.Get(key);
    if (!v) {
        if (required) return Fail(why, std::string("metrics: \"") + key + "\" is missing");
        *out = static_cast<float>(def);
        return true;
    }
    if (!v->IsNumber() || !(v->number >= lo) || !(v->number <= hi))
        return Fail(why, std::string("metrics: \"") + key + "\" must be a number from " + std::to_string(static_cast<int>(lo)) +
                             " to " + std::to_string(static_cast<int>(hi)));
    *out = static_cast<float>(v->number);
    return true;
}
}  // namespace

bool ParseModFont(std::string_view text, ModFontData* out, std::string* why) {
    *out = ModFontData{};
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, kMaxMetricsBytes))
        return Fail(why, "metrics JSON line " + std::to_string(e.line) + ": " + e.text);
    if (!v.IsObject()) return Fail(why, "metrics: a JSON object expected");
    const json::Value* fmt = v.Get("format");
    if (!fmt || !fmt->IsString() || fmt->string != "holo-font-1") return Fail(why, "metrics: \"format\" must be \"holo-font-1\"");
    if (!Number(v, "size", 1, 1024, true, 0, &out->size, why) || !Number(v, "scale", 0.25, 16, false, 1, &out->scale, why) ||
        !Number(v, "lineHeight", 1, 8192, true, 0, &out->lineHeight, why) ||
        !Number(v, "ascent", 0, 8192, true, 0, &out->ascent, why) || !Number(v, "outline", 0, 256, false, 0, &out->outline, why))
        return false;
    const json::Value* glyphs = v.Get("glyphs");
    if (!glyphs || !glyphs->IsObject() || glyphs->members.empty()) return Fail(why, "metrics: \"glyphs\" must be a non-empty object");
    if (glyphs->members.size() > kMaxGlyphs) return Fail(why, "metrics: more than " + std::to_string(kMaxGlyphs) + " glyphs");
    out->glyphs.reserve(glyphs->members.size());
    for (const auto& [key, g] : glyphs->members) {
        uint32_t cp = 0;
        if (!ParseCodePoint(key, &cp)) return Fail(why, "metrics: glyph key \"" + key + "\" is not a decimal code point");
        if (!g.IsArray() || g.items.size() != 7) return Fail(why, "metrics: glyph " + key + " must be [x, y, w, h, xoff, yoff, adv]");
        float f[7];
        for (int i = 0; i < 7; ++i) {
            if (!g.items[i].IsNumber() || !std::isfinite(g.items[i].number)) return Fail(why, "metrics: glyph " + key + " has a non-number");
            f[i] = static_cast<float>(g.items[i].number);
        }
        const bool ok = f[0] >= 0 && f[1] >= 0 && f[2] >= 0 && f[3] >= 0 && f[2] <= kMaxAtlasSide && f[3] <= kMaxAtlasSide &&
                        std::fabs(f[4]) <= 8192 && std::fabs(f[5]) <= 8192 && f[6] >= 0 && f[6] <= 8192;
        if (!ok) return Fail(why, "metrics: glyph " + key + " is out of range");
        out->glyphs[cp] = {f[0], f[1], f[2], f[3], f[4], f[5], f[6]};
    }
    if (const json::Value* kern = v.Get("kern")) {
        if (!kern->IsObject()) return Fail(why, "metrics: \"kern\" must be an object");
        if (kern->members.size() > kMaxKernPairs) return Fail(why, "metrics: more than " + std::to_string(kMaxKernPairs) + " kern pairs");
        out->kern.reserve(kern->members.size());
        for (const auto& [key, dx] : kern->members) {
            const size_t comma = key.find(',');
            uint32_t a = 0, b = 0;
            if (comma == std::string::npos || !ParseCodePoint(std::string_view(key).substr(0, comma), &a) ||
                !ParseCodePoint(std::string_view(key).substr(comma + 1), &b))
                return Fail(why, "metrics: kern key \"" + key + "\" must be \"<code point>,<code point>\"");
            if (!dx.IsNumber() || !(std::fabs(dx.number) <= 1024)) return Fail(why, "metrics: kern " + key + " must be a number from -1024 to 1024");
            out->kern[(static_cast<uint64_t>(a) << 32) | b] = static_cast<float>(dx.number);
        }
    }
    return true;
}

bool CheckAtlas(ModFontData* font, int atlasW, int atlasH, std::string* why) {
    if (atlasW <= 0 || atlasH <= 0 || atlasW > kMaxAtlasSide || atlasH > kMaxAtlasSide)
        return Fail(why, "the atlas must be at most " + std::to_string(kMaxAtlasSide) + "x" + std::to_string(kMaxAtlasSide) +
                             " (it is " + std::to_string(atlasW) + "x" + std::to_string(atlasH) + ")");
    for (const auto& [cp, g] : font->glyphs)
        if (g.x + g.w > atlasW || g.y + g.h > atlasH)
            return Fail(why, "metrics: glyph " + std::to_string(cp) + " lies outside the atlas");
    font->atlasW = atlasW;
    font->atlasH = atlasH;
    return true;
}

bool ValidateModFont(std::string_view json, int atlasW, int atlasH, std::string* why) {
    ModFontData d;
    return ParseModFont(json, &d, why) && CheckAtlas(&d, atlasW, atlasH, why);
}

bool ModFontSource::Glyph(uint32_t cp, float sizePx, GlyphMetrics* out) const {
    auto it = d_.glyphs.find(cp);
    if (it == d_.glyphs.end()) return false;
    const ModGlyph& g = it->second;
    const float f = Factor(sizePx);
    out->x0 = g.xoff * f;
    out->y0 = (d_.ascent + g.yoff) * f;
    out->x1 = out->x0 + g.w * f;
    out->y1 = out->y0 + g.h * f;
    out->advance = g.adv * f;
    out->visible = g.w > 0 && g.h > 0 && d_.atlasW > 0 && d_.atlasH > 0;
    if (out->visible) {
        out->u0 = g.x / d_.atlasW;
        out->v0 = g.y / d_.atlasH;
        out->u1 = (g.x + g.w) / d_.atlasW;
        out->v1 = (g.y + g.h) / d_.atlasH;
    }
    return true;
}

float ModFontSource::Kern(uint32_t a, uint32_t b, float sizePx) const {
    if (d_.kern.empty()) return 0;
    auto it = d_.kern.find((static_cast<uint64_t>(a) << 32) | b);
    return it == d_.kern.end() ? 0 : it->second * Factor(sizePx);
}

float ModFontSource::LineHeight(float sizePx) const { return d_.lineHeight * Factor(sizePx); }
}  // namespace melange::mirage::drawfont
