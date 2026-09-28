#include "render/mirage/draw_gl.h"

#include <windows.h>
#include <GL/gl.h>

#include <cmath>

#include "melange/draw.h"
#include "render/mirage/draw_font.h"

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
bool g_haveCam = false;

void UnpackColor(draw::Rgba c, float rgba[4]) { drawqueue::Unpack(c, rgba); }

// View-space half width of `widthPx` pixels at view depth -viewZ, from the projection's Y scale proj[5].
float HalfWidthAt(float viewZ, float widthPx) {
    float depth = (-viewZ < 1.f) ? 1.f : -viewZ;
    float yScale = (g_cam.proj[5] != 0.f) ? g_cam.proj[5] : 1.f;
    return (widthPx * depth) / (yScale * (g_windowH > 0 ? g_windowH : 1)) * 0.5f;
}

// Thick line as a camera-facing quad, so widthPx holds on drivers that cap glLineWidth.
void ThickLineViewSpace(Vec3 a, Vec3 b, float halfWidth, const float rgba[4]) {
    Vec3 dir = Sub(b, a);
    Vec3 mid = Scale(Add(a, b), 0.5f);
    Vec3 perp = Scale(Norm(Cross(dir, mid)), halfWidth);
    Vec3 a0 = Sub(a, perp), a1 = Add(a, perp), b0 = Sub(b, perp), b1 = Add(b, perp);
    glColor4fv(rgba);
    glBegin(GL_QUADS);
    glVertex3f(a0.x, a0.y, a0.z);
    glVertex3f(a1.x, a1.y, a1.z);
    glVertex3f(b1.x, b1.y, b1.z);
    glVertex3f(b0.x, b0.y, b0.z);
    glEnd();
}

void WorldLineTo(Vec3 aWorld, Vec3 bWorld, float widthPx, const float rgba[4], uint32_t& verts) {
    Vec3 a = TransformPoint(g_cam.view, aWorld), b = TransformPoint(g_cam.view, bWorld);
    ThickLineViewSpace(a, b, HalfWidthAt((a.z + b.z) * 0.5f, widthPx), rgba);
    verts += 4;
}

void SetupWorldViewIdentity() {
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(g_cam.proj);
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

void DrawQuad(const Primitive& p, const float rgba[4], bool noCull, uint32_t& verts) {
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(g_cam.view);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(g_cam.proj);
    if (noCull) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    glDisable(GL_TEXTURE_2D);
    glColor4fv(rgba);
    glBegin(GL_QUADS);
    for (const auto& pt : p.p) glVertex3fv(pt);
    glEnd();
    verts += 4;
}

void DrawMesh(const Primitive& p, bool noCull, uint32_t& verts) {
    if (p.meshV.empty()) return;
    glMatrixMode(GL_MODELVIEW);
    glLoadMatrixf(g_cam.view);
    if (p.hasModel) glMultMatrixf(p.model);
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(g_cam.proj);
    if (noCull) glDisable(GL_CULL_FACE);
    else {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }
    if (p.texture) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, p.texture);
    } else {
        glDisable(GL_TEXTURE_2D);
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

void DrawGlyphs(const std::vector<drawfont::GlyphQuad>& glyphs, const float rgba[4]) {
    if (glyphs.empty()) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, drawfont::Texture());
    glColor4fv(rgba);
    glBegin(GL_QUADS);
    for (const auto& g : glyphs) {
        glTexCoord2f(g.u0, g.v0);
        glVertex2f(g.x0, g.y0);
        glTexCoord2f(g.u1, g.v0);
        glVertex2f(g.x1, g.y0);
        glTexCoord2f(g.u1, g.v1);
        glVertex2f(g.x1, g.y1);
        glTexCoord2f(g.u0, g.v1);
        glVertex2f(g.x0, g.y1);
    }
    glEnd();
}

void DrawWorldText(const Primitive& p, uint32_t& verts) {
    if (!drawfont::Init()) return;
    float sx = 0, sy = 0, depth = 0;
    if (!render::WorldToScreen(p.p[0], &sx, &sy, &depth)) return;

    std::vector<drawfont::GlyphQuad> glyphs;
    drawfont::Extent ext = drawfont::Layout(p.text.c_str(), p.sizePx, 0, 0, glyphs);
    float dx = sx - ext.w * 0.5f, dy = sy - ext.h;  // centred above the anchor
    for (auto& g : glyphs) {
        g.x0 += dx;
        g.x1 += dx;
        g.y0 += dy;
        g.y1 += dy;
    }

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    // glOrtho(..., -1, 1) maps z to NDC -z, so z = 1 - 2*depth lands on the anchor's depth-buffer value.
    glOrtho(0, g_windowW, g_windowH, 0, -1, 1);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    float zParam = 1.f - 2.f * depth;
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    float rgba[4];
    UnpackColor(p.color, rgba);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, drawfont::Texture());
    glColor4fv(rgba);
    glBegin(GL_QUADS);
    for (const auto& g : glyphs) {
        glTexCoord2f(g.u0, g.v0);
        glVertex3f(g.x0, g.y0, zParam);
        glTexCoord2f(g.u1, g.v0);
        glVertex3f(g.x1, g.y0, zParam);
        glTexCoord2f(g.u1, g.v1);
        glVertex3f(g.x1, g.y1, zParam);
        glTexCoord2f(g.u0, g.v1);
        glVertex3f(g.x0, g.y1, zParam);
    }
    glEnd();
    verts += static_cast<uint32_t>(glyphs.size()) * 4;
}

void DrawWorldOne(const Primitive& p, uint32_t& prims, uint32_t& verts) {
    bool depthTest = (p.flags & draw::kDepthTest) != 0;
    bool depthWrite = (p.flags & draw::kDepthWrite) != 0;
    bool additive = (p.flags & draw::kAdditive) != 0;
    bool noCull = (p.flags & draw::kNoCull) != 0;

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, additive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);
    if (depthTest) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(depthWrite ? GL_TRUE : GL_FALSE);

    float rgba[4];
    UnpackColor(p.color, rgba);
    ++prims;
    switch (p.kind) {
        case Kind::Line:
            SetupWorldViewIdentity();
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_CULL_FACE);
            WorldLineTo(FromArr(p.p[0]), FromArr(p.p[1]), p.widthPx, rgba, verts);
            break;
        case Kind::Box:
            SetupWorldViewIdentity();
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_CULL_FACE);
            DrawBox(p, rgba, verts);
            break;
        case Kind::Sphere:
            SetupWorldViewIdentity();
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_CULL_FACE);
            DrawSphere(p, rgba, verts);
            break;
        case Kind::Axes:
            SetupWorldViewIdentity();
            glDisable(GL_TEXTURE_2D);
            glDisable(GL_CULL_FACE);
            DrawAxes(p, verts);
            break;
        case Kind::Quad:
            DrawQuad(p, rgba, noCull, verts);
            break;
        case Kind::Mesh:
            DrawMesh(p, noCull, verts);
            break;
        case Kind::Text:
            DrawWorldText(p, verts);
            break;
        default:
            break;
    }
}

void DrawHudOne(const Primitive& p, uint32_t& prims, uint32_t& verts) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    float rgba[4];
    UnpackColor(p.color, rgba);
    ++prims;
    switch (p.kind) {
        case Kind::HudLine: {
            glDisable(GL_TEXTURE_2D);
            glColor4fv(rgba);
            glLineWidth(p.widthPx > 0 ? p.widthPx : 1.f);
            glBegin(GL_LINES);
            glVertex2f(p.p[0][0] + 0.375f, p.p[0][1] + 0.375f);
            glVertex2f(p.p[1][0] + 0.375f, p.p[1][1] + 0.375f);
            glEnd();
            verts += 2;
            break;
        }
        case Kind::HudRect: {
            glDisable(GL_TEXTURE_2D);
            glColor4fv(rgba);
            float x0 = p.p[0][0], y0 = p.p[0][1], x1 = p.p[1][0], y1 = p.p[1][1];
            if (p.filled) {
                glBegin(GL_QUADS);
                glVertex2f(x0, y0);
                glVertex2f(x1, y0);
                glVertex2f(x1, y1);
                glVertex2f(x0, y1);
                glEnd();
                verts += 4;
            } else {
                glLineWidth(p.widthPx > 0 ? p.widthPx : 1.f);
                glBegin(GL_LINE_LOOP);
                glVertex2f(x0 + 0.375f, y0 + 0.375f);
                glVertex2f(x1 + 0.375f, y0 + 0.375f);
                glVertex2f(x1 + 0.375f, y1 + 0.375f);
                glVertex2f(x0 + 0.375f, y1 + 0.375f);
                glEnd();
                verts += 4;
            }
            break;
        }
        case Kind::HudText: {
            if (!drawfont::Init()) break;
            std::vector<drawfont::GlyphQuad> glyphs;
            drawfont::Layout(p.text.c_str(), p.sizePx, p.p[0][0], p.p[0][1], glyphs);
            DrawGlyphs(glyphs, rgba);
            verts += static_cast<uint32_t>(glyphs.size()) * 4;
            break;
        }
        case Kind::HudImage: {
            if (!p.texture) break;
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, p.texture);
            float t[4];
            UnpackColor(p.color2, t);
            glColor4fv(t);
            glBegin(GL_QUADS);
            glTexCoord2f(0, 0);
            glVertex2f(p.p[0][0], p.p[0][1]);
            glTexCoord2f(1, 0);
            glVertex2f(p.p[1][0], p.p[0][1]);
            glTexCoord2f(1, 1);
            glVertex2f(p.p[1][0], p.p[1][1]);
            glTexCoord2f(0, 1);
            glVertex2f(p.p[0][0], p.p[1][1]);
            glEnd();
            verts += 4;
            break;
        }
        default:
            break;
    }
}

void DrawList(render::Stage stage, std::vector<Primitive>& list, FrameStats& stats) {
    for (auto& p : list) {
        if (stage == render::Stage::Hud)
            DrawHudOne(p, stats.primitives, stats.vertices);
        else
            DrawWorldOne(p, stats.primitives, stats.vertices);
    }
}
}  // namespace

FrameStats DrawStage(render::Stage stage, std::vector<Primitive>& a, std::vector<Primitive>& b) {
    FrameStats stats;
    if (a.empty() && b.empty()) return stats;

    render::WindowSize(&g_windowW, &g_windowH);
    g_haveCam = render::GetCamera(&g_cam);
    if (stage != render::Stage::Hud && !g_haveCam) return stats;

    uint32_t token = render::PushState();
    if (!token) return stats;

    if (stage == render::Stage::Hud) {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, g_windowW, g_windowH, 0, -1, 1);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }
    DrawList(stage, a, stats);
    DrawList(stage, b, stats);

    render::PopState(token);
    return stats;
}
}  // namespace melange::mirage::drawgl
