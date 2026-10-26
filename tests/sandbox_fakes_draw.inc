// sandbox_selftest fakes for the Melange 0.12 wum.draw functions (owner: C5): extend namespace draw with the new
// queue entry points (UV images, nine-slice, clip, fonts, video) and media::FrameForDraw. Included inside namespace
// melange, after the other fakes.
namespace mirage::drawfont {
bool ValidateModFont(std::string_view json, int atlasW, int atlasH, std::string* why);  // draw_font_core.cpp (real)
}
namespace draw {
struct UvCall {
    float r[4];
    unsigned tex;
    float uv[4];
    Rgba tint;
};
std::vector<UvCall> g_uvCalls;
void HudImageUV(float x0, float y0, float x1, float y1, unsigned tex, float u0, float v0, float u1, float v1, Rgba tint, int) {
    g_uvCalls.push_back({{x0, y0, x1, y1}, tex, {u0, v0, u1, v1}, tint});
}
struct NineCall {
    float r[4];
    unsigned tex;
    float insets[4];
    NineOptions o;
};
std::vector<NineCall> g_nineCalls;
void HudNine(float x0, float y0, float x1, float y1, unsigned tex, float l, float t, float r, float b, const NineOptions& o, int) {
    g_nineCalls.push_back({{x0, y0, x1, y1}, tex, {l, t, r, b}, o});
}
struct TextExCall {
    std::string text;
    float size;
    Rgba color;
    TextOptions o;
};
std::vector<TextExCall> g_textEx;
void HudTextEx(float, float, const char* utf8, Rgba c, float sizePx, const TextOptions& o, int) {
    g_textEx.push_back({utf8, sizePx, c, o});
}
// Half the size per byte, plus the font id, so a test can tell which font measured.
TextExtent MeasureText(const char* utf8, float sizePx, int font) {
    return {static_cast<float>(strlen(utf8)) * sizePx * 0.5f + static_cast<float>(font), sizePx};
}
int g_nextFont = 1, g_liveFonts = 0;
// The real holo-font-1 checks against a 512x512 atlas (huge.png: 4096x4096; missing.png: unreadable).
int LoadFont(const wchar_t* png, const char* json, size_t len, std::string* why) {
    if (wcsstr(png, L"missing.png")) {
        *why = "atlas: cannot read the PNG";
        return 0;
    }
    const int side = wcsstr(png, L"huge.png") ? 4096 : 512;
    if (!mirage::drawfont::ValidateModFont(std::string_view(json, len), side, side, why)) return 0;
    ++g_liveFonts;
    return g_nextFont++;
}
void FreeFont(int) { --g_liveFonts; }
int g_clipDepth = 0;
bool HudClip(float, float, float, float) {
    if (g_stage != render::Stage::Hud && g_stage != render::Stage::Screen) return false;
    return ++g_clipDepth <= 4;
}
void HudClipEnd() {
    if (g_clipDepth > 0) --g_clipDepth;
}
struct VideoCall {
    float r[4];
    unsigned tex;
    float uv[4];
    float alpha;
};
std::vector<VideoCall> g_video;
void HudVideo(float x0, float y0, float x1, float y1, unsigned tex, float u0, float v0, float u1, float v1, float alpha, int) {
    g_video.push_back({{x0, y0, x1, y1}, tex, {u0, v0, u1, v1}, alpha});
}
}  // namespace draw
namespace media {
// A 1280x536 frame (2.39:1) in texture 77, unless g_frameOk is false.
bool g_frameOk = true;
std::string g_frameMod;
uint32_t g_framePlayer = 0;
int g_frameCalls = 0;
bool FrameForDraw(const char* modId, uint32_t playerId, DrawFrame* out) {
    ++g_frameCalls;
    g_frameMod = modId;
    g_framePlayer = playerId;
    if (!g_frameOk) return false;
    out->texture = 77;
    out->w = 1280;
    out->h = 536;
    out->u1 = 1.f;
    out->v1 = 536.f / 720.f;
    out->aspect = 1280.0 / 536.0;
    return true;
}
}  // namespace media
