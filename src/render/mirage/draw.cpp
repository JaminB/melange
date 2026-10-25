// Module "MirageDraw": world and HUD drawing API (melange/draw.h).
#include "core/log.h"
#include "core/module.h"
#include "melange/draw.h"

#include <windows.h>
#include <GL/gl.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "melange/render.h"
#include "melange/testcmd.h"
#include "render/mirage/draw_font.h"
#include "render/mirage/draw_gl.h"
#include "render/mirage/draw_queue.h"
#include "render/mirage/draw_sprites.h"
#include "render/mirage/shadertex_logic.h"
#include "render/mirage/stages.h"

namespace {
class MirageDraw final : public melange::Module {
  public:
    const char* Name() const override { return "MirageDraw"; }
    const char* Description() const override { return "draw API: world and HUD primitives, text"; }
    int Order() const override { return 44; }
    bool Install() override;
};
}  // namespace

MELANGE_MODULE(MirageDraw);

namespace melange::draw {
namespace {
using mirage::drawqueue::Kind;
using mirage::drawqueue::Primitive;
using mirage::drawqueue::RouteStage;
using mirage::drawqueue::StageQueue;

// Screen (Melange 0.12) has no engine stage callbacks: RunScreenPass, called from the present hook, runs it.
constexpr int kNumStages = 4;
constexpr render::Stage kStageOf[kNumStages] = {render::Stage::World, render::Stage::WorldLate, render::Stage::Hud,
                                                render::Stage::Screen};
constexpr int kScreenIdx = 3;

int IndexOf(render::Stage s) {
    for (int i = 0; i < kNumStages; ++i)
        if (kStageOf[i] == s) return i;
    return -1;
}

struct DrawCb {
    int handle;
    int order;
    DrawFn fn;
    void* user;
    int faults = 0;
};

struct StageState {
    StageQueue queue;
    int enterHandle = 0, exitHandle = 0;
    std::vector<Primitive>* activePass = nullptr;
    std::vector<DrawCb> callbacks;
    std::vector<Sprite> sprites;  // this pass only (DrawSprite is immediate-only), drawn after the primitives
};
StageState g_stage[kNumStages];

// Enter/Exit bracket every other callback of the stage (orders INT_MIN/INT_MAX); calls between them are immediate.
bool g_insideStage = false;
render::Stage g_currentStage = render::Stage::Count;
int g_nextHandle = 1;
// FrameSerial(): stages run World, WorldLate, Hud, Screen within a frame, so entering one at or before the last
// entered index starts a new pass.
uint64_t g_frameSerial = 0;
int g_lastEnterIdx = kNumStages;

// hudClip pushes made by the running draw callback (ended for it when it returns), including those past the depth cap.
int g_clipDepth = 0;

uint64_t g_statsFrame = 0;
Stats g_stats{};
Stats g_frameAccum{};
std::unordered_map<unsigned, TextureInfo> g_texInfo;  // every live LoadTexture[Ex] texture

// A user draw callback faulting must not count against the Enter/Exit stage callbacks themselves (that would get
// them removed by stages::Fault after 3 faults from any mod, wedging the whole stage; see MaybeUnregister/
// EnsureRegistered below), so each one is isolated here instead of relying on stages.cpp's own fault handling.
bool InvokeDrawCb(DrawFn fn, render::Stage stage, void* user, unsigned long* code) {
    __try {
        fn(stage, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

const char* StageLabel(render::Stage stage) { return stage == render::Stage::Screen ? "Screen" : mirage::stages::Name(stage); }

void FaultDrawCb(StageState* s, int handle, render::Stage stage, unsigned long code) {
    auto it = std::find_if(s->callbacks.begin(), s->callbacks.end(), [&](const DrawCb& c) { return c.handle == handle; });
    if (it == s->callbacks.end()) return;
    ++it->faults;
    LOG_ERROR("[draw] %s draw callback %d raised exception 0x%08lx (%d/3)", StageLabel(stage), handle, code, it->faults);
    if (it->faults >= 3) {
        LOG_ERROR("[draw] %s draw callback %d removed after 3 faults", StageLabel(stage), handle);
        s->callbacks.erase(it);
    }
}

void Submit(Kind kind, uint32_t flags, Primitive p);

// Ends the clips the callback left open, so they never leak into the next callback's primitives.
void CloseClips() {
    for (; g_clipDepth > 0; --g_clipDepth) {
        Primitive p;
        p.kind = Kind::ClipPop;
        Submit(Kind::ClipPop, 0, std::move(p));
    }
}

void EnterCb(render::Stage stage, void* user) {
    StageState* s = static_cast<StageState*>(user);
    const int idx = IndexOf(stage);
    if (idx <= g_lastEnterIdx) ++g_frameSerial;
    g_lastEnterIdx = idx;
    g_insideStage = true;
    g_currentStage = stage;
    s->sprites.clear();
    s->activePass = &s->queue.BeginPass();
    std::vector<DrawCb> sorted = s->callbacks;
    std::stable_sort(sorted.begin(), sorted.end(), [](const DrawCb& a, const DrawCb& b) { return a.order < b.order; });
    for (auto& cb : sorted) {
        unsigned long code = 0;
        g_clipDepth = 0;
        if (!InvokeDrawCb(cb.fn, stage, cb.user, &code)) FaultDrawCb(s, cb.handle, stage, code);
        CloseClips();
    }
}

void MaybeUnregister(int idx);

void ExitCb(render::Stage stage, void* user) {
    StageState* s = static_cast<StageState*>(user);
    // activePass is null if EnterCb did not run this frame (e.g. its stage callback was itself removed after
    // faulting); draw whatever was queued for immediate drawing and otherwise treat the pass as empty.
    static std::vector<Primitive> kEmptyPass;
    std::vector<Primitive>& pass = s->activePass ? *s->activePass : kEmptyPass;
    mirage::drawgl::FrameStats fs = mirage::drawgl::DrawStage(stage, pass, s->queue.Immediate(), s->sprites);
    s->sprites.clear();

    uint64_t frame = render::GetTiming().frames;
    if (frame != g_statsFrame) {
        g_stats = g_frameAccum;
        g_frameAccum = Stats{};
        g_statsFrame = frame;
    }
    g_frameAccum.primitives += fs.primitives;
    g_frameAccum.vertices += fs.vertices;

    s->queue.EndPass();
    s->activePass = nullptr;
    g_insideStage = false;
    g_currentStage = render::Stage::Count;
    int idx = IndexOf(stage);
    if (idx >= 0) MaybeUnregister(idx);
}

void EnsureRegistered(int idx) {
    StageState& s = g_stage[idx];
    if (s.enterHandle || idx == kScreenIdx) return;
    render::Stage st = kStageOf[idx];
    s.enterHandle = render::AddStageCallback(st, &EnterCb, &s, INT_MIN);
    s.exitHandle = render::AddStageCallback(st, &ExitCb, &s, INT_MAX);
}

void MaybeUnregister(int idx) {
    StageState& s = g_stage[idx];
    if (idx == kScreenIdx || !s.enterHandle || !s.queue.Idle() || !s.callbacks.empty()) return;
    render::RemoveStageCallback(s.enterHandle);
    render::RemoveStageCallback(s.exitHandle);
    s.enterHandle = s.exitHandle = 0;
}

void Submit(Kind kind, uint32_t flags, Primitive p) {
    const render::Stage stage = RouteStage(kind, flags, g_insideStage ? g_currentStage : render::Stage::Count);
    // Mirage off (ini or an unrecognised exe) means AddStageCallback can never succeed; queuing anyway would grow
    // without bound for a mod that keeps drawing every frame (EnsureRegistered just retries and fails again). The
    // screen stage needs no engine stage: the present hook drains it every frame.
    if (stage != render::Stage::Screen && !mirage::stages::Enabled()) return;
    int idx = IndexOf(stage);
    if (idx < 0) return;
    EnsureRegistered(idx);
    bool insideThisStage = g_insideStage && g_currentStage == stage;
    g_stage[idx].queue.Push(std::move(p), insideThisStage);
}

int ClampFrames(int frames) { return frames < 1 ? 1 : frames; }
}  // namespace

bool DrawSprite(const Sprite& s) {
    if (!g_insideStage || (g_currentStage != render::Stage::World && g_currentStage != render::Stage::WorldLate)) return false;
    const int idx = IndexOf(g_currentStage);
    if (idx < 0) return false;
    std::vector<Sprite>& v = g_stage[idx].sprites;
    if (v.size() >= kMaxSpritesPerStage) return false;
    if (mirage::drawsprites::Drawable(s)) v.push_back(s);
    return true;
}

render::Stage CurrentStage() { return g_insideStage ? g_currentStage : render::Stage::Count; }

uint64_t FrameSerial() { return g_frameSerial; }

void Line(const float a[3], const float b[3], Rgba c, float widthPx, uint32_t flags, int frames) {
    Primitive p;
    p.kind = Kind::Line;
    p.flags = flags;
    p.color = c;
    p.widthPx = widthPx;
    p.framesLeft = ClampFrames(frames);
    for (int i = 0; i < 3; ++i) {
        p.p[0][i] = a[i];
        p.p[1][i] = b[i];
    }
    Submit(Kind::Line, flags, std::move(p));
}

void Box(const float mn[3], const float mx[3], Rgba c, float widthPx, uint32_t flags, int frames) {
    Primitive p;
    p.kind = Kind::Box;
    p.flags = flags;
    p.color = c;
    p.widthPx = widthPx;
    p.framesLeft = ClampFrames(frames);
    for (int i = 0; i < 3; ++i) {
        p.p[0][i] = mn[i];
        p.p[1][i] = mx[i];
    }
    Submit(Kind::Box, flags, std::move(p));
}

void Sphere(const float center[3], float radius, Rgba c, float widthPx, uint32_t flags, int frames) {
    Primitive p;
    p.kind = Kind::Sphere;
    p.flags = flags;
    p.color = c;
    p.widthPx = widthPx;
    p.scalar = radius;
    p.framesLeft = ClampFrames(frames);
    for (int i = 0; i < 3; ++i) p.p[0][i] = center[i];
    Submit(Kind::Sphere, flags, std::move(p));
}

void Axes(const float origin[3], float length, float widthPx, uint32_t flags, int frames) {
    Primitive p;
    p.kind = Kind::Axes;
    p.flags = flags;
    p.widthPx = widthPx;
    p.scalar = length;
    p.framesLeft = ClampFrames(frames);
    for (int i = 0; i < 3; ++i) p.p[0][i] = origin[i];
    Submit(Kind::Axes, flags, std::move(p));
}

void Quad(const float p4[4][3], Rgba c, uint32_t flags, int frames) {
    Primitive p;
    p.kind = Kind::Quad;
    p.flags = flags;
    p.color = c;
    p.framesLeft = ClampFrames(frames);
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) p.p[i][k] = p4[i][k];
    Submit(Kind::Quad, flags, std::move(p));
}

void Text(const float anchor[3], const char* utf8, Rgba c, float sizePx, uint32_t flags, int frames) {
    Primitive p;
    p.kind = Kind::Text;
    p.flags = flags;
    p.color = c;
    p.sizePx = sizePx;
    p.framesLeft = ClampFrames(frames);
    for (int i = 0; i < 3; ++i) p.p[0][i] = anchor[i];
    p.text = utf8 ? utf8 : "";
    Submit(Kind::Text, flags, std::move(p));
}

void Mesh(const Vertex* v, uint32_t nv, const uint16_t* idx, uint32_t ni, const float* model, unsigned texture,
          uint32_t flags, int frames) {
    Primitive p;
    p.kind = Kind::Mesh;
    p.flags = flags;
    p.framesLeft = ClampFrames(frames);
    p.texture = texture;
    if (v && nv) p.meshV.assign(v, v + nv);
    if (idx && ni) p.meshI.assign(idx, idx + ni);
    if (model) {
        p.hasModel = true;
        for (int i = 0; i < 16; ++i) p.model[i] = model[i];
    }
    Submit(Kind::Mesh, flags, std::move(p));
}

void HudLine(float x0, float y0, float x1, float y1, Rgba c, float widthPx, int frames) {
    Primitive p;
    p.kind = Kind::HudLine;
    p.color = c;
    p.widthPx = widthPx;
    p.framesLeft = ClampFrames(frames);
    p.p[0][0] = x0;
    p.p[0][1] = y0;
    p.p[1][0] = x1;
    p.p[1][1] = y1;
    Submit(Kind::HudLine, 0, std::move(p));
}

void HudRect(float x0, float y0, float x1, float y1, Rgba c, bool filled, float widthPx, int frames) {
    Primitive p;
    p.kind = Kind::HudRect;
    p.color = c;
    p.filled = filled;
    p.widthPx = widthPx;
    p.framesLeft = ClampFrames(frames);
    p.p[0][0] = x0;
    p.p[0][1] = y0;
    p.p[1][0] = x1;
    p.p[1][1] = y1;
    Submit(Kind::HudRect, 0, std::move(p));
}

void HudText(float x, float y, const char* utf8, Rgba c, float sizePx, int frames) {
    Primitive p;
    p.kind = Kind::HudText;
    p.color = c;
    p.sizePx = sizePx;
    p.framesLeft = ClampFrames(frames);
    p.p[0][0] = x;
    p.p[0][1] = y;
    p.text = utf8 ? utf8 : "";
    Submit(Kind::HudText, 0, std::move(p));
}

void HudImage(float x0, float y0, float x1, float y1, unsigned texture, Rgba tint, int frames) {
    Primitive p;
    p.kind = Kind::HudImage;
    p.color2 = tint;
    p.texture = texture;
    p.framesLeft = ClampFrames(frames);
    p.p[0][0] = x0;
    p.p[0][1] = y0;
    p.p[1][0] = x1;
    p.p[1][1] = y1;
    Submit(Kind::HudImage, 0, std::move(p));
}

namespace {
Primitive UvPrimitive(Kind kind, float x0, float y0, float x1, float y1, unsigned texture, float u0, float v0, float u1,
                      float v1, int frames) {
    Primitive p;
    p.kind = kind;
    p.texture = texture;
    p.framesLeft = ClampFrames(frames);
    p.p[0][0] = x0;
    p.p[0][1] = y0;
    p.p[1][0] = x1;
    p.p[1][1] = y1;
    p.uv[0] = u0;
    p.uv[1] = v0;
    p.uv[2] = u1;
    p.uv[3] = v1;
    return p;
}
}  // namespace

void HudImageUV(float x0, float y0, float x1, float y1, unsigned texture, float u0, float v0, float u1, float v1, Rgba tint,
                int frames) {
    Primitive p = UvPrimitive(Kind::HudImageUV, x0, y0, x1, y1, texture, u0, v0, u1, v1, frames);
    p.color2 = tint;
    Submit(Kind::HudImageUV, 0, std::move(p));
}

void HudNine(float x0, float y0, float x1, float y1, unsigned texture, float l, float t, float r, float b,
             const NineOptions& o, int frames) {
    auto it = g_texInfo.find(texture);
    if (it == g_texInfo.end() || it->second.layers > 0) return;
    mirage::drawqueue::UvQuad cells[9];
    const int n = mirage::drawqueue::NineSlice(x0, y0, x1, y1, it->second.width, it->second.height, l, t, r, b, o.scale, o.uv, cells);
    for (int i = 0; i < n; ++i) {
        const auto& c = cells[i];
        HudImageUV(c.x0, c.y0, c.x1, c.y1, texture, c.u0, c.v0, c.u1, c.v1, o.tint, frames);
    }
}

void HudTextEx(float x, float y, const char* utf8, Rgba c, float sizePx, const TextOptions& o, int frames) {
    Primitive p;
    p.kind = Kind::HudText;
    p.color = c;
    p.sizePx = sizePx;
    p.framesLeft = ClampFrames(frames);
    p.p[0][0] = x;
    p.p[0][1] = y;
    p.text = utf8 ? utf8 : "";
    p.textEx = true;
    p.textOpts = o;
    Submit(Kind::HudText, 0, std::move(p));
}

TextExtent MeasureText(const char* utf8, float sizePx, int font) {
    const mirage::drawfont::Extent e = mirage::drawfont::LayoutEx(font, utf8, sizePx, 0, 0, {}, nullptr);
    return {e.w, e.h};
}

bool HudClip(float x0, float y0, float x1, float y1) {
    if (!g_insideStage || (g_currentStage != render::Stage::Hud && g_currentStage != render::Stage::Screen)) return false;
    const bool real = g_clipDepth < mirage::drawqueue::kMaxClipDepth;
    ++g_clipDepth;
    Primitive p;
    p.kind = Kind::ClipPush;
    p.p[0][0] = x0;
    p.p[0][1] = y0;
    p.p[1][0] = x1;
    p.p[1][1] = y1;
    Submit(Kind::ClipPush, 0, std::move(p));
    return real;
}

void HudClipEnd() {
    if (g_clipDepth <= 0 || !g_insideStage) return;
    --g_clipDepth;
    Primitive p;
    p.kind = Kind::ClipPop;
    Submit(Kind::ClipPop, 0, std::move(p));
}

void HudVideo(float x0, float y0, float x1, float y1, unsigned texture, float u0, float v0, float u1, float v1, float alpha,
              int frames) {
    Primitive p = UvPrimitive(Kind::HudVideo, x0, y0, x1, y1, texture, u0, v0, u1, v1, frames);
    p.scalar = std::clamp(alpha, 0.f, 1.f);
    Submit(Kind::HudVideo, 0, std::move(p));
}

unsigned LoadTexture(const wchar_t* pngPath) {
    if (!pngPath) return 0;
    FILE* f = _wfopen(pngPath, L"rb");
    if (!f) return 0;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_file(f, &w, &h, &channels, STBI_rgb_alpha);
    fclose(f);
    if (!pixels) return 0;

    GLint prev = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    GLint prevAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // RGBA8 rows are always 4-byte aligned; only guards a stray setting
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    const int w0 = w, h0 = h;
    // Full chain down to 1x1 on the CPU (no GL 3 / FBO extension needed), so sprites minify without shimmering.
    const bool premul = mirage::drawsprites::LooksPremultiplied(pixels, static_cast<size_t>(w) * h);
    std::vector<uint8_t> a(pixels, pixels + static_cast<size_t>(w) * h * 4), b;
    for (int level = 1; w > 1 || h > 1; ++level) {
        b.resize(static_cast<size_t>(mirage::drawsprites::MipSize(w)) * mirage::drawsprites::MipSize(h) * 4);
        mirage::drawsprites::Downsample(a.data(), w, h, b.data(), premul);
        w = mirage::drawsprites::MipSize(w);
        h = mirage::drawsprites::MipSize(h);
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, b.data());
        a.swap(b);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    stbi_image_free(pixels);
    g_texInfo[texture] = {GL_TEXTURE_2D, w0, h0, 0, false};
    return texture;
}

namespace {
constexpr GLenum kTexture2DArray = 0x8C1A, kTextureBinding2DArray = 0x8C1D, kClampToEdge = 0x812F, kMaxArrayLayers = 0x88FF,
                 kTextureMaxLevel = 0x813D, kRgba8 = 0x8058;
using TexImage3D = void(APIENTRY*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);

// glTexImage3D, when the context can make texture arrays (GL 3.0, or EXT_texture_array on an older one).
TexImage3D ArrayUpload() {
    static bool tried = false;
    static TexImage3D fn = nullptr;
    if (tried) return fn;
    tried = true;
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    const bool ok = (ver && ver[0] >= '3' && ver[0] <= '9') || (ext && strstr(ext, "GL_EXT_texture_array"));
    PROC p = ok ? wglGetProcAddress("glTexImage3D") : nullptr;
    if (!p && ok) p = wglGetProcAddress("glTexImage3DEXT");
    const auto v = reinterpret_cast<intptr_t>(p);
    fn = (p && v != 1 && v != 2 && v != 3 && v != -1) ? reinterpret_cast<TexImage3D>(p) : nullptr;
    return fn;
}
}  // namespace

unsigned LoadTextureEx(const wchar_t* pngPath, const TextureOptions& o, const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    *why = "cannot read the PNG";
    if (!pngPath) return 0;
    FILE* f = _wfopen(pngPath, L"rb");
    if (!f) return 0;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_file(f, &w, &h, &channels, STBI_rgb_alpha);
    fclose(f);
    if (!pixels) return 0;
    const bool array = o.layers > 0;
    const TexImage3D upload3d = array ? ArrayUpload() : nullptr;
    if (array && !upload3d) {
        stbi_image_free(pixels);
        *why = "this GPU driver has no texture arrays (GL 3.0)";
        return 0;
    }
    GLint maxSize = 2048, maxLayers = 256;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
    if (array) glGetIntegerv(kMaxArrayLayers, &maxLayers);
    const int layers = array ? o.layers : 1;
    int lh = h;
    if (!mirage::shadertex::StripLayout(w, h, layers, maxSize, array ? std::min<int>(maxLayers, mirage::shadertex::kMaxLayers) : 1, &lh, why)) {
        stbi_image_free(pixels);
        return 0;
    }
    const GLenum target = array ? kTexture2DArray : GL_TEXTURE_2D;
    GLint prev = 0, prevAlign = 4;
    glGetIntegerv(array ? kTextureBinding2DArray : GL_TEXTURE_BINDING_2D, &prev);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    while (glGetError() != GL_NO_ERROR) {
    }
    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(target, texture);
    const GLint wrap = o.repeat ? GL_REPEAT : kClampToEdge;
    glTexParameteri(target, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, o.nearest ? GL_NEAREST : o.mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, o.nearest ? GL_NEAREST : GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    auto put = [&](GLint level, int lw, int lhh, const void* px) {
        if (array) upload3d(target, level, kRgba8, lw, lhh, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        else glTexImage2D(target, level, kRgba8, lw, lhh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    };
    put(0, w, lh, pixels);
    if (o.nearest || !o.mips) {
        glTexParameteri(target, kTextureMaxLevel, 0);
    } else {
        const std::vector<mirage::shadertex::Level> mips = mirage::shadertex::LayerMips(pixels, w, lh, layers);
        for (size_t i = 0; i < mips.size(); ++i) put(static_cast<GLint>(i + 1), mips[i].w, mips[i].h, mips[i].px.data());
    }
    const bool glOk = glGetError() == GL_NO_ERROR;
    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
    glBindTexture(target, static_cast<GLuint>(prev));
    stbi_image_free(pixels);
    if (!glOk) {
        glDeleteTextures(1, &texture);
        *why = "the GPU driver refused the texture";
        return 0;
    }
    g_texInfo[texture] = {target, w, lh, array ? layers : 0, o.nearest};
    *why = nullptr;
    return texture;
}

bool GetTextureInfo(unsigned texture, TextureInfo* out) {
    auto it = g_texInfo.find(texture);
    if (it == g_texInfo.end()) return false;
    if (out) *out = it->second;
    return true;
}

void FreeTexture(unsigned texture) {
    if (!texture) return;
    g_texInfo.erase(texture);
    GLuint t = texture;
    glDeleteTextures(1, &t);
}

int LoadFont(const wchar_t* atlasPng, const char* metricsJson, size_t metricsLen, std::string* why) {
    std::string dummy;
    if (!why) why = &dummy;
    mirage::drawfont::ModFontData data;
    if (!metricsJson || !mirage::drawfont::ParseModFont(std::string_view(metricsJson, metricsLen), &data, why)) return 0;
    TextureOptions o;
    o.mips = false;
    const char* texWhy = nullptr;
    const unsigned tex = LoadTextureEx(atlasPng, o, &texWhy);
    if (!tex) {
        *why = std::string("atlas: ") + (texWhy ? texWhy : "cannot read the PNG");
        return 0;
    }
    const TextureInfo& info = g_texInfo[tex];
    if (!mirage::drawfont::CheckAtlas(&data, info.width, info.height, why)) {
        FreeTexture(tex);
        return 0;
    }
    return mirage::drawfont::AddModFont(tex, std::move(data));
}

void FreeFont(int font) {
    if (font <= 0) return;
    if (const unsigned tex = mirage::drawfont::RemoveModFont(font)) FreeTexture(tex);
}

int AddDrawCallback(render::Stage stage, DrawFn fn, void* user, int order) {
    int idx = IndexOf(stage);
    if (idx < 0 || !fn) return 0;
    EnsureRegistered(idx);
    int handle = g_nextHandle++;
    g_stage[idx].callbacks.push_back({handle, order, fn, user});
    return handle;
}

void RemoveDrawCallback(int handle) {
    if (!handle) return;
    for (int i = 0; i < kNumStages; ++i) {
        std::vector<DrawCb>& v = g_stage[i].callbacks;
        auto it = std::find_if(v.begin(), v.end(), [handle](const DrawCb& c) { return c.handle == handle; });
        if (it != v.end()) {
            v.erase(it);
            MaybeUnregister(i);
            return;
        }
    }
}

void RunScreenPass() {
    StageState& s = g_stage[kScreenIdx];
    if (s.callbacks.empty() && s.queue.Idle()) return;
    EnterCb(render::Stage::Screen, &s);
    ExitCb(render::Stage::Screen, &s);
}

Stats GetStats() {
    Stats s = g_stats;
    if (render::GetTiming().frames > g_statsFrame + 1) s.primitives = s.vertices = 0;
    for (int i = 0; i < kNumStages; ++i) s.callbacks += static_cast<uint32_t>(g_stage[i].callbacks.size());
    return s;
}
}  // namespace melange::draw

namespace {
bool VerbStats(std::string_view, void*) {
    melange::draw::Stats s = melange::draw::GetStats();
    LOG_INFO("[draw] stats: primitives=%u vertices=%u callbacks=%u", s.primitives, s.vertices, s.callbacks);
    return true;
}

bool MirageDraw::Install() {
    if (!melange::mirage::stages::CoreEnabled(Name())) return false;
    melange::testcmd::Register("draw.stats", &VerbStats);
    return true;
}
}  // namespace
