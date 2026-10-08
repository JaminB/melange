#pragma once
#include <cstdint>
#include <vector>

#include "melange/render.h"
#include "render/mirage/draw_queue.h"

namespace melange::mirage::drawgl {
struct FrameStats {
    uint32_t primitives = 0, vertices = 0;
};
// `sprites` (World and WorldLate only) are drawn after the primitives, back to front, in one vertex array.
FrameStats DrawStage(render::Stage stage, std::vector<drawqueue::Primitive>& a, std::vector<drawqueue::Primitive>& b,
                     const std::vector<draw::Sprite>& sprites);
}  // namespace melange::mirage::drawgl
