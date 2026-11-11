// Module "MirageNoHud": a clean shot (render.shot {clean}, erg-mcp's game_shots and game_camera) hides the engine's own
// HUD too, not only the mods' wum.draw primitives. While draw::HiddenAt(frame) holds (the frames of a clean shot), the
// scene colour is copied aside at PostWorld (after the world and its post-FX, before the worm labels and the engine HUD)
// and copied back at Hud (after the engine HUD), so the frame's Final post-FX run on a picture without names, energy,
// the team bars, the timer or the wind. Only those frames and only the picture: nothing the match reads changes.
#include <windows.h>
#include <GL/gl.h>

#include "core/events.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/render.h"
#include "render/mirage/draw_gl.h"
#include "render/mirage/stages.h"

namespace melange::mirage::nohud {
namespace {
constexpr GLenum kReadFb = 0x8CA8, kDrawFb = 0x8CA9, kReadFbBinding = 0x8CAA, kDrawFbBinding = 0x8CA6, kColor0 = 0x8CE0,
                 kComplete = 0x8CD5, kClampToEdge = 0x812F;
using BindFb = void(APIENTRY*)(GLenum, GLuint);
using GenFb = void(APIENTRY*)(GLsizei, GLuint*);
using DelFb = void(APIENTRY*)(GLsizei, const GLuint*);
using FbTex2D = void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using CheckFb = GLenum(APIENTRY*)(GLenum);
using BlitFb = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
struct Procs {
    BindFb Bind = nullptr;
    GenFb Gen = nullptr;
    DelFb Del = nullptr;
    FbTex2D Tex = nullptr;
    CheckFb Check = nullptr;
    BlitFb Blit = nullptr;
    HGLRC ctx = nullptr;
    bool ok = false;
} g_p;
GLuint g_fbo = 0, g_tex = 0;
int g_w = 0, g_h = 0;
uint64_t g_saved = ~0ull;  // the frame whose scene was copied aside at PostWorld
uint64_t g_hidden = 0;     // frames drawn without the HUD (log)
bool g_warned = false;

template <class T>
void Proc(T& fn, const char* name) {
    PROC p = wglGetProcAddress(name);
    auto v = reinterpret_cast<intptr_t>(p);
    fn = (p && v != 1 && v != 2 && v != 3 && v != -1) ? reinterpret_cast<T>(p) : nullptr;
}

bool Load() {
    HGLRC ctx = wglGetCurrentContext();
    if (!ctx) return false;
    if (ctx != g_p.ctx) {  // a new context: names of the old one are gone
        g_p = Procs{};
        g_p.ctx = ctx;
        g_fbo = g_tex = 0;
        g_w = g_h = 0;
        Proc(g_p.Bind, "glBindFramebuffer");
        Proc(g_p.Gen, "glGenFramebuffers");
        Proc(g_p.Del, "glDeleteFramebuffers");
        Proc(g_p.Tex, "glFramebufferTexture2D");
        Proc(g_p.Check, "glCheckFramebufferStatus");
        Proc(g_p.Blit, "glBlitFramebuffer");
        g_p.ok = g_p.Bind && g_p.Gen && g_p.Del && g_p.Tex && g_p.Check && g_p.Blit;
    }
    return g_p.ok;
}

bool Ensure(int w, int h) {
    if (g_fbo && w == g_w && h == g_h) return true;
    if (g_fbo) g_p.Del(1, &g_fbo);
    if (g_tex) glDeleteTextures(1, &g_tex);
    g_fbo = g_tex = 0;
    glGenTextures(1, &g_tex);
    glBindTexture(GL_TEXTURE_2D, g_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g_p.Gen(1, &g_fbo);
    g_p.Bind(kDrawFb, g_fbo);
    g_p.Tex(kDrawFb, kColor0, GL_TEXTURE_2D, g_tex, 0);
    const bool ok = g_p.Check(kDrawFb) == kComplete;
    g_w = w;
    g_h = h;
    if (!ok && !g_warned) {
        g_warned = true;
        LOG_WARN("[nohud] the copy target is not complete: clean shots keep the engine HUD");
    }
    return ok;
}

void OnStage(render::Stage stage, void*) {
    const uint64_t f = events::FrameCount();
    if (!draw::HiddenAt(f)) return;
    if (stage == render::Stage::Hud && g_saved != f) return;
    const render::SceneTargets t = render::GetSceneTargets();
    if (!t.valid || !t.fbo || t.w <= 0 || t.h <= 0 || !Load()) return;
    const uint32_t token = render::PushState();
    GLint read = 0, draw = 0;
    glGetIntegerv(kReadFbBinding, &read);
    glGetIntegerv(kDrawFbBinding, &draw);
    glDisable(GL_SCISSOR_TEST);
    while (glGetError() != GL_NO_ERROR) {}
    if (stage == render::Stage::PostWorld) {
        if (Ensure(t.w, t.h)) {
            g_p.Bind(kReadFb, t.fbo);
            g_p.Bind(kDrawFb, g_fbo);
            g_p.Blit(0, 0, t.w, t.h, 0, 0, t.w, t.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            g_saved = glGetError() == GL_NO_ERROR ? f : ~0ull;
        }
    } else if (g_w == t.w && g_h == t.h) {
        g_p.Bind(kReadFb, g_fbo);
        g_p.Bind(kDrawFb, t.fbo);
        g_p.Blit(0, 0, t.w, t.h, 0, 0, t.w, t.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (glGetError() == GL_NO_ERROR && (++g_hidden == 1 || g_hidden % 100 == 0))
            LOG_INFO("[nohud] clean shot: frame %llu drawn without the engine HUD (%llu so far)", static_cast<unsigned long long>(f),
                     static_cast<unsigned long long>(g_hidden));
    }
    g_p.Bind(kReadFb, static_cast<GLuint>(read));
    g_p.Bind(kDrawFb, static_cast<GLuint>(draw));
    render::PopState(token);
}

class MirageNoHud final : public melange::Module {
  public:
    const char* Name() const override { return "MirageNoHud"; }
    const char* Description() const override { return "clean shots (render.shot {clean}) hide the engine HUD too"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 47; }
    bool Install() override {
        if (!stages::CoreEnabled(Name())) return false;
        const bool a = render::AddStageCallback(render::Stage::PostWorld, &OnStage, nullptr, (1 << 20) + 1) != 0;
        const bool b = render::AddStageCallback(render::Stage::Hud, &OnStage, nullptr, -(1 << 20)) != 0;
        if (!a || !b) LOG_WARN("[nohud] stage callbacks not added (PostWorld %d, Hud %d): clean shots keep the engine HUD", a, b);
        return a && b;
    }
};
MELANGE_MODULE(MirageNoHud);
}  // namespace
}  // namespace melange::mirage::nohud
