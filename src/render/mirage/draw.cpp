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
#include <vector>

#include "melange/render.h"
#include "melange/testcmd.h"
#include "render/mirage/draw_gl.h"
#include "render/mirage/draw_queue.h"

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

constexpr int kNumStages = 3;
constexpr render::Stage kStageOf[kNumStages] = {render::Stage::World, render::Stage::WorldLate, render::Stage::Hud};

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
};

struct StageState {
    StageQueue queue;
    int enterHandle = 0, exitHandle = 0;
    std::vector<Primitive>* activePass = nullptr;
    std::vector<DrawCb> callbacks;
};
StageState g_stage[kNumStages];

// Enter/Exit bracket every other callback of the stage (orders INT_MIN/INT_MAX); calls between them are immediate.
bool g_insideStage = false;
render::Stage g_currentStage = render::Stage::Count;
int g_nextHandle = 1;

uint64_t g_statsFrame = 0;
Stats g_stats{};
Stats g_frameAccum{};

void EnterCb(render::Stage stage, void* user) {
    StageState* s = static_cast<StageState*>(user);
    g_insideStage = true;
    g_currentStage = stage;
    s->activePass = &s->queue.BeginPass();
    std::vector<DrawCb> sorted = s->callbacks;
    std::stable_sort(sorted.begin(), sorted.end(), [](const DrawCb& a, const DrawCb& b) { return a.order < b.order; });
    for (auto& cb : sorted) cb.fn(stage, cb.user);
}

void MaybeUnregister(int idx);

void ExitCb(render::Stage stage, void* user) {
    StageState* s = static_cast<StageState*>(user);
    mirage::drawgl::FrameStats fs = mirage::drawgl::DrawStage(stage, *s->activePass, s->queue.Immediate());

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
    if (s.enterHandle) return;
    render::Stage st = kStageOf[idx];
    s.enterHandle = render::AddStageCallback(st, &EnterCb, &s, INT_MIN);
    s.exitHandle = render::AddStageCallback(st, &ExitCb, &s, INT_MAX);
}

void MaybeUnregister(int idx) {
    StageState& s = g_stage[idx];
    if (!s.enterHandle || !s.queue.Idle() || !s.callbacks.empty()) return;
    render::RemoveStageCallback(s.enterHandle);
    render::RemoveStageCallback(s.exitHandle);
    s.enterHandle = s.exitHandle = 0;
}

void Submit(Kind kind, uint32_t flags, Primitive p) {
    render::Stage stage = RouteStage(kind, flags);
    int idx = IndexOf(stage);
    if (idx < 0) return;
    EnsureRegistered(idx);
    bool insideThisStage = g_insideStage && g_currentStage == stage;
    g_stage[idx].queue.Push(std::move(p), insideThisStage);
}

int ClampFrames(int frames) { return frames < 1 ? 1 : frames; }
}  // namespace

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

unsigned LoadTexture(const wchar_t* pngPath) {
    if (!pngPath) return 0;
    FILE* f = _wfopen(pngPath, L"rb");
    if (!f) return 0;
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_file(f, &w, &h, &channels, STBI_rgb_alpha);
    fclose(f);
    if (!pixels) return 0;

    unsigned texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(pixels);
    return texture;
}

void FreeTexture(unsigned texture) {
    if (!texture) return;
    GLuint t = texture;
    glDeleteTextures(1, &t);
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

Stats GetStats() {
    Stats s = g_stats;
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
    melange::testcmd::Register("draw.stats", &VerbStats);
    return true;
}
}  // namespace
