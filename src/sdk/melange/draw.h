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

// HUD: window pixels, origin top-left; drawn at Stage::Hud (above the engine HUD).
void HudLine(float x0, float y0, float x1, float y1, Rgba c, float widthPx = 1.f, int frames = 1);
void HudRect(float x0, float y0, float x1, float y1, Rgba c, bool filled, float widthPx = 1.f, int frames = 1);
void HudText(float x, float y, const char* utf8, Rgba c, float sizePx = 16.f, int frames = 1);
void HudImage(float x0, float y0, float x1, float y1, unsigned texture, Rgba tint = 0xffffffff, int frames = 1);

// A PNG loaded into a GL texture (RGBA8, linear, clamp). 0 on failure.
unsigned LoadTexture(const wchar_t* pngPath);
void FreeTexture(unsigned texture);

// Per-frame drawing: `fn` runs at `stage` every main pass; draw calls inside it are immediate.
using DrawFn = void (*)(render::Stage stage, void* user);
int AddDrawCallback(render::Stage stage, DrawFn fn, void* user, int order = 0);  // World, WorldLate, Hud only
void RemoveDrawCallback(int handle);

struct Stats { uint32_t primitives, vertices, callbacks; double cpuMs, gpuMs; };
Stats GetStats();
}
