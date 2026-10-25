// The "screen" draw stage (stages_screen.h): draw callbacks registered for render::Stage::Screen and the HUD
// primitives they submit, drawn on the back buffer at present time. The pass itself lives in draw.cpp (it shares the
// queues, callback isolation and GL drawing of the other stages); DrawStage binds the back buffer and the window
// viewport inside PushState/PopState.
#include "render/mirage/stages_screen.h"

#include "render/mirage/draw_gl.h"

namespace melange::mirage {
void RunScreenStage() { draw::RunScreenPass(); }
}  // namespace melange::mirage
