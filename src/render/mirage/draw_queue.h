#pragma once
// Per-stage primitive queues for melange::draw, free of GL and engine access.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "melange/draw.h"
#include "melange/render.h"

namespace melange::mirage::drawqueue {

// HudImageUV, HudVideo: uv[] holds u0, v0, u1, v1. ClipPush: the clip rectangle in p[0..1]; ClipPop: nothing.
enum class Kind : uint8_t {
    Line, Box, Sphere, Axes, Quad, Text, Mesh, HudLine, HudRect, HudText, HudImage,
    HudImageUV, HudVideo, ClipPush, ClipPop,
};

inline bool IsHud(Kind kind) {
    switch (kind) {
        case Kind::HudLine:
        case Kind::HudRect:
        case Kind::HudText:
        case Kind::HudImage:
        case Kind::HudImageUV:
        case Kind::HudVideo:
        case Kind::ClipPush:
        case Kind::ClipPop:
            return true;
        default:
            return false;
    }
}

// HUD kinds keep x,y pairs in p[i][0..1].
struct Primitive {
    Kind kind = Kind::Line;
    uint32_t flags = 0;
    draw::Rgba color = 0;
    draw::Rgba color2 = 0xffffffff;
    float widthPx = 0, sizePx = 0, scalar = 0;
    float p[4][3] = {};
    float uv[4] = {0, 0, 1, 1};
    int framesLeft = 1;
    bool filled = false;
    std::string text;
    unsigned texture = 0;
    float model[16] = {};
    bool hasModel = false;
    std::vector<draw::Vertex> meshV;
    std::vector<uint16_t> meshI;
    bool textEx = false;          // HudText from HudTextEx: laid out with textOpts
    draw::TextOptions textOpts;
};

// `current` is the stage whose draw callbacks are running (Stage::Count outside them): HUD kinds called inside a
// Screen callback stay at Screen; everything else goes where it always did.
inline render::Stage RouteStage(Kind kind, uint32_t flags, render::Stage current = render::Stage::Count) {
    if (IsHud(kind)) return current == render::Stage::Screen ? render::Stage::Screen : render::Stage::Hud;
    return (flags & draw::kDepthTest) ? render::Stage::World : render::Stage::WorldLate;
}

// Push(p, false) waits for the next pass; Push(p, true) is drawn in the current one.
class StageQueue {
  public:
    void Push(Primitive p, bool insideThisStage) {
        if (insideThisStage)
            immediate_.push_back(std::move(p));
        else
            outside_.push_back(std::move(p));
    }

    bool Idle() const { return outside_.empty() && immediate_.empty() && carried_.empty() && thisPass_.empty(); }

    std::vector<Primitive>& BeginPass() {
        thisPass_ = std::move(carried_);
        carried_.clear();
        for (auto& p : outside_) thisPass_.push_back(std::move(p));
        outside_.clear();
        return thisPass_;
    }
    std::vector<Primitive>& Immediate() { return immediate_; }

    void EndPass() {
        Age(thisPass_);
        Age(immediate_);
        thisPass_.clear();
        immediate_.clear();
    }

    size_t QueuedCount() const { return outside_.size() + carried_.size(); }

  private:
    void Age(std::vector<Primitive>& v) {
        for (auto& p : v)
            if (--p.framesLeft > 0) carried_.push_back(std::move(p));
    }
    std::vector<Primitive> outside_, immediate_, thisPass_, carried_;
};

inline void Unpack(draw::Rgba c, float rgba[4]) {
    rgba[0] = (c & 0xff) / 255.f;
    rgba[1] = ((c >> 8) & 0xff) / 255.f;
    rgba[2] = ((c >> 16) & 0xff) / 255.f;
    rgba[3] = ((c >> 24) & 0xff) / 255.f;
}

// ---- Melange 0.12 geometry (pure: draw.cpp and draw_gl.cpp use it, draw_queue_selftest checks it).

struct UvQuad {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
};

// Nine-slice cells of the window rectangle (x0, y0)-(x1, y1) from the texture rectangle uv[] of a texW x texH texture.
// Insets l, t, r, b are texture pixels; they shrink evenly to fit the source rectangle, and the drawn corners
// (scale * inset) shrink evenly to fit the window rectangle. Cells of zero area are left out. Returns the count (<= 9).
inline int NineSlice(float x0, float y0, float x1, float y1, int texW, int texH, float l, float t, float r, float b,
                     float scale, const float uv[4], UvQuad out[9]) {
    auto finite = [](float v) { return std::isfinite(v); };
    if (texW <= 0 || texH <= 0 || !finite(x0) || !finite(y0) || !finite(x1) || !finite(y1)) return 0;
    for (int i = 0; i < 4; ++i)
        if (!finite(uv[i])) return 0;
    if (!(x1 > x0) || !(y1 > y0)) return 0;
    float in[4] = {l, t, r, b};
    for (float& v : in) v = finite(v) && v > 0 ? v : 0;
    if (!(scale > 0) || !finite(scale)) scale = 1;
    const float su0 = uv[0], sv0 = uv[1], su1 = uv[2], sv1 = uv[3];
    const float srcW = std::fabs(su1 - su0) * texW, srcH = std::fabs(sv1 - sv0) * texH;
    if (in[0] + in[2] > srcW) {
        const float k = srcW / (in[0] + in[2]);
        in[0] *= k;
        in[2] *= k;
    }
    if (in[1] + in[3] > srcH) {
        const float k = srcH / (in[1] + in[3]);
        in[1] *= k;
        in[3] *= k;
    }
    float d[4] = {in[0] * scale, in[1] * scale, in[2] * scale, in[3] * scale};
    const float w = x1 - x0, h = y1 - y0;
    if (d[0] + d[2] > w) {
        const float k = w / (d[0] + d[2]);
        d[0] *= k;
        d[2] *= k;
    }
    if (d[1] + d[3] > h) {
        const float k = h / (d[1] + d[3]);
        d[1] *= k;
        d[3] *= k;
    }
    // A flipped uv rectangle (u1 < u0) mirrors the picture; the insets stay on their screen sides.
    const float du = su1 >= su0 ? 1.f : -1.f, dv = sv1 >= sv0 ? 1.f : -1.f;
    const float xs[4] = {x0, x0 + d[0], x1 - d[2], x1};
    const float ys[4] = {y0, y0 + d[1], y1 - d[3], y1};
    const float us[4] = {su0, su0 + du * in[0] / texW, su1 - du * in[2] / texW, su1};
    const float vs[4] = {sv0, sv0 + dv * in[1] / texH, sv1 - dv * in[3] / texH, sv1};
    int n = 0;
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) {
            if (xs[col + 1] - xs[col] <= 0 || ys[row + 1] - ys[row] <= 0) continue;
            out[n++] = {xs[col], ys[row], xs[col + 1], ys[row + 1], us[col], vs[row], us[col + 1], vs[row + 1]};
        }
    return n;
}

enum class Fit : uint8_t { Contain, Cover, Stretch };

// Where a picture of display aspect `aspect` (w / h), whose pixels fill (0, 0)-(u1, v1) of its texture, goes in the
// rectangle. Contain letterboxes (centred, the whole picture), cover fills the rectangle and crops the texture
// rectangle (centred), stretch fills it all. False for an empty rectangle, or a bad aspect unless stretched.
inline bool FitPicture(float x0, float y0, float x1, float y1, double aspect, float u1, float v1, Fit fit, UvQuad* out) {
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return false;
    if (!(x1 > x0) || !(y1 > y0)) return false;
    *out = {x0, y0, x1, y1, 0, 0, u1, v1};
    if (fit == Fit::Stretch) return true;
    if (!(aspect > 0) || !std::isfinite(aspect)) return false;
    const float w = x1 - x0, h = y1 - y0;
    const double box = static_cast<double>(w) / h;
    if (fit == Fit::Contain) {
        if (aspect > box) {
            const float ph = static_cast<float>(w / aspect), cy = (y0 + y1) * 0.5f;
            out->y0 = cy - ph * 0.5f;
            out->y1 = cy + ph * 0.5f;
        } else {
            const float pw = static_cast<float>(h * aspect), cx = (x0 + x1) * 0.5f;
            out->x0 = cx - pw * 0.5f;
            out->x1 = cx + pw * 0.5f;
        }
        return true;
    }
    if (aspect > box) {  // wider than the rectangle: crop left and right
        const float keep = static_cast<float>(box / aspect) * u1;
        out->u0 = (u1 - keep) * 0.5f;
        out->u1 = out->u0 + keep;
    } else {
        const float keep = static_cast<float>(aspect / box) * v1;
        out->v0 = (v1 - keep) * 0.5f;
        out->v1 = out->v0 + keep;
    }
    return true;
}

// A clip rectangle in window pixels (origin top-left) as a glScissor box (origin bottom-left) in the viewport `vp`
// (x, y, w, h) that the window's winW x winH pixels map onto. Clamped to the window, rounded outward to whole pixels.
struct Scissor {
    int x = 0, y = 0, w = 0, h = 0;
};
inline Scissor ClipToScissor(float x0, float y0, float x1, float y1, int winW, int winH, const int vp[4]) {
    Scissor s;
    if (winW <= 0 || winH <= 0) return s;
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return s;
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    const float W = static_cast<float>(winW), H = static_cast<float>(winH);
    const float sx = vp[2] / W, sy = vp[3] / H;
    const float left = vp[0] + std::clamp(x0, 0.f, W) * sx;
    const float right = vp[0] + std::clamp(x1, 0.f, W) * sx;
    const float bottom = vp[1] + (H - std::clamp(y1, 0.f, H)) * sy;
    const float top = vp[1] + (H - std::clamp(y0, 0.f, H)) * sy;
    s.x = static_cast<int>(std::floor(left));
    s.y = static_cast<int>(std::floor(bottom));
    s.w = std::max(0, static_cast<int>(std::ceil(right)) - s.x);
    s.h = std::max(0, static_cast<int>(std::ceil(top)) - s.y);
    return s;
}

inline Scissor Intersect(const Scissor& a, const Scissor& b) {
    Scissor s;
    s.x = std::max(a.x, b.x);
    s.y = std::max(a.y, b.y);
    s.w = std::max(0, std::min(a.x + a.w, b.x + b.w) - s.x);
    s.h = std::max(0, std::min(a.y + a.h, b.y + b.h) - s.y);
    return s;
}

// The clip stack of one stage pass. Push intersects with the open clip; past kMaxClipDepth a push is only counted (so
// its Pop pairs) and the deepest real clip stays.
constexpr int kMaxClipDepth = 4;
class ClipStack {
  public:
    void Reset() { depth_ = 0; }
    void Push(const Scissor& s) {
        if (depth_ < kMaxClipDepth) stack_[depth_] = depth_ ? Intersect(stack_[depth_ - 1], s) : s;
        ++depth_;
    }
    void Pop() {
        if (depth_ > 0) --depth_;
    }
    bool Active() const { return depth_ > 0; }
    int Depth() const { return depth_; }
    Scissor Top() const { return depth_ ? stack_[std::min(depth_, kMaxClipDepth) - 1] : Scissor{}; }

  private:
    Scissor stack_[kMaxClipDepth];
    int depth_ = 0;
};

}  // namespace melange::mirage::drawqueue
