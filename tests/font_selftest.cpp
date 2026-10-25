// Offline self-test for mod fonts: holo-font-1 metrics parse, UTF-8 layout, missing glyphs, kerning, measure and
// ellipsis (render/mirage/draw_font_core.h).
// Owner: C5 (Melange 0.12.0). Exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "render/mirage/draw_font_core.h"

namespace {
using namespace melange::mirage::drawfont;

int g_pass = 0, g_fail = 0;
void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}
bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// size 10 at scale 2: one atlas pixel is half a window pixel at size 10. A, B, V, '?', '.', ' ' and (optionally) U+2026.
std::string Metrics(bool ellipsisGlyph = true, bool question = true) {
    std::string g = R"("65": [0, 0, 20, 24, 2, -20, 20], "66": [20, 0, 18, 24, 0, -20, 18], "86": [40, 0, 20, 24, 0, -20, 20],)"
                    R"( "46": [60, 0, 4, 4, 0, -4, 6], "32": [0, 0, 0, 0, 0, 0, 8])";
    if (question) g += R"(, "63": [64, 0, 14, 24, 1, -20, 16])";
    if (ellipsisGlyph) g += R"(, "8230": [80, 0, 16, 4, 0, -4, 16])";
    return R"({"format": "holo-font-1", "size": 10, "scale": 2, "lineHeight": 30, "ascent": 22, "outline": 2, "glyphs": {)" + g +
           R"(}, "kern": {"65,86": -4, "46,46": -1}})";
}

bool Load(const std::string& json, ModFontData* d, std::string* why = nullptr) {
    std::string w;
    return ParseModFont(json, d, why ? why : &w) && CheckAtlas(d, 128, 64, why ? why : &w);
}

std::string ParseError(const std::string& json) {
    ModFontData d;
    std::string why;
    if (ParseModFont(json, &d, &why)) return "(parsed)";
    return why;
}

void TestParse() {
    ModFontData d;
    std::string why;
    Check(Load(Metrics(), &d, &why), "the sample parses: " + why);
    Check(d.size == 10 && d.scale == 2 && d.lineHeight == 30 && d.ascent == 22 && d.outline == 2, "header fields");
    Check(d.glyphs.size() == 7 && d.glyphs.count(65) && d.glyphs.at(8230).w == 16, "glyphs by code point");
    Check(d.kern.size() == 2 && d.kern.at((65ull << 32) | 86) == -4, "kern pairs");
    Check(d.atlasW == 128 && d.atlasH == 64, "CheckAtlas records the size");

    const std::string head = R"({"format": "holo-font-1", "size": 10, "lineHeight": 12, "ascent": 9, )";
    ModFontData m;
    Check(ParseModFont(head + R"("glyphs": {"65": [0,0,1,1,0,0,1]}})", &m, &why) && m.scale == 1 && m.outline == 0 && m.kern.empty(),
          "scale, outline and kern are optional");
    struct Bad {
        std::string json, want;
    } bad[] = {
        {"not json", "metrics JSON line 1"},
        {"[1]", "a JSON object expected"},
        {R"({"format": "holo-font-2", "size": 10})", "\"format\" must be"},
        {R"({"format": "holo-font-1", "lineHeight": 12, "ascent": 9, "glyphs": {}})", "\"size\" is missing"},
        {R"({"format": "holo-font-1", "size": 0, "lineHeight": 12, "ascent": 9})", "\"size\" must be a number"},
        {R"({"format": "holo-font-1", "size": 10, "scale": 99, "lineHeight": 12, "ascent": 9})", "\"scale\" must be"},
        {R"({"format": "holo-font-1", "size": 10, "lineHeight": "12", "ascent": 9})", "\"lineHeight\" must be"},
        {R"({"format": "holo-font-1", "size": 10, "lineHeight": 12})", "\"ascent\" is missing"},
        {head + R"("glyphs": {}})", "\"glyphs\" must be a non-empty object"},
        {head + R"("glyphs": [1]})", "\"glyphs\" must be a non-empty object"},
        {head + R"("glyphs": {"A": [0,0,1,1,0,0,1]}})", "glyph key \"A\""},
        {head + R"("glyphs": {"065": [0,0,1,1,0,0,1]}})", "glyph key \"065\""},
        {head + R"("glyphs": {"1114112": [0,0,1,1,0,0,1]}})", "glyph key \"1114112\""},
        {head + R"("glyphs": {"-1": [0,0,1,1,0,0,1]}})", "glyph key \"-1\""},
        {head + R"("glyphs": {"65": [0,0,1,1,0,0]}})", "glyph 65 must be"},
        {head + R"("glyphs": {"65": [0,0,1,1,0,0,"1"]}})", "glyph 65 has a non-number"},
        {head + R"("glyphs": {"65": [-1,0,1,1,0,0,1]}})", "glyph 65 is out of range"},
        {head + R"("glyphs": {"65": [0,0,3000,1,0,0,1]}})", "glyph 65 is out of range"},
        {head + R"("glyphs": {"65": [0,0,1,1,0,0,-1]}})", "glyph 65 is out of range"},
        {head + R"("glyphs": {"65": [0,0,1,1,0,0,1]}, "kern": []})", "\"kern\" must be an object"},
        {head + R"("glyphs": {"65": [0,0,1,1,0,0,1]}, "kern": {"65": 1}})", "kern key \"65\""},
        {head + R"("glyphs": {"65": [0,0,1,1,0,0,1]}, "kern": {"65,x": 1}})", "kern key \"65,x\""},
        {head + R"("glyphs": {"65": [0,0,1,1,0,0,1]}, "kern": {"65,66": 5000}})", "kern 65,66 must be"},
    };
    for (const Bad& b : bad) {
        const std::string e = ParseError(b.json);
        Check(e.find(b.want) != std::string::npos, "refused (" + b.want + "): got " + e);
    }
    std::string many = head + R"("glyphs": {)";
    for (size_t i = 0; i <= kMaxGlyphs; ++i) many += (i ? "," : "") + std::string("\"") + std::to_string(i) + "\": [0,0,1,1,0,0,1]";
    Check(ParseError(many + "}}").find("more than 8192 glyphs") != std::string::npos, "the glyph cap");

    ModFontData a;
    Check(ParseModFont(head + R"("glyphs": {"65": [120,0,10,10,0,0,1]}})", &a, &why), "a wide glyph parses");
    Check(!CheckAtlas(&a, 128, 64, &why) && why.find("outside the atlas") != std::string::npos, "a glyph past the atlas edge");
    Check(CheckAtlas(&a, 130, 64, &why), "the same glyph fits a wider atlas");
    Check(!CheckAtlas(&a, 4096, 64, &why) && why.find("at most 2048x2048") != std::string::npos, "an atlas wider than 2048");
    Check(!CheckAtlas(&a, 0, 0, &why), "an empty atlas");
    Check(ValidateModFont(Metrics(), 128, 64, &why) && !ValidateModFont(Metrics(), 32, 32, &why), "ValidateModFont = parse + atlas");
}

void TestUtf8() {
    struct Case {
        std::string s;
        std::vector<uint32_t> cps;
    } cases[] = {
        {"Az", {'A', 'z'}},
        {"\xC3\xA9", {0xE9}},
        {"\xE2\x80\xA6", {0x2026}},
        {"\xF0\x9F\x98\x80", {0x1F600}},
        {"\xFF" "A", {0xFFFD, 'A'}},
        {"\xC3" "A", {0xFFFD, 'A'}},           // a lead byte without its continuation
        {"\xC0\xAF", {0xFFFD}},                 // overlong '/'
        {"\xED\xA0\x80", {0xFFFD}},             // a surrogate
        {"\xF4\x90\x80\x80", {0xFFFD}},         // past U+10FFFF
        {"\xE2\x80", {0xFFFD}},                 // truncated at the end
        {"\x80" "B", {0xFFFD, 'B'}},            // a stray continuation byte
    };
    for (const Case& c : cases) {
        std::vector<uint32_t> got;
        const char* p = c.s.data();
        const char* end = p + c.s.size();
        while (p < end) {
            uint32_t cp = 0;
            const int n = DecodeUtf8(p, end, &cp);
            if (n < 1) break;
            p += n;
            got.push_back(cp);
        }
        std::string hex;
        for (unsigned char ch : c.s) hex += std::to_string(ch) + " ";
        Check(got == c.cps, "UTF-8 decode of bytes " + hex);
    }
}

void TestLayout() {
    ModFontData d;
    Check(Load(Metrics(), &d), "layout font loads");
    ModFontSource font(d);
    std::vector<GlyphQuad> q;

    // At size 10 the factor is 10 / (10 * 2) = 0.5: A is 10 wide and advances 10, the baseline sits 11 below the top.
    Extent e = LayoutText(font, "A", 10, 100, 50, {}, &q);
    Check(q.size() == 1 && Near(q[0].x0, 101) && Near(q[0].y0, 50 + (22 - 20) * 0.5f) && Near(q[0].x1, 111) && Near(q[0].y1, 51 + 12),
          "glyph quad from xoff, ascent + yoff and the scale");
    Check(Near(q[0].u0, 0) && Near(q[0].u1, 20.f / 128) && Near(q[0].v1, 24.f / 64), "glyph uv from the atlas size");
    Check(Near(e.w, 10) && Near(e.h, 15), "extent: advance by lineHeight * factor");

    q.clear();
    e = LayoutText(font, "AV", 10, 0, 0, {}, &q);
    Check(q.size() == 2 && Near(q[1].x0, 10 - 2), "kerning moves the second glyph (65,86: -4 atlas px)");
    Check(Near(e.w, 10 + 10 - 2), "kerning counts in the width");
    q.clear();
    e = LayoutText(font, "VA", 10, 0, 0, {}, &q);
    Check(Near(e.w, 20), "kerning is per ordered pair");

    q.clear();
    e = LayoutText(font, "A\xE2\x98\x83" "B", 10, 0, 0, {}, &q);  // U+2603 is not in the font
    Check(q.size() == 3 && Near(e.w, 10 + 8 + 9), "a missing glyph draws as '?'");
    ModFontData noq;
    Check(Load(Metrics(true, false), &noq), "font without '?' loads");
    ModFontSource fontNoQ(noq);
    e = LayoutText(fontNoQ, "AzB", 10, 0, 0, {}, nullptr);
    Check(Near(e.w, 19), "a missing glyph is skipped when '?' is missing too");

    q.clear();
    e = LayoutText(font, "A A", 10, 0, 0, {}, &q);
    Check(q.size() == 2 && Near(e.w, 24), "a space advances without a quad");
    e = LayoutText(font, "A\tB\x01", 10, 0, 0, {}, nullptr);
    Check(Near(e.w, 19), "control characters are skipped");

    q.clear();
    e = LayoutText(font, "AB\nA", 10, 0, 0, {}, &q);
    Check(q.size() == 3 && Near(q[2].y0, 15 + 1) && Near(q[2].x0, 1), "a newline starts the next line at the left");
    Check(Near(e.w, 19) && Near(e.h, 30), "extent: widest line, two lines");
    e = LayoutText(font, "A\n", 10, 0, 0, {}, nullptr);
    Check(Near(e.h, 30), "a trailing newline is an empty second line");
    e = LayoutText(font, "", 10, 0, 0, {}, nullptr);
    Check(e.w == 0 && e.h == 0, "empty text has no extent");
    e = LayoutText(font, nullptr, 10, 0, 0, {}, nullptr);
    Check(e.w == 0 && e.h == 0, "null text has no extent");
    e = LayoutText(font, "A", 0, 0, 0, {}, nullptr);
    Check(e.w == 0, "size 0 draws nothing");
    e = LayoutText(font, "A", 20, 0, 0, {}, nullptr);
    Check(Near(e.w, 20) && Near(e.h, 30), "size 20 doubles everything");

    LayoutOptions centre;
    centre.align = Align::Center;
    q.clear();
    LayoutText(font, "AB\nA", 10, 100, 0, centre, &q);
    Check(q.size() == 3 && Near(q[0].x0, 100 - 9.5f + 1) && Near(q[2].x0, 100 - 5 + 1), "centre aligns each line on x");
    LayoutOptions right;
    right.align = Align::Right;
    q.clear();
    e = LayoutText(font, "AB", 10, 100, 0, right, &q);
    Check(q.size() == 2 && Near(q[1].x0 - 0, 100 - 9) && Near(e.w, 19), "right ends the line at x");

    // Cutting: A=10, B=9, V=10, ellipsis glyph 8 wide.
    LayoutOptions cut;
    cut.maxWidth = 30;
    q.clear();
    e = LayoutText(font, "ABVAB", 10, 0, 0, cut, &q);
    Check(Near(e.w, 10 + 9 + 8) && q.size() == 3, "maxWidth keeps what fits before the ellipsis");
    Check(Near(q[2].x0, 19) && Near(q[2].u0, 80.f / 128), "the ellipsis glyph ends the line");
    e = LayoutText(font, "ABV", 10, 0, 0, cut, nullptr);
    Check(Near(e.w, 29), "a line that fits is not cut");
    cut.ellipsis = false;
    e = LayoutText(font, "ABVAB", 10, 0, 0, cut, nullptr);
    Check(Near(e.w, 29), "without ellipsis the line is cut at the last glyph that fits");
    cut.ellipsis = true;
    cut.maxWidth = 6;
    q.clear();
    e = LayoutText(font, "ABV", 10, 0, 0, cut, &q);
    Check(q.empty() && e.w == 0, "nothing when even the ellipsis does not fit");
    cut.maxWidth = 23;  // "A " + ellipsis = 22 fits, but the space is trimmed
    q.clear();
    e = LayoutText(font, "A ABV", 10, 0, 0, cut, &q);
    Check(Near(e.w, 18) && q.size() == 2, "spaces before the ellipsis are trimmed");
    cut.maxWidth = 22;
    ModFontData dots;
    Check(Load(Metrics(false), &dots), "font without U+2026 loads");
    ModFontSource fontDots(dots);
    q.clear();
    e = LayoutText(fontDots, "ABVAB", 10, 0, 0, cut, &q);
    // three dots of 3 with -0.5 kerning between them: 8 wide; A (10) fits, A+B does not.
    Check(q.size() == 4 && Near(e.w, 10 + 3 + 2.5f + 2.5f), "without U+2026 the ellipsis is three kerned dots");
    LayoutOptions cutRight;
    cutRight.align = Align::Right;
    cutRight.maxWidth = 30;
    q.clear();
    LayoutText(font, "ABVAB", 10, 100, 0, cutRight, &q);
    Check(!q.empty() && Near(q.back().x0, 100 - 8), "alignment uses the cut width");
    LayoutOptions cutLines;
    cutLines.maxWidth = 20;
    e = LayoutText(font, "ABV\nA", 10, 0, 0, cutLines, nullptr);
    Check(Near(e.w, 18) && Near(e.h, 30), "each line is cut on its own");

    std::string longText(kMaxLayoutBytes + 100, 'A');
    e = LayoutText(font, longText.c_str(), 10, 0, 0, {}, nullptr);
    Check(Near(e.w, kMaxLayoutBytes * 10.f), "text past kMaxLayoutBytes is ignored");
}
}  // namespace

int main() {
    TestParse();
    TestUtf8();
    TestLayout();
    printf("font_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
