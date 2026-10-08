#include "render/mirage/draw_gl.h"

#include <windows.h>
#include <GL/gl.h>

#include <cmath>

#include "melange/draw.h"
#include "render/mirage/draw_font.h"
#include "render/mirage/draw_sprites.h"

namespace melange::mirage::drawgl {
namespace {
using drawqueue::Kind;
using drawqueue::Primitive;

struct Vec3 {
    float x, y, z;
};
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Scale(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
Vec3 Norm(Vec3 a) {
    float l = Len(a);
    return l > 1e-8f ? Vec3{a.x / l, a.y / l, a.z / l} : Vec3{0, 0, 1};
}
Vec3 FromArr(const float p[3]) { return {p[0], p[1], p[2]}; }

Vec3 TransformPoint(const float m[16], Vec3 p) {
    return {
        m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12],
        m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
        m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14],
    };
}

int g_windowW = 0, g_windowH = 0;
render::Camera g_cam{};

void UnpackColor(draw::Rgba c, float rgba[4]) { drawqueue::Unpack(c, rgba); }

// GL state already set in this stage, so consecutive primitives skip redundant calls and thick lines share one
// glBegin(GL_QUADS). Every change ends the open batch first.
enum class Space { Unset, ViewSpace, World, Screen };
struct Cache {
    Space space = Space::Unset;
    int additive = -1, depthTest = -1, depthWrite = -1, cull = -1;
    unsigned texture = ~0u;
    float lineWidth = -1;
    bool quads = false;
} g_c;

void EndQuads() {
    if (!g_c.quads) return;
    glEnd();
    g_c.quads = false;
}

void BeginQuads() {
    if (g_c.quads) return;
    glBegin(GL_QUADS);
    g_c.quads = true;
}

void SetSpace(Space s) {
    if (g_c.space == s) return;
    EndQuads();
    g_c.space = s;
    glMatrixMode(GL_PROJECTION);
    if (s == Space::Screen) {
        glLoadIdentity();
        glOrtho(0, g_windowW, g_windowH, 0, -1, 1);
    } else {
        glLoadMatrixf(g_cam.proj);
    }
    glMatrixMode(GL_MODELVIEW);
    if (s == Space::World) glLoadMatrixf(g_cam.view);
    else glLoadIdentity();
}

// g_c.additive: 0 alpha, 1 additive, 2 premultiplied (sprites only).
void SetBlendMode(int mode) {
    if (g_c.additive == mode) return;
    EndQuads();
    g_c.additive = mode;
    if (mode == 2) glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    else glBlendFunc(GL_SRC_ALPHA, mode == 1 ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
}

void SetBlend(bool additive) { SetBlendMode(additive ? 1 : 0); }

void SetDepth(bool test, bool write) {
    if (g_c.depthTest != static_cast<int>(test)) {
        EndQuads();
        g_c.depthTest = test;
        if (test) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }
    if (g_c.depthWrite != static_cast<int>(write)) {
        EndQuads();
        g_c.depthWrite = write;
        glDepthMask(write ? GL_TRUE : GL_FALSE);
    }
}

void SetCull(bool cull) {
    if (g_c.cull == static_cast<int>(cull)) return;
    EndQuads();
    g_c.cull = cull;
    if (cull) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }
}

void SetTexture(unsigned tex) {
    if (g_c.texture == tex) return;
    EndQuads();
    g_c.texture = tex;
    if (tex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, tex);
    } else {
        glDisable(GL_TEXTURE_2D);
    }
}

void SetLineWidth(float w) {
    if (g_c.lineWidth == w) return;
    EndQuads();
    g_c.lineWidth = w;
    glLineWidth(w);
}

// View-space half width of `widthPx` pixels at view depth -viewZ, from the projection's Y scale proj[5].
float HalfWidthAt(float viewZ, float widthPx) {
    float depth = (-viewZ < 1.f) ? 1.f : -viewZ;
    float yScale = (g_cam.proj[5] != 0.f) ? g_cam.proj[5] : 1.f;
    return (widthPx * depth) / (yScale * (g_windowH > 0 ? g_windowH : 1)) * 0.5f;
}

// Thick line as a camera-facing quad in view space, so widthPx holds on drivers that cap glLineWidth.
void WorldLineTo(Vec3 aWorld, Vec3 bWorld, float widthPx, const float rgba[4], uint32_t& verts) {
    Vec3 a = TransformPoint(g_cam.view, aWorld), b = TransformPoint(g_cam.view, bWorld);
    Vec3 perp = Scale(Norm(Cross(Sub(b, a), Scale(Add(a, b), 0.5f))), HalfWidthAt((a.z + b.z) * 0.5f, widthPx));
    Vec3 a0 = Sub(a, perp), a1 = Add(a, perp), b0 = Sub(b, perp), b1 = Add(b, perp);
    BeginQuads();
    glColor4fv(rgba);
    glVertex3f(a0.x, a0.y, a0.z);
    glVertex3f(a1.x, a1.y, a1.z);
    glVertex3f(b1.x, b1.y, b1.z);
    glVertex3f(b0.x, b0.y, b0.z);
    verts += 4;
}

void DrawBox(const Primitive& p, const float rgba[4], uint32_t& verts) {
    const float* mn = p.p[0];
    const float* mx = p.p[1];
    const Vec3 c[8] = {
        {mn[0], mn[1], mn[2]}, {mx[0], mn[1], mn[2]}, {mx[0], mx[1], mn[2]}, {mn[0], mx[1], mn[2]},
        {mn[0], mn[1], mx[2]}, {mx[0], mn[1], mx[2]}, {mx[0], mx[1], mx[2]}, {mn[0], mx[1], mx[2]},
    };
    static const int kEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                       {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (auto& e : kEdges) WorldLineTo(c[e[0]], c[e[1]], p.widthPx, rgba, verts);
}

void DrawSphere(const Primitive& p, const float rgba[4], uint32_t& verts) {
    constexpr int kSegs = 24;
    Vec3 o = FromArr(p.p[0]);
    float r = p.scalar;
    for (int ring = 0; ring < 3; ++ring) {
        Vec3 prev{};
        for (int i = 0; i <= kSegs; ++i) {
            float a = (2.f * 3.14159265f * i) / kSegs;
            float s = std::sin(a) * r, cc = std::cos(a) * r;
            Vec3 pt = o;
            if (ring == 0) pt = Add(o, {cc, s, 0});
            else if (ring == 1) pt = Add(o, {cc, 0, s});
            else pt = Add(o, {0, cc, s});
            if (i > 0) WorldLineTo(prev, pt, p.widthPx, rgba, verts);
            prev = pt;
        }
    }
}

void DrawAxes(const Primitive& p, uint32_t& verts) {
    Vec3 o = FromArr(p.p[0]);
    float L = p.scalar;
    static const float kRed[4] = {1, 0, 0, 1}, kGreen[4] = {0, 1, 0, 1}, kBlue[4] = {0, 0.4f, 1, 1};
    WorldLineTo(o, Add(o, {L, 0, 0}), p.widthPx, kRed, verts);
    WorldLineTo(o, Add(o, {0, L, 0}), p.widthPx, kGreen, verts);
    WorldLineTo(o, Add(o, {0, 0, L}), p.widthPx, kBlue, verts);
}

void DrawMesh(const Primitive& p, uint32_t& verts) {
    if (p.meshV.empty()) return;
    SetTexture(p.texture);
    EndQuads();
    if (p.hasModel) {
        glMatrixMode(GL_MODELVIEW);
        glMultMatrixf(p.model);
        g_c.space = Space::Unset;
    }
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    const draw::Vertex* v = p.meshV.data();
    glVertexPointer(3, GL_FLOAT, sizeof(draw::Vertex), &v->pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(draw::Vertex), &v->color);
    glTexCoordPointer(2, GL_FLOAT, sizeof(draw::Vertex), &v->uv);
    if (!p.meshI.empty())
        glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(p.meshI.size()), GL_UNSIGNED_SHORT, p.meshI.data());
    else
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(p.meshV.size()));
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    verts += static_cast<uint32_t>(p.meshV.size());
}

void EmitGlyphs(const std::vector<drawfont::GlyphQuad>& glyphs, float z) {
    for (const auto& g : glyphs) {
        glTexCoord2f(g.u0, g.v0);
        glVertex3f(g.x0, g.y0, z);
        glTexCoord2f(g.u1, g.v0);
        glVertex3f(g.x1, g.y0, z);
        glTexCoord2f(g.u1, g.v1);
        glVertex3f(g.x1, g.y1, z);
        glTexCoord2f(g.u0, g.v1);
        glVertex3f(g.x0, g.y1, z);
    }
}

std::vector<drawfont::GlyphQuad> g_glyphs;

void DrawWorldText(const Primitive& p, const float rgba[4], uint32_t& verts) {
    if (!drawfont::Texture()) return;
    float sx = 0, sy = 0, depth = 0;
    if (!render::WorldToScreen(p.p[0], &sx, &sy, &depth)) return;
    g_glyphs.clear();
    drawfont::Extent ext = drawfont::Layout(p.text.c_str(), p.sizePx, 0, 0, g_glyphs);
    float dx = sx - ext.w * 0.5f, dy = sy - ext.h;  // centred above the anchor
    for (auto& g : g_glyphs) {
        g.x0 += dx;
        g.x1 += dx;
        g.y0 += dy;
        g.y1 += dy;
    }
    SetSpace(Space::Screen);
    SetTexture(drawfont::Texture());
    BeginQuads();
    glColor4fv(rgba);
    // glOrtho(..., -1, 1) maps z to NDC -z, so z = 1 - 2*depth lands on the anchor's depth-buffer value.
    EmitGlyphs(g_glyphs, 1.f - 2.f * depth);
    verts += static_cast<uint32_t>(g_glyphs.size()) * 4;
}

void DrawWorldOne(const Primitive& p, uint32_t& prims, uint32_t& verts) {
    SetBlend((p.flags & draw::kAdditive) != 0);
    SetDepth((p.flags & draw::kDepthTest) != 0, (p.flags & draw::kDepthWrite) != 0);
    float rgba[4];
    UnpackColor(p.color, rgba);
    ++prims;
    switch (p.kind) {
        case Kind::Line:
        case Kind::Box:
        case Kind::Sphere:
        case Kind::Axes:
            SetSpace(Space::ViewSpace);
            SetTexture(0);
            SetCull(false);
            if (p.kind == Kind::Line) WorldLineTo(FromArr(p.p[0]), FromArr(p.p[1]), p.widthPx, rgba, verts);
            else if (p.kind == Kind::Box) DrawBox(p, rgba, verts);
            else if (p.kind == Kind::Sphere) DrawSphere(p, rgba, verts);
            else DrawAxes(p, verts);
            break;
        case Kind::Quad:
            SetSpace(Space::World);
            SetTexture(0);
            SetCull((p.flags & draw::kNoCull) == 0);
            BeginQuads();
            glColor4fv(rgba);
            for (const auto& pt : p.p) glVertex3fv(pt);
            verts += 4;
            break;
        case Kind::Mesh:
            SetSpace(Space::World);
            SetCull((p.flags & draw::kNoCull) == 0);
            DrawMesh(p, verts);
            break;
        case Kind::Text:
            SetCull(false);
            DrawWorldText(p, rgba, verts);
            break;
        default:
            break;
    }
}

drawsprites::Batch g_sprites;

// All of a stage's sprites from one client-side vertex array, one glDrawArrays per texture + blend run.
void DrawSprites(const std::vector<draw::Sprite>& sprites, uint32_t& prims, uint32_t& verts) {
    g_sprites.Build(sprites, g_cam.view);
    if (g_sprites.runs.empty()) return;
    EndQuads();
    SetSpace(Space::ViewSpace);
    SetDepth(true, false);
    SetCull(false);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);  // restored by PopState (GL_TEXTURE_BIT)
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    const draw::Vertex* v = g_sprites.verts.data();
    glVertexPointer(3, GL_FLOAT, sizeof(draw::Vertex), &v->pos);
    glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(draw::Vertex), &v->color);
    glTexCoordPointer(2, GL_FLOAT, sizeof(draw::Vertex), &v->uv);
    for (const drawsprites::Run& r : g_sprites.runs) {
        SetTexture(r.texture);
        SetBlendMode(r.blend == draw::SpriteBlend::Additive ? 1 : r.blend == draw::SpriteBlend::Premultiplied ? 2 : 0);
        glDrawArrays(GL_QUADS, static_cast<GLint>(r.first), static_cast<GLsizei>(r.count));
    }
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    prims += static_cast<uint32_t>(g_sprites.verts.size() / 4);
    verts += static_cast<uint32_t>(g_sprites.verts.size());
}

void DrawHudOne(const Primitive& p, uint32_t& prims, uint32_t& verts) {
    float rgba[4];
    UnpackColor(p.color, rgba);
    ++prims;
    float x0 = p.p[0][0], y0 = p.p[0][1], x1 = p.p[1][0], y1 = p.p[1][1];
    switch (p.kind) {
        case Kind::HudLine:
            SetTexture(0);
            SetLineWidth(p.widthPx > 0 ? p.widthPx : 1.f);
            EndQuads();
            glColor4fv(rgba);
            glBegin(GL_LINES);
            glVertex2f(x0 + 0.375f, y0 + 0.375f);
            glVertex2f(x1 + 0.375f, y1 + 0.375f);
            glEnd();
            verts += 2;
            break;
        case Kind::HudRect:
            SetTexture(0);
            if (p.filled) {
                BeginQuads();
                glColor4fv(rgba);
                glVertex2f(x0, y0);
                glVertex2f(x1, y0);
                glVertex2f(x1, y1);
                glVertex2f(x0, y1);
            } else {
                SetLineWidth(p.widthPx > 0 ? p.widthPx : 1.f);
                EndQuads();
                glColor4fv(rgba);
                glBegin(GL_LINE_LOOP);
                glVertex2f(x0 + 0.375f, y0 + 0.375f);
                glVertex2f(x1 + 0.375f, y0 + 0.375f);
                glVertex2f(x1 + 0.375f, y1 + 0.375f);
                glVertex2f(x0 + 0.375f, y1 + 0.375f);
                glEnd();
            }
            verts += 4;
            break;
        case Kind::HudText: {
            if (!drawfont::Texture()) break;
            g_glyphs.clear();
            drawfont::Layout(p.text.c_str(), p.sizePx, x0, y0, g_glyphs);
            SetTexture(drawfont::Texture());
            BeginQuads();
            glColor4fv(rgba);
            EmitGlyphs(g_glyphs, 0.f);
            verts += static_cast<uint32_t>(g_glyphs.size()) * 4;
            break;
        }
        case Kind::HudImage: {
            if (!p.texture) break;
            float t[4];
            UnpackColor(p.color2, t);
            SetTexture(p.texture);
            BeginQuads();
            glColor4fv(t);
            glTexCoord2f(0, 0);
            glVertex2f(x0, y0);
            glTexCoord2f(1, 0);
            glVertex2f(x1, y0);
            glTexCoord2f(1, 1);
            glVertex2f(x1, y1);
            glTexCoord2f(0, 1);
            glVertex2f(x0, y1);
            verts += 4;
            break;
        }
        default:
            break;
    }
}
}  // namespace

FrameStats DrawStage(render::Stage stage, std::vector<Primitive>& a, std::vector<Primitive>& b,
                     const std::vector<draw::Sprite>& sprites) {
    FrameStats stats;
    const bool haveSprites = !sprites.empty() && stage != render::Stage::Hud;
    if (a.empty() && b.empty() && !haveSprites) return stats;

    render::WindowSize(&g_windowW, &g_windowH);
    bool haveCam = render::GetCamera(&g_cam);
    if (stage != render::Stage::Hud && !haveCam) return stats;

    uint32_t token = render::PushState();
    if (!token) return stats;
    // The atlas upload must happen outside a glBegin/glEnd batch.
    for (const auto* list : {&a, &b})
        for (const auto& p : *list)
            if (p.kind == Kind::Text || p.kind == Kind::HudText) {
                drawfont::Init();
                break;
            }
    g_c = Cache{};
    glEnable(GL_BLEND);
    if (stage == render::Stage::Hud) {
        SetSpace(Space::Screen);
        SetBlend(false);
        SetDepth(false, false);
        SetCull(false);
        for (auto& p : a) DrawHudOne(p, stats.primitives, stats.vertices);
        for (auto& p : b) DrawHudOne(p, stats.primitives, stats.vertices);
    } else {
        for (auto& p : a) DrawWorldOne(p, stats.primitives, stats.vertices);
        for (auto& p : b) DrawWorldOne(p, stats.primitives, stats.vertices);
        if (haveSprites) DrawSprites(sprites, stats.primitives, stats.vertices);
    }
    EndQuads();
    render::PopState(token);
    return stats;
}
}  // namespace melange::mirage::drawgl
