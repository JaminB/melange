// Offline self-test of the draw queue (render/mirage/draw_queue.h) and sprite batching (render/mirage/draw_sprites.h).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>

#include "render/mirage/draw_queue.h"
#include "render/mirage/draw_sprites.h"

namespace {
using melange::draw::kDepthTest;
using melange::mirage::drawqueue::Kind;
using melange::mirage::drawqueue::Primitive;
using melange::mirage::drawqueue::RouteStage;
using melange::mirage::drawqueue::StageQueue;
using melange::mirage::drawqueue::Unpack;
using melange::render::Stage;

int g_checks = 0, g_failures = 0;
void Check(bool cond, const char* what) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
}

Primitive Make(int frames) {
    Primitive p;
    p.kind = Kind::Line;
    p.framesLeft = frames;
    return p;
}

void TestOneShotDrop() {
    StageQueue q;
    q.Push(Make(1), false);
    Check(!q.Idle(), "one-shot: queued before its first pass is not idle");

    auto& pass = q.BeginPass();
    Check(pass.size() == 1, "one-shot: BeginPass promotes the queued primitive");
    Check(q.Immediate().empty(), "one-shot: nothing landed in the immediate list");
    q.EndPass();
    Check(q.Idle(), "one-shot: frames=1 is dropped after its one pass");
}

void TestMultiFrameSurvives() {
    StageQueue q;
    q.Push(Make(3), false);

    for (int pass = 0; pass < 3; ++pass) {
        auto& v = q.BeginPass();
        Check(v.size() == 1, "multi-frame: exactly one primitive drawn each of its 3 passes");
        q.EndPass();
    }
    Check(q.Idle(), "multi-frame: gone after exactly 3 passes");
}

void TestImmediateSameFrame() {
    StageQueue q;
    q.BeginPass();
    Check(q.Immediate().empty() && q.BeginPass().empty(), "immediate: window opens empty");
    q.Push(Make(1), /*insideThisStage=*/true);
    Check(q.Immediate().size() == 1, "immediate: an inside-window push lands in Immediate(), not next pass's queue");
    q.EndPass();
    Check(q.Idle(), "immediate: a one-shot immediate primitive is gone once its pass ends");
}

void TestOutsidePushDuringOpenWindowWaitsOnePass() {
    StageQueue q;
    q.BeginPass();
    q.Push(Make(1), /*insideThisStage=*/false);
    Check(q.Immediate().empty(), "outside-during-window: does not join this pass's immediate list");
    q.EndPass();
    Check(!q.Idle(), "outside-during-window: still pending after this pass ends");
    auto& v = q.BeginPass();
    Check(v.size() == 1, "outside-during-window: drawn on the following pass");
    q.EndPass();
}

void TestRouting() {
    Check(RouteStage(Kind::Line, kDepthTest) == Stage::World, "routing: depth-tested world primitive -> World");
    Check(RouteStage(Kind::Line, 0) == Stage::WorldLate, "routing: non-depth-tested world primitive -> WorldLate");
    Check(RouteStage(Kind::HudRect, kDepthTest) == Stage::Hud, "routing: HUD kinds ignore flags and always go to Hud");
}

// Melange 0.12: the screen stage, nine-slice, picture fit and the clip stack.
bool Close(float a, float b) { return std::fabs(a - b) < 1e-3f; }

void TestScreenRouting() {
    using melange::mirage::drawqueue::IsHud;
    for (Kind k : {Kind::HudLine, Kind::HudRect, Kind::HudText, Kind::HudImage, Kind::HudImageUV, Kind::HudVideo, Kind::ClipPush,
                   Kind::ClipPop}) {
        Check(IsHud(k), "screen routing: every HUD kind is a HUD kind");
        Check(RouteStage(k, 0) == Stage::Hud, "screen routing: HUD kinds outside a callback go to Hud");
        Check(RouteStage(k, 0, Stage::Screen) == Stage::Screen, "screen routing: HUD kinds inside a Screen callback stay at Screen");
        Check(RouteStage(k, 0, Stage::World) == Stage::Hud, "screen routing: HUD kinds inside a World callback still go to Hud");
    }
    Check(!IsHud(Kind::Line) && RouteStage(Kind::Line, kDepthTest, Stage::Screen) == Stage::World,
          "screen routing: world kinds inside a Screen callback keep their world stage");
    Check(static_cast<int>(Stage::Screen) > static_cast<int>(Stage::Count),
          "screen routing: Screen is past Count, so the engine stage table never sees it");
}

void TestNineSlice() {
    using melange::mirage::drawqueue::NineSlice;
    using melange::mirage::drawqueue::UvQuad;
    UvQuad c[9];
    const float whole[4] = {0, 0, 1, 1};
    // 64x32 texture, insets 8/4/16/12 at scale 1 into 200x100.
    int n = NineSlice(10, 20, 210, 120, 64, 32, 8, 4, 16, 12, 1, whole, c);
    Check(n == 9, "nine: nine cells");
    Check(Close(c[0].x0, 10) && Close(c[0].x1, 18) && Close(c[0].y1, 24) && Close(c[0].u1, 8.f / 64) && Close(c[0].v1, 4.f / 32),
          "nine: top-left corner at inset size, uv to the inset");
    Check(Close(c[2].x0, 194) && Close(c[2].u0, 1 - 16.f / 64) && Close(c[8].y0, 108) && Close(c[8].v0, 1 - 12.f / 32),
          "nine: right and bottom insets");
    Check(Close(c[4].x0, 18) && Close(c[4].x1, 194) && Close(c[4].y0, 24) && Close(c[4].y1, 108), "nine: the centre stretches");
    n = NineSlice(0, 0, 200, 100, 64, 32, 8, 4, 16, 12, 2, whole, c);
    Check(n == 9 && Close(c[0].x1, 16) && Close(c[0].u1, 8.f / 64) && Close(c[8].y0, 76), "nine: scale grows the corners, not their uv");
    // Corners wider than the rectangle shrink evenly (8 + 16 into 12: 4 and 8), leaving no centre column.
    n = NineSlice(0, 0, 12, 100, 64, 32, 8, 4, 16, 12, 1, whole, c);
    Check(n == 6 && Close(c[0].x1, 4) && Close(c[1].x0, 4) && Close(c[1].x1, 12), "nine: too narrow: corners share the width");
    const float sub[4] = {0.5f, 0.5f, 1.f, 1.f};
    n = NineSlice(0, 0, 100, 100, 64, 32, 4, 4, 4, 4, 1, sub, c);
    Check(n == 9 && Close(c[0].u0, 0.5f) && Close(c[0].u1, 0.5f + 4.f / 64) && Close(c[8].u1, 1) && Close(c[8].v0, 1 - 4.f / 32),
          "nine: uv selects part of an atlas");
    n = NineSlice(0, 0, 100, 100, 64, 32, 40, 0, 40, 0, 1, whole, c);
    Check(n == 3 && Close(c[0].u1, 0.5f) && Close(c[0].x1, 32), "nine: insets past the source shrink to fit it");
    n = NineSlice(0, 0, 100, 100, 64, 32, 0, 0, 0, 0, 1, whole, c);
    Check(n == 1 && Close(c[0].u1, 1) && Close(c[0].x1, 100), "nine: zero insets are one stretched image");
    Check(NineSlice(10, 10, 10, 50, 64, 32, 1, 1, 1, 1, 1, whole, c) == 0, "nine: an empty rectangle draws nothing");
    Check(NineSlice(0, 0, 10, 10, 0, 32, 1, 1, 1, 1, 1, whole, c) == 0, "nine: an unknown texture size draws nothing");
    const float bad[4] = {0, std::numeric_limits<float>::quiet_NaN(), 1, 1};
    Check(NineSlice(0, 0, 10, 10, 64, 32, 1, 1, 1, 1, 1, bad, c) == 0, "nine: a NaN uv draws nothing");
    n = NineSlice(0, 0, 100, 100, 64, 32, -5, std::numeric_limits<float>::infinity(), 4, 4, -1, whole, c);
    Check(n == 4 && Close(c[0].x1, 96), "nine: negative or infinite insets count as 0, a bad scale as 1");
}

void TestFitPicture() {
    using melange::mirage::drawqueue::Fit;
    using melange::mirage::drawqueue::FitPicture;
    using melange::mirage::drawqueue::UvQuad;
    UvQuad q{};
    // A 16:9 picture filling 960x540 of a 1280x720 texture.
    const float u1 = 0.75f, v1 = 0.75f;
    Check(FitPicture(0, 0, 400, 400, 16.0 / 9, u1, v1, Fit::Contain, &q) && Close(q.x0, 0) && Close(q.x1, 400) && Close(q.y0, 87.5f) &&
              Close(q.y1, 312.5f) && Close(q.u1, u1) && Close(q.v1, v1),
          "fit: contain letterboxes top and bottom, whole uv");
    Check(FitPicture(0, 0, 400, 100, 16.0 / 9, u1, v1, Fit::Contain, &q) && Close(q.y0, 0) && Close(q.y1, 100) &&
              Close(q.x0, 200 - 88.8889f) && Close(q.x1, 200 + 88.8889f),
          "fit: contain pillarboxes left and right");
    Check(FitPicture(0, 0, 400, 400, 16.0 / 9, u1, v1, Fit::Cover, &q) && Close(q.x0, 0) && Close(q.y1, 400) && Close(q.v0, 0) &&
              Close(q.v1, v1) && Close(q.u1 - q.u0, u1 * 9.f / 16) && Close(q.u0 + q.u1, u1),
          "fit: cover keeps the rectangle and crops u around the centre");
    Check(FitPicture(0, 0, 400, 100, 16.0 / 9, u1, v1, Fit::Cover, &q) && Close(q.u0, 0) && Close(q.v1 - q.v0, v1 * (16.f / 9) / 4) &&
              Close(q.v0 + q.v1, v1),
          "fit: cover crops v for a wide rectangle");
    Check(FitPicture(5, 6, 7, 8, 0, u1, v1, Fit::Stretch, &q) && Close(q.x0, 5) && Close(q.y1, 8) && Close(q.u1, u1),
          "fit: stretch fills, any aspect");
    Check(!FitPicture(0, 0, 10, 10, 0, u1, v1, Fit::Contain, &q), "fit: a zero aspect cannot contain");
    Check(!FitPicture(0, 0, 0, 10, 1, u1, v1, Fit::Stretch, &q), "fit: an empty rectangle");
    Check(!FitPicture(0, 0, std::numeric_limits<float>::infinity(), 10, 1, u1, v1, Fit::Stretch, &q), "fit: a non-finite rectangle");
}

void TestClipStack() {
    using melange::mirage::drawqueue::ClipStack;
    using melange::mirage::drawqueue::ClipToScissor;
    using melange::mirage::drawqueue::Scissor;
    const int vp[4] = {0, 0, 800, 600};
    Scissor s = ClipToScissor(10, 20, 110, 70, 800, 600, vp);
    Check(s.x == 10 && s.y == 530 && s.w == 100 && s.h == 50, "clip: window rect to a bottom-left scissor");
    s = ClipToScissor(110, 70, 10, 20, 800, 600, vp);
    Check(s.x == 10 && s.y == 530 && s.w == 100 && s.h == 50, "clip: corners in any order");
    s = ClipToScissor(-50, -50, 2000, 2000, 800, 600, vp);
    Check(s.x == 0 && s.y == 0 && s.w == 800 && s.h == 600, "clip: clamped to the window");
    s = ClipToScissor(10.5f, 20.25f, 20.5f, 30.75f, 800, 600, vp);
    Check(s.x == 10 && s.w == 11 && s.y == 569 && s.h == 11, "clip: rounded outward");
    const int ss[4] = {0, 0, 1600, 1200};  // a 2x supersampled scene target
    s = ClipToScissor(10, 20, 110, 70, 800, 600, ss);
    Check(s.x == 20 && s.y == 1060 && s.w == 200 && s.h == 100, "clip: scaled into a larger viewport");
    const int off[4] = {100, 50, 800, 600};
    s = ClipToScissor(0, 0, 800, 600, 800, 600, off);
    Check(s.x == 100 && s.y == 50 && s.w == 800 && s.h == 600, "clip: offset viewport");
    s = ClipToScissor(0, 0, 10, 10, 0, 600, vp);
    Check(s.w == 0 && s.h == 0, "clip: no window, empty scissor");
    s = ClipToScissor(std::numeric_limits<float>::quiet_NaN(), 0, 10, 10, 800, 600, vp);
    Check(s.w == 0 && s.h == 0, "clip: a NaN clips everything");

    ClipStack st;
    Check(!st.Active(), "clip stack: starts empty");
    st.Push({0, 0, 100, 100});
    st.Push({50, 50, 100, 100});
    Scissor t = st.Top();
    Check(st.Depth() == 2 && t.x == 50 && t.y == 50 && t.w == 50 && t.h == 50, "clip stack: nested clips intersect");
    st.Push({200, 200, 10, 10});
    t = st.Top();
    Check(t.w == 0 && t.h == 0, "clip stack: disjoint clips leave nothing");
    st.Push({0, 0, 1000, 1000});
    st.Push({0, 0, 1, 1});  // past the cap: counted, ignored
    Check(st.Depth() == 5 && st.Top().w == 0, "clip stack: past depth 4 the deepest real clip stays");
    st.Pop();
    st.Pop();
    st.Pop();
    t = st.Top();
    Check(st.Depth() == 2 && t.x == 50 && t.w == 50, "clip stack: pops pair with pushes, the cap included");
    st.Pop();
    st.Pop();
    st.Pop();
    Check(!st.Active() && st.Depth() == 0, "clip stack: extra pops are harmless");
}

void TestColorUnpack() {
    float rgba[4];
    Unpack(0xff0000ffu, rgba);  // IM_COL32(255,0,0,255): opaque red
    Check(rgba[0] > 0.99f && rgba[1] < 0.01f && rgba[2] < 0.01f && rgba[3] > 0.99f, "colour: opaque red unpacks R=1,A=1");
    Unpack(0x80ff0000u, rgba);  // IM_COL32(0,0,255,128): half-alpha blue
    Check(rgba[2] > 0.99f && rgba[0] < 0.01f && rgba[3] > 0.49f && rgba[3] < 0.51f, "colour: half-alpha blue unpacks B=1,A~0.5");
}

// ---------------------------------------------------------------- sprites
namespace ds = melange::mirage::drawsprites;
using melange::draw::Sprite;
using melange::draw::SpriteBlend;
using melange::draw::Vertex;

const float kIdentity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

Sprite MakeSprite(float x, float y, float z, float hw, float hl = 0, float ax = 0, float ay = 0, float az = 0,
                  unsigned tex = 1, SpriteBlend blend = SpriteBlend::Alpha, uint32_t color = 0xffffffffu) {
    Sprite s{};
    s.pos[0] = x, s.pos[1] = y, s.pos[2] = z;
    s.halfW = hw, s.halfL = hl;
    s.axis[0] = ax, s.axis[1] = ay, s.axis[2] = az;
    s.color = color;
    s.texture = tex;
    s.blend = blend;
    return s;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-4f; }
bool At(const Vertex& v, float x, float y, float z, float u, float t) {
    return Near(v.pos[0], x) && Near(v.pos[1], y) && Near(v.pos[2], z) && Near(v.uv[0], u) && Near(v.uv[1], t);
}

void TestSpriteDrawable() {
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    Check(ds::Drawable(MakeSprite(0, 0, 0, 1)), "sprite: a plain billboard is drawable");
    Check(!ds::Drawable(MakeSprite(nan, 0, 0, 1)), "sprite: NaN position skipped");
    Check(!ds::Drawable(MakeSprite(0, inf, 0, 1)), "sprite: infinite position skipped");
    Check(!ds::Drawable(MakeSprite(0, 0, 0, 1, 1, 0, nan, 0)), "sprite: NaN axis skipped");
    Check(!ds::Drawable(MakeSprite(0, 0, 0, 1, inf)), "sprite: infinite halfL skipped");
    Check(!ds::Drawable(MakeSprite(0, 0, 0, 0)), "sprite: halfW 0 skipped");
    Check(!ds::Drawable(MakeSprite(0, 0, 0, -1)), "sprite: negative halfW skipped");
    Check(!ds::Drawable(MakeSprite(0, 0, 0, 1, 0, 1, 0, 0)), "sprite: stretched with halfL 0 skipped");
    Check(ds::Drawable(MakeSprite(0, 0, 0, 1, -2, 1, 0, 0)), "sprite: negative halfL is its magnitude");
}

void TestSpriteGeometry() {
    Vertex v[4];
    Check(ds::Expand(MakeSprite(0, 0, -10, 1), kIdentity, v), "billboard: in front of the eye expands");
    Check(At(v[0], -1, 1, -10, 0, 0) && At(v[1], 1, 1, -10, 1, 0) && At(v[2], 1, -1, -10, 1, 1) && At(v[3], -1, -1, -10, 0, 1),
          "billboard: upright square, v = 0 at the top, u = 0 on the left");

    // Axis pointing screen-down: same layout as the billboard, stretched to halfL along the axis.
    Check(ds::Expand(MakeSprite(0, 0, -10, 1, 3, 0, -2, 0), kIdentity, v), "stretched: expands");
    Check(At(v[0], -1, 3, -10, 0, 0) && At(v[1], 1, 3, -10, 1, 0) && At(v[2], 1, -3, -10, 1, 1) && At(v[3], -1, -3, -10, 0, 1),
          "stretched: tail (v 0) at pos - axis*halfL, head (v 1) at pos + axis*halfL, axis normalised");

    // Axis along screen-right: the quad's long side follows it, width perpendicular on screen.
    ds::Expand(MakeSprite(0, 0, -10, 0.5f, 4, 1, 0, 0), kIdentity, v);
    Check(Near(v[0].pos[0], -4) && Near(v[2].pos[0], 4) && Near(std::fabs(v[0].pos[1] - v[1].pos[1]), 1.f),
          "stretched: horizontal axis -> 8 long in x, 1 wide in y");

    // Axis with depth: the true 3D endpoints are kept (perspective-correct streak).
    ds::Expand(MakeSprite(0, 0, -10, 0.5f, 2, 1, 0, -1), kIdentity, v);
    Check(Near(v[0].pos[2], -10 + 1.41421356f) && Near(v[2].pos[2], -10 - 1.41421356f), "stretched: endpoints keep their depth");

    // Straight at the camera: no sliver, a min(halfW, halfL) square facing the eye.
    Check(ds::Expand(MakeSprite(0, 0, -10, 1, 5, 0, 0, 1), kIdentity, v), "head-on: expands");
    float maxX = 0, maxY = 0;
    for (const Vertex& q : v) {
        maxX = std::max(maxX, std::fabs(q.pos[0]));
        maxY = std::max(maxY, std::fabs(q.pos[1]));
        Check(Near(q.pos[2], -10), "head-on: quad faces the eye at the sprite's depth");
    }
    Check(Near(maxX, 1) && Near(maxY, 1), "head-on: padded to halfW x halfW");
    ds::Expand(MakeSprite(0, 0, -10, 1, 5, 0.02f, 0, 1), kIdentity, v);
    float len = 0;
    for (const Vertex& q : v) len = std::max(len, std::fabs(q.pos[0]));
    Check(len >= 1.f - 1e-4f && len < 1.2f, "nearly head-on: still at least the minimum length");

    Check(!ds::Expand(MakeSprite(0, 0, 10, 1), kIdentity, v), "billboard behind the eye is culled");
    Check(!ds::Expand(MakeSprite(0, 0, 10, 1, 3, 0, 1, 0), kIdentity, v), "stretched behind the eye is culled");
    Check(ds::Expand(MakeSprite(0, 0, 0.5f, 1), kIdentity, v), "straddling the eye plane is kept (GL clips it)");

    // View matrix: eye at z = +10 looking down -z (translation in [12..14]).
    float view[16];
    std::copy(kIdentity, kIdentity + 16, view);
    view[14] = -10;
    ds::Expand(MakeSprite(0, 0, 0, 1), view, v);
    Check(Near(v[0].pos[2], -10), "view matrix applied to the centre");

    Check(ds::VertexColor(MakeSprite(0, 0, 0, 1, 0, 0, 0, 0, 1, SpriteBlend::Premultiplied, 0x80ffffffu)) == 0x80808080u,
          "premul: tint premultiplied by its alpha");
    Check(ds::VertexColor(MakeSprite(0, 0, 0, 1, 0, 0, 0, 0, 1, SpriteBlend::Additive, 0x80ffffffu)) == 0x80ffffffu,
          "additive: tint unchanged");
}

void TestSpriteBatch() {
    ds::Batch b;
    std::vector<Sprite> in = {MakeSprite(0, 0, -5, 1, 0, 0, 0, 0, 7), MakeSprite(0, 0, -20, 1, 0, 0, 0, 0, 7),
                              MakeSprite(0, 0, -10, 1, 0, 0, 0, 0, 8, SpriteBlend::Additive), MakeSprite(0, 0, 5, 1)};
    b.Build(in, kIdentity);
    Check(b.verts.size() == 12, "batch: the sprite behind the eye is dropped");
    Check(b.runs.size() == 3 && b.runs[0].texture == 7 && b.runs[1].texture == 8 && b.runs[2].texture == 7,
          "batch: back to front (-20, -10, -5) splits into texture runs");
    Check(Near(b.verts[0].pos[2], -20) && Near(b.verts[4].pos[2], -10) && Near(b.verts[8].pos[2], -5), "batch: farthest first");
    Check(b.runs[1].blend == SpriteBlend::Additive && b.runs[1].first == 4 && b.runs[1].count == 4, "batch: run ranges");

    // Equal depth: submission order.
    in = {MakeSprite(1, 0, -5, 1, 0, 0, 0, 0, 3), MakeSprite(2, 0, -5, 1, 0, 0, 0, 0, 4), MakeSprite(3, 0, -5, 1, 0, 0, 0, 0, 3)};
    b.Build(in, kIdentity);
    Check(b.runs.size() == 3 && b.runs[0].texture == 3 && b.runs[1].texture == 4 && Near(b.verts[0].pos[0], 0) &&
              Near(b.verts[8].pos[0], 2),
          "batch: ties keep submission order");

    // Same texture and mode: one run however many sprites.
    in.clear();
    for (int i = 0; i < 1000; ++i) in.push_back(MakeSprite(static_cast<float>(i % 37), 0, -1.f - static_cast<float>(i % 101), 1));
    b.Build(in, kIdentity);
    Check(b.runs.size() == 1 && b.runs[0].count == 4000, "batch: one texture and mode -> one draw call");
    bool sorted = true;
    for (size_t i = 4; i < b.verts.size(); i += 4) sorted &= b.verts[i].pos[2] >= b.verts[i - 4].pos[2] - 1.5f;
    Check(sorted, "batch: non-decreasing depth");
    // A different blend mode with the same texture is its own run.
    in = {MakeSprite(0, 0, -2, 1), MakeSprite(0, 0, -1, 1, 0, 0, 0, 0, 1, SpriteBlend::Premultiplied)};
    b.Build(in, kIdentity);
    Check(b.runs.size() == 2, "batch: blend mode splits runs");

    // Additive sprites commute: an unbroken additive stretch is regrouped by texture; an alpha sprite breaks it.
    const SpriteBlend add = SpriteBlend::Additive;
    in = {MakeSprite(0, 0, -1, 1, 0, 0, 0, 0, 5, add), MakeSprite(0, 0, -2, 1, 0, 0, 0, 0, 6, add),
          MakeSprite(0, 0, -3, 1, 0, 0, 0, 0, 5, add), MakeSprite(0, 0, -4, 1, 0, 0, 0, 0, 6, add)};
    b.Build(in, kIdentity);
    Check(b.runs.size() == 2 && b.runs[0].count == 8 && b.runs[1].count == 8, "batch: interleaved additive textures -> 2 runs");
    in.push_back(MakeSprite(0, 0, -2.5f, 1, 0, 0, 0, 0, 5));  // alpha, between -2 and -3
    b.Build(in, kIdentity);
    Check(b.runs.size() == 5, "batch: an alpha sprite in between keeps depth order across it");
    Check(Near(b.verts[8].pos[2], -2.5f), "batch: the alpha sprite stays at its depth");
}

void TestMips() {
    // Straight alpha: one opaque red texel among transparent black ones keeps its colour at the next level.
    const uint8_t straight[16] = {255, 0, 0, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t d[4];
    Check(!ds::LooksPremultiplied(straight, 4), "mips: r > a is straight alpha");
    ds::Downsample(straight, 2, 2, d, false);
    Check(d[0] == 255 && d[1] == 0 && d[3] == 32, "mips: straight alpha averages colour weighted by alpha");
    ds::Downsample(straight, 2, 2, d, true);
    Check(d[0] == 64 && d[3] == 32, "mips: premultiplied averages as stored");
    const uint8_t premul[16] = {128, 0, 0, 128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    Check(ds::LooksPremultiplied(premul, 4), "mips: r <= a everywhere looks premultiplied");
    const uint8_t clear[16] = {200, 100, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    ds::Downsample(clear, 2, 2, d, false);
    Check(d[0] == 50 && d[3] == 0, "mips: all-transparent block falls back to a plain average");
    const uint8_t odd[12] = {10, 10, 10, 255, 30, 30, 30, 255, 200, 200, 200, 255};
    ds::Downsample(odd, 3, 1, d, false);
    Check(ds::MipSize(3) == 1 && ds::MipSize(1) == 1 && d[0] == 20 && d[3] == 255, "mips: 3x1 -> 1x1, last column dropped");
}

// CPU cost of building a stage's batch, per 1000 sprites (expand + sort + runs); printed, not checked.
void BenchSprites() {
    std::vector<Sprite> in;
    uint32_t rng = 12345;
    auto rnd = [&] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.f;
    };
    for (int i = 0; i < 4096; ++i) {
        const bool streak = (i & 1) != 0;
        in.push_back(MakeSprite(rnd() * 200 - 100, rnd() * 50, -10 - rnd() * 300, 0.5f + rnd(), streak ? 2 + rnd() * 6 : 0,
                                streak ? rnd() - 0.5f : 0, streak ? rnd() : 0, streak ? rnd() - 0.5f : 0,
                                (i % 3 == 0) ? 2u : 1u, (i % 5 == 0) ? SpriteBlend::Additive : SpriteBlend::Alpha));
    }
    float view[16] = {0.8f, 0.1f, -0.59f, 0, 0, 0.98f, 0.17f, 0, 0.6f, -0.13f, 0.79f, 0, 3, -20, -40, 1};
    ds::Batch b;
    b.Build(in, view);  // warm the vectors, as the per-stage batch is reused every frame
    double best = 1e9;
    for (int rep = 0; rep < 50; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        b.Build(in, view);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    Check(!b.verts.empty(), "bench: built");
    std::printf("sprite batch: %.3f ms per 1000 sprites (4096 mixed, %zu runs, best of 50)\n", best * 1000.0 / 4096.0,
                b.runs.size());
    for (Sprite& s : in) s.texture = 1, s.blend = SpriteBlend::Alpha;
    best = 1e9;
    for (int rep = 0; rep < 50; ++rep) {
        const auto t0 = std::chrono::steady_clock::now();
        b.Build(in, view);
        const auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    std::printf("sprite batch: %.3f ms per 1000 sprites (4096, one texture, %zu run)\n", best * 1000.0 / 4096.0, b.runs.size());
}
}  // namespace

int main() {
    TestOneShotDrop();
    TestMultiFrameSurvives();
    TestImmediateSameFrame();
    TestOutsidePushDuringOpenWindowWaitsOnePass();
    TestRouting();
    TestScreenRouting();
    TestNineSlice();
    TestFitPicture();
    TestClipStack();
    TestColorUnpack();
    TestSpriteDrawable();
    TestSpriteGeometry();
    TestSpriteBatch();
    TestMips();
    BenchSprites();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
