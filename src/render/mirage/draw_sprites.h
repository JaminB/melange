#pragma once
// Sprite batching for melange::draw::DrawSprite (view-space quads sorted back to front, grouped into runs of one
// texture and blend mode) and the CPU mip chain of LoadTexture. Free of GL and engine access.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "melange/draw.h"

namespace melange::mirage::drawsprites {

struct V3 {
    float x, y, z;
};
inline V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(V3 a) { return std::sqrt(Dot(a, a)); }

// glLoadMatrixf layout (column-major, translation in [12..14]).
inline V3 ViewPoint(const float m[16], const float p[3]) {
    return {m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12], m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14]};
}
inline V3 ViewDir(const float m[16], const float d[3]) {
    return {m[0] * d[0] + m[4] * d[1] + m[8] * d[2], m[1] * d[0] + m[5] * d[1] + m[9] * d[2], m[2] * d[0] + m[6] * d[1] + m[10] * d[2]};
}

inline bool IsBillboard(const draw::Sprite& s) { return s.axis[0] * s.axis[0] + s.axis[1] * s.axis[1] + s.axis[2] * s.axis[2] <= 1e-12f; }

// False for a sprite DrawSprite skips: a non-finite value, halfW <= 0, or a stretched one with halfL == 0.
inline bool Drawable(const draw::Sprite& s) {
    const float v[8] = {s.pos[0], s.pos[1], s.pos[2], s.halfW, s.halfL, s.axis[0], s.axis[1], s.axis[2]};
    for (float f : v)
        if (!std::isfinite(f)) return false;
    if (!(s.halfW > 0.f)) return false;
    return IsBillboard(s) || s.halfL != 0.f;
}

// The tint as the vertex colour: premultiplied by its own alpha for SpriteBlend::Premultiplied.
inline draw::Rgba VertexColor(const draw::Sprite& s) {
    if (s.blend != draw::SpriteBlend::Premultiplied) return s.color;
    const uint32_t c = s.color, a = c >> 24;
    auto ch = [&](int shift) { return ((((c >> shift) & 255u) * a + 127u) / 255u) << shift; };
    return ch(0) | ch(8) | ch(16) | (a << 24);
}

// Four view-space corners, in GL_QUADS order: tail-left (u 0, v 0), tail-right, head-right, head-left (u 0, v 1).
// A billboard's "tail" is its top edge, so the image shows upright. False if the sprite is wholly behind the eye.
inline bool Expand(const draw::Sprite& s, const float view[16], draw::Vertex out[4]) {
    const V3 c = ViewPoint(view, s.pos);
    const float hw = s.halfW, hl = std::fabs(s.halfL);
    V3 t, h, side;
    if (IsBillboard(s)) {
        if (c.z - hw * 1.4143f > 0.f) return false;
        t = {c.x, c.y + hw, c.z};
        h = {c.x, c.y - hw, c.z};
        side = {hw, 0.f, 0.f};
    } else {
        if (c.z - (hw + hl) > 0.f) return false;
        V3 a = ViewDir(view, s.axis);
        a = Mul(a, 1.f / Len(a));
        const float cl = Len(c);
        const V3 e = cl > 1e-6f ? Mul(c, 1.f / cl) : V3{0.f, 0.f, -1.f};  // eye -> centre
        const V3 cr = Cross(a, e);
        const float f = Len(cr);  // sin of the angle between the axis and the eye ray: screen foreshortening
        const float minHalf = (std::min)(hw, hl);
        if (f > 1e-6f && hl * f >= minHalf) {
            t = Sub(c, Mul(a, hl));
            h = Add(c, Mul(a, hl));
            side = Mul(cr, hw / f);
        } else {
            // Nearly head-on: keep the axis's screen direction (screen up if it has none) at the minimum length, in
            // the plane facing the eye. Continuous with the branch above at hl * f == minHalf.
            V3 dir = Sub(a, Mul(e, Dot(a, e)));
            float dl = Len(dir);
            if (dl <= 1e-6f) {
                const V3 up{0.f, 1.f, 0.f};
                dir = Sub(up, Mul(e, Dot(up, e)));
                dl = Len(dir);
                if (dl <= 1e-6f) dir = {1.f, 0.f, 0.f}, dl = 1.f;
            }
            dir = Mul(dir, 1.f / dl);
            t = Sub(c, Mul(dir, minHalf));
            h = Add(c, Mul(dir, minHalf));
            const V3 sd = Cross(dir, e);
            side = Mul(sd, hw / (std::max)(Len(sd), 1e-6f));
        }
    }
    const draw::Rgba col = VertexColor(s);
    const V3 p[4] = {Sub(t, side), Add(t, side), Add(h, side), Sub(h, side)};
    static const float kUv[4][2] = {{0.f, 0.f}, {1.f, 0.f}, {1.f, 1.f}, {0.f, 1.f}};
    for (int i = 0; i < 4; ++i) out[i] = {{p[i].x, p[i].y, p[i].z}, col, {kUv[i][0], kUv[i][1]}};
    return true;
}

struct Run {
    unsigned texture;
    draw::SpriteBlend blend;
    uint32_t first, count;  // vertices into Batch::verts, GL_QUADS
};

// One stage's sprites as a single vertex array: back to front by view depth (ties in submission order), split into
// a run wherever the texture or blend mode changes. Additive blending commutes (no depth writes, saturating adds),
// so each unbroken stretch of additive sprites in that order is regrouped by texture: fewer runs, the same image.
struct Batch {
    std::vector<draw::Vertex> verts;
    std::vector<Run> runs;
    std::vector<std::pair<float, uint32_t>> order;  // scratch

    void Build(const std::vector<draw::Sprite>& in, const float view[16]) {
        verts.clear();
        runs.clear();
        order.clear();
        order.reserve(in.size());
        for (uint32_t i = 0; i < in.size(); ++i) order.emplace_back(ViewPoint(view, in[i].pos).z, i);
        std::sort(order.begin(), order.end());  // most negative view z (farthest) first; equal z by index
        auto additive = [&](size_t k) { return in[order[k].second].blend == draw::SpriteBlend::Additive; };
        for (size_t a = 0; a < order.size();) {
            if (!additive(a)) {
                ++a;
                continue;
            }
            size_t b = a + 1;
            while (b < order.size() && additive(b)) ++b;
            if (b - a > 2)
                std::sort(order.begin() + static_cast<ptrdiff_t>(a), order.begin() + static_cast<ptrdiff_t>(b),
                          [&](const std::pair<float, uint32_t>& x, const std::pair<float, uint32_t>& y) {
                              const unsigned tx = in[x.second].texture, ty = in[y.second].texture;
                              return tx != ty ? tx < ty : x.second < y.second;
                          });
            a = b;
        }
        verts.resize(in.size() * 4);
        uint32_t n = 0;
        for (const auto& [z, i] : order) {
            const draw::Sprite& s = in[i];
            if (!Expand(s, view, &verts[n])) continue;
            if (runs.empty() || runs.back().texture != s.texture || runs.back().blend != s.blend)
                runs.push_back({s.texture, s.blend, n, 0});
            runs.back().count += 4;
            n += 4;
        }
        verts.resize(n);
    }
};

// ---------------------------------------------------------------- mips
// True if every pixel has r, g, b <= a: the image is (or can only be) premultiplied.
inline bool LooksPremultiplied(const uint8_t* rgba, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i, rgba += 4)
        if (rgba[0] > rgba[3] || rgba[1] > rgba[3] || rgba[2] > rgba[3]) return false;
    return true;
}

inline int MipSize(int n) { return n > 1 ? n / 2 : 1; }

// Next level of an RGBA8 image with a 2x2 box filter (an odd last row/column is dropped, as GL sizes levels).
// premultiplied: channels average as stored. Otherwise colour averages weighted by alpha and is stored straight,
// so fully transparent texels (often black) do not darken the edges of a straight-alpha sprite at a distance.
inline void Downsample(const uint8_t* src, int w, int h, uint8_t* dst, bool premultiplied) {
    const int nw = MipSize(w), nh = MipSize(h);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            const int x0 = (std::min)(2 * x, w - 1), x1 = (std::min)(2 * x + 1, w - 1);
            const int y0 = (std::min)(2 * y, h - 1), y1 = (std::min)(2 * y + 1, h - 1);
            const uint8_t* q[4] = {src + 4 * (y0 * w + x0), src + 4 * (y0 * w + x1), src + 4 * (y1 * w + x0),
                                   src + 4 * (y1 * w + x1)};
            uint8_t* d = dst + 4 * (y * nw + x);
            const uint32_t sa = q[0][3] + q[1][3] + q[2][3] + q[3][3];
            for (int k = 0; k < 3; ++k) {
                if (premultiplied || sa == 0) {
                    d[k] = static_cast<uint8_t>((q[0][k] + q[1][k] + q[2][k] + q[3][k] + 2u) / 4u);
                } else {
                    const uint32_t sum = q[0][k] * q[0][3] + q[1][k] * q[1][3] + q[2][k] * q[2][3] + q[3][k] * q[3][3];
                    d[k] = static_cast<uint8_t>((std::min<uint32_t>)(255u, (sum + sa / 2u) / sa));
                }
            }
            d[3] = static_cast<uint8_t>((sa + 2u) / 4u);
        }
}

}  // namespace melange::mirage::drawsprites
