#pragma once
#include <cstdint>
#include "melange/render.h"
namespace melange::draw {
using Rgba = uint32_t;  // 0xAABBGGRR, as ImGui's IM_COL32
enum Flags : uint32_t {
    kDepthTest = 1,     // world only; default on
    kDepthWrite = 2,
    kAdditive = 4,
    kNoCull = 8,
    kScreenSize = 16,   // text/point size in pixels regardless of distance (world text)
};
constexpr uint32_t kDefaultWorld = kDepthTest;

// Called outside a stage callback, primitives are queued and drawn at that stage in the next main pass, then
// dropped unless `frames` > 1. Called inside a stage callback, they are drawn at that stage right after the
// callback returns. Main thread only.
void Line(const float a[3], const float b[3], Rgba c, float widthPx = 2.f, uint32_t flags = kDefaultWorld, int frames = 1);
void Box(const float mn[3], const float mx[3], Rgba c, float widthPx = 2.f, uint32_t flags = kDefaultWorld, int frames = 1);
void Sphere(const float center[3], float radius, Rgba c, float widthPx = 2.f, uint32_t flags = kDefaultWorld, int frames = 1);
void Axes(const float origin[3], float length, float widthPx = 2.f, uint32_t flags = kDefaultWorld, int frames = 1);
void Quad(const float p[4][3], Rgba c, uint32_t flags = kDefaultWorld, int frames = 1);  // filled
void Text(const float anchor[3], const char* utf8, Rgba c, float sizePx = 16.f,
          uint32_t flags = kDefaultWorld | kScreenSize, int frames = 1);  // screen-aligned, centred above the anchor

struct Vertex { float pos[3]; Rgba color; float uv[2]; };
// model: float[16] glLoadMatrixf layout or nullptr; texture: a GL texture name (0 = untextured).
void Mesh(const Vertex* v, uint32_t nv, const uint16_t* idx, uint32_t ni, const float* model, unsigned texture,
          uint32_t flags = kDefaultWorld, int frames = 1);

// Textured world sprite. With a zero axis it is a round billboard facing the camera (2*halfW square, halfL unused).
// Otherwise it is a view-facing quad 2*halfW wide whose long side follows the screen projection of `axis`, from
// pos - axis*halfL (texture v = 0) to pos + axis*halfL (v = 1); u runs 0..1 across it. A stretched sprite never looks
// shorter than min(halfW, halfL) on screen, so one flying straight at the camera does not collapse to a sliver.
enum class SpriteBlend : uint8_t {
    Alpha,          // straight alpha: SRC_ALPHA, ONE_MINUS_SRC_ALPHA
    Premultiplied,  // ONE, ONE_MINUS_SRC_ALPHA; the tint is premultiplied by its own alpha
    Additive,       // SRC_ALPHA, ONE
};
struct Sprite {
    float pos[3];
    float halfW, halfL;
    float axis[3];     // world space, any length (normalised); zero = billboard
    Rgba color;        // tint, multiplied with the texture
    unsigned texture;  // GL texture name, e.g. from LoadTexture (0 = untextured)
    SpriteBlend blend;
};
constexpr uint32_t kMaxSpritesPerStage = 65536;  // all callers together, per stage and frame
// Only inside a World or WorldLate draw callback: drawn at that stage right after its other primitives, depth-tested
// without depth writes or culling, sorted back to front by view depth (ties keep submission order). False (dropped)
// outside such a callback or past kMaxSpritesPerStage. Non-finite values or halfW <= 0 are skipped and return true.
bool DrawSprite(const Sprite& s);
// The stage whose draw callbacks are running now (World, WorldLate or Hud), or Stage::Count outside them.
render::Stage CurrentStage();
// Increments once per main pass in which any draw stage ran; a per-frame key for callers' own budgets.
uint64_t FrameSerial();

// HUD: window pixels, origin top-left; drawn at Stage::Hud (above the engine HUD).
void HudLine(float x0, float y0, float x1, float y1, Rgba c, float widthPx = 1.f, int frames = 1);
void HudRect(float x0, float y0, float x1, float y1, Rgba c, bool filled, float widthPx = 1.f, int frames = 1);
void HudText(float x, float y, const char* utf8, Rgba c, float sizePx = 16.f, int frames = 1);
void HudImage(float x0, float y0, float x1, float y1, unsigned texture, Rgba tint = 0xffffffff, int frames = 1);

// A PNG loaded into a GL texture (RGBA8, trilinear with a full mip chain, clamp). 0 on failure. Mips average in
// premultiplied space, unless every pixel already has r, g, b <= a (a premultiplied PNG), which averages as stored.
unsigned LoadTexture(const wchar_t* pngPath);
void FreeTexture(unsigned texture);

// Per-frame drawing: `fn` runs at `stage` every main pass; draw calls inside it are immediate.
using DrawFn = void (*)(render::Stage stage, void* user);
int AddDrawCallback(render::Stage stage, DrawFn fn, void* user, int order = 0);  // World, WorldLate, Hud only
void RemoveDrawCallback(int handle);

struct Stats { uint32_t primitives, vertices, callbacks; double cpuMs, gpuMs; };
Stats GetStats();
}
