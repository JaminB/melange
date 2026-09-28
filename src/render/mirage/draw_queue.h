#pragma once
// Per-stage primitive queues for melange::draw, free of GL and engine access.
#include <cstdint>
#include <string>
#include <vector>

#include "melange/draw.h"
#include "melange/render.h"

namespace melange::mirage::drawqueue {

enum class Kind : uint8_t { Line, Box, Sphere, Axes, Quad, Text, Mesh, HudLine, HudRect, HudText, HudImage };

// HUD kinds keep x,y pairs in p[i][0..1].
struct Primitive {
    Kind kind = Kind::Line;
    uint32_t flags = 0;
    draw::Rgba color = 0;
    draw::Rgba color2 = 0xffffffff;
    float widthPx = 0, sizePx = 0, scalar = 0;
    float p[4][3] = {};
    int framesLeft = 1;
    bool filled = false;
    std::string text;
    unsigned texture = 0;
    float model[16] = {};
    bool hasModel = false;
    std::vector<draw::Vertex> meshV;
    std::vector<uint16_t> meshI;
};

inline render::Stage RouteStage(Kind kind, uint32_t flags) {
    switch (kind) {
        case Kind::HudLine:
        case Kind::HudRect:
        case Kind::HudText:
        case Kind::HudImage:
            return render::Stage::Hud;
        default:
            return (flags & draw::kDepthTest) ? render::Stage::World : render::Stage::WorldLate;
    }
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

}  // namespace melange::mirage::drawqueue
