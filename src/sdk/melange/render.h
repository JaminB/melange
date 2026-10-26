#pragma once
#include <cstdint>
namespace melange::render {
enum class Pass : uint8_t { None = 0, Shadow = 1, PiP = 2, Main = 3 };
Pass CurrentPass();
bool Ready();  // render manager present and a GL context current on this thread

// Matrices are float[16] ready for glLoadMatrixf (the engine's own layout; translation in [12..14]).
struct Camera {
    float view[16], invView[16], proj[16], viewProj[16];
    float pos[3], fwd[3], up[3];  // world space, from invView
    float nearZ, farZ;            // derived from proj
    bool valid;
};
bool GetCamera(Camera* out);  // main camera; refreshed once per main pass; reflects a camera override
// The game's own main camera, as GetCamera would report it without a camera override.
bool GetGameCamera(Camera* out);

// Render-only camera override: the main camera draws the frame from `pos` toward `target` (and, with fovDeg > 0, with
// that vertical field of view) while the game's logical camera, which Wormsign compares between machines, is never
// written. The engine's render camera (XCamera) holds the override only while its Render call runs and gets its own
// values back right after, so game code outside rendering never sees it. GetCamera and WorldToScreen report it
// everywhere. One owner at a time; it ends `frames` frames (1..600, 0 = 30) after its last refresh, at match end,
// and on ClearCameraOverride. Main thread.
struct CameraOverride {
    float pos[3], target[3], up[3];
    float fovDeg;  // 5..150, or 0 = the game's
};
enum class OverrideResult : uint8_t { Ok, Busy, Invalid, Unavailable };
OverrideResult SetCameraOverride(const void* owner, const CameraOverride& o, int frames = 0);
bool ClearCameraOverride(const void* owner);  // nullptr: any owner; true if one was cleared
bool CameraOverridden();                       // an override is set (it draws from the next Render on)
// Pixels, origin top-left of the window; depth in [0,1] as the depth buffer stores it. False if behind the camera.
bool WorldToScreen(const float world[3], float* x, float* y, float* depth);
void WindowSize(int* w, int* h);

struct Timing {
    double busyMsP50, busyMsP95;  // Frame event -> previous SwapBuffers return, last 240 frames
    double frameMsP50, fps;
    uint64_t frames;
};
Timing GetTiming();  // any thread (copy)

// Fixed points in the main-pass draw list where Melange runs mod code.
enum class Stage : uint8_t {
    World,      // after the world geometry, sea and worms, before particles; the depth buffer is complete for opaque
    WorldLate,  // after all world content including particles, before worm labels and the engine HUD
    PostWorld,  // post-FX on the world only: after WorldLate callbacks, before worm labels and the engine HUD
    Hud,        // after the engine HUD, before Composite (still in the scene FBO)
    Final,      // post-FX on the whole frame: just before the engine's Composite copy to the back buffer
    Count,
    // Not an engine stage (AddStageCallback refuses it): draw callbacks only, run at present time on the back buffer,
    // after Composite and every post effect, before the overlay; in the frontend, lobby and loading screens too.
    Screen,
};
// Runs on the main thread in the main pass only, with the engine's scene FBO bound and GL state as the engine left
// it. The callback must leave GL state as it found it (PushState/PopState). Lower `order` runs first.
using StageFn = void (*)(Stage stage, void* user);
int AddStageCallback(Stage stage, StageFn fn, void* user, int order = 0);  // any thread; 0 = failure
void RemoveStageCallback(int handle);                                     // any thread
struct StageInfo { int bucket; bool post; bool installed; uint64_t calls; };
StageInfo GetStageInfo(Stage stage);

// The engine's scene render target at the current stage, read from the bound FBO's attachments.
struct SceneTargets { unsigned fbo, colorTex, depthTex; int w, h; bool valid; };
SceneTargets GetSceneTargets();

// Saves all GL state and neutralises the engine's leftovers (ARB programs off, GLSL program 0, alpha/stencil/fog/
// lighting/cull off, texture unit 0, no client arrays, no buffers bound). Unlike the overlay guard it works with an
// FBO bound. Returns a token for PopState; nesting is allowed up to 4 deep.
uint32_t PushState();
void PopState(uint32_t token);
}
