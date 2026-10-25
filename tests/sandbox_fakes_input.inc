// sandbox_selftest fakes for the Melange 0.12 wum.input functions (owner: C2). Included inside namespace melange,
// after the other fakes. The real mod input core (input_regions_core.cpp) runs on this Host: the cursor, the time,
// the match and the turn are set by the cases, and events go through the real lua::PostEventTo.
}  // namespace melange (the test file includes this inside it; the header must be at file scope)
#include "input/gesture.h"
#include "render/input_logic.h"
#include "render/input_regions.h"
namespace melange {
namespace fake_input {
inline bool g_allowed = true, g_overlay = false, g_fg = true, g_match = false, g_fe = true, g_hookOk = true, g_turn = false;
inline float g_x = 100, g_y = 100;  // the frontend cursor, and the OS cursor in pointer mode
inline uint64_t g_frame = 1, g_now = 50000;
inline uint8_t g_mods = 0;
inline std::wstring g_clip = L"https://youtu.be/dQw4w9WgXcQ\r\n";
inline int g_started = 0, g_ended = 0;
inline std::vector<int> g_held;

inline void Install() {
    render::modinput::Host h{};
    h.allowed = [] { return g_allowed; };
    h.overlayCapturing = [] { return g_overlay; };
    h.foreground = [] { return g_fg; };
    h.inMatch = [] { return g_match; };
    h.frame = [] { return g_frame; };
    h.nowMs = [] { return g_now; };
    h.windowSize = [](int* w, int* h2) {
        *w = 1280;
        *h2 = 720;
    };
    h.feCursor = [](float* x, float* y) {
        *x = g_x;
        *y = g_y;
        return g_fe;
    };
    h.osCursor = [](float* x, float* y) {
        *x = g_x;
        *y = g_y;
        return true;
    };
    h.modifiers = [] { return g_mods; };
    h.clipboard = [](std::wstring* t) {
        *t = g_clip;
        return true;
    };
    h.pointerHook = [] { return g_hookOk; };
    h.pointerStarted = [](const std::vector<int>& held) {
        ++g_started;
        g_held = held;
    };
    h.pointerEnded = [](int* cx, int* cy) {
        ++g_ended;
        *cx = 640;
        *cy = 360;
        return true;
    };
    h.myTurnStarted = [](bool reset) {
        if (reset) g_turn = false;
        return g_turn;
    };
    h.post = [](const std::string& mod, const char* name, const std::string& json) {
        lua::PostEventTo(mod.c_str(), name, json.c_str());
    };
    render::modinput::SetHost(h);
}
}  // namespace fake_input
