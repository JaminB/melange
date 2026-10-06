// Module "Display": borderless fullscreen on the game's own window (View > Fullscreen, [Display] Hotkey, Alt+Enter by
// default), remembered in [Display] Fullscreen and applied as soon as the game's window is up. The window is restyled
// as a popup and sized to cover its monitor, and back: no display-mode change, and the window and GL context stay the
// same ones (the engine's own ChangeDisplay re-creates both, so it is never called). The engine then renders at the
// new size: its viewport is set to the client area and PCPostProcess rebuilds its scene targets from it (the switch
// MirageSupersample uses to change supersampling live), so 3D, HUD and menus draw at the monitor's resolution.
#include <windows.h>
#include <GL/gl.h>

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"
#include "render/display_logic.h"
#include "render/input_logic.h"
#include "render/mirage/engine.h"
#include "render/mirage/supersample.h"

namespace {
namespace logic = melange::display::logic;
constexpr int kMaxSyncAttempts = 3;

struct Saved {
    bool valid = false;
    LONG style = 0, ex = 0;
    RECT rect{};
};

std::atomic<bool> g_want{false};   // the setting: [Display] Fullscreen, flipped by the menu, the hotkey and the test verb
std::atomic<int> g_request{-1};    // -1 none, 0 windowed, 1 borderless; carried out on the next frame (main thread)
// Main thread only from here on.
HWND g_hwnd = nullptr;
bool g_on = false, g_exclusive = false, g_touched = false;
Saved g_saved;
bool g_syncPending = false;
int g_syncAttempts = 0;
uint64_t g_nextSync = 0, g_nextCheck = 0;
std::string g_hotkey = "-";

bool MonitorRects(HWND hwnd, RECT* mon, RECT* work) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    if (mon) *mon = mi.rcMonitor;
    if (work) *work = mi.rcWork;
    return true;
}

BOOL CALLBACK AddWorkArea(HMONITOR m, HDC, LPRECT, LPARAM out) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (GetMonitorInfoW(m, &mi)) reinterpret_cast<std::vector<RECT>*>(out)->push_back(mi.rcWork);
    return TRUE;
}

std::vector<RECT> WorkAreas() {
    std::vector<RECT> v;
    EnumDisplayMonitors(nullptr, nullptr, &AddWorkArea, reinterpret_cast<LPARAM>(&v));
    return v;
}

// The engine's viewport and scene targets follow the client area from the next frames on.
void StartSync() {
    g_syncPending = true;
    g_syncAttempts = 0;
    g_nextSync = 0;
    g_touched = true;
}

bool Enter(HWND hwnd) {
    if (g_on) return true;
    RECT mon{}, win{};
    const LONG style = static_cast<LONG>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    const LONG ex = static_cast<LONG>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    if (!MonitorRects(hwnd, &mon, nullptr) || !GetWindowRect(hwnd, &win)) {
        LOG_WARN("[display] fullscreen: the game window's monitor could not be read (error %lu)", GetLastError());
        return false;
    }
    if (logic::LooksExclusive(style, win, mon)) {
        g_exclusive = true;
        LOG_WARN("[display] the game runs in its own exclusive fullscreen (/FS in local.cfg): borderless fullscreen stays off. "
                 "Turn on Fullscreen in Melange.exe Settings > Display to switch to Melange's");
        return false;
    }
    g_saved = Saved{true, style, ex, win};
    SetWindowLongPtrW(hwnd, GWL_STYLE, logic::BorderlessStyle(style));
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, logic::BorderlessExStyle(ex));
    SetWindowPos(hwnd, HWND_TOP, mon.left, mon.top, logic::Width(mon), logic::Height(mon),
                 SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    g_on = true;
    LOG_INFO("[display] borderless fullscreen on: %dx%d at (%ld, %ld) (was %dx%d at (%ld, %ld), style 0x%08lx)",
             logic::Width(mon), logic::Height(mon), mon.left, mon.top, logic::Width(win), logic::Height(win), win.left, win.top,
             static_cast<unsigned long>(style));
    StartSync();
    return true;
}

bool Leave(HWND hwnd) {
    if (!g_on) return true;
    RECT work{};
    MonitorRects(hwnd, nullptr, &work);
    const RECT r = logic::RestoreRect(g_saved.rect, WorkAreas(), work);
    SetWindowLongPtrW(hwnd, GWL_STYLE, g_saved.style);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, g_saved.ex);
    SetWindowPos(hwnd, nullptr, r.left, r.top, logic::Width(r), logic::Height(r),
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    g_on = false;
    LOG_INFO("[display] windowed: %dx%d at (%ld, %ld)", logic::Width(r), logic::Height(r), r.left, r.top);
    StartSync();
    return true;
}

// Sets the GL viewport to the client area and has PCPostProcess rebuild its targets from it; then checks, a few
// frames later, that its cached window viewport (Composite's) took the new size.
void Sync(HWND hwnd, uint64_t frame) {
    if (!g_syncPending || frame < g_nextSync || IsIconic(hwnd) || !wglGetCurrentContext()) return;
    RECT c{};
    GetClientRect(hwnd, &c);
    const int cw = logic::Width(c), ch = logic::Height(c);
    if (cw <= 0 || ch <= 0) return;
    int vp[4] = {};
    if (melange::mirage::engine::WindowViewport(vp) && logic::ViewportMatches(vp, cw, ch)) {
        int sw = 0, sh = 0;
        melange::mirage::engine::SceneSize(&sw, &sh);
        LOG_INFO("[display] rendering at %dx%d (scene %dx%d)", cw, ch, sw, sh);
        g_syncPending = false;
        return;
    }
    if (g_syncAttempts >= kMaxSyncAttempts) {
        LOG_WARN("[display] the engine's viewport stayed %dx%d for a %dx%d window after %d tries: the picture may not fill it",
                 vp[2], vp[3], cw, ch, kMaxSyncAttempts);
        g_syncPending = false;
        return;
    }
    glViewport(0, 0, cw, ch);
    if (!melange::mirage::supersample::RebuildTargets(cw, ch)) {
        g_nextSync = frame + 30;  // the renderer is not up yet
        return;
    }
    ++g_syncAttempts;
    g_nextSync = frame + 20;
}

// Every two seconds: the monitor may have changed mode (or the window moved to another one) while borderless, and the
// engine may have reset its viewport.
void Recheck(HWND hwnd, uint64_t frame) {
    if (!g_touched || g_syncPending || frame < g_nextCheck || IsIconic(hwnd)) return;
    g_nextCheck = frame + 120;
    if (g_on) {
        RECT mon{}, win{};
        if (MonitorRects(hwnd, &mon, nullptr) && GetWindowRect(hwnd, &win) && !logic::SameRect(win, mon)) {
            SetWindowPos(hwnd, nullptr, mon.left, mon.top, logic::Width(mon), logic::Height(mon),
                         SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
            LOG_INFO("[display] borderless window re-fitted to its monitor: %dx%d at (%ld, %ld)", logic::Width(mon),
                     logic::Height(mon), mon.left, mon.top);
            StartSync();
            return;
        }
    }
    RECT c{};
    GetClientRect(hwnd, &c);
    int vp[4] = {};
    if (melange::mirage::engine::WindowViewport(vp) && !logic::ViewportMatches(vp, logic::Width(c), logic::Height(c))) {
        LOG_INFO("[display] the engine's viewport is %dx%d for a %dx%d window: syncing it", vp[2], vp[3], logic::Width(c),
                 logic::Height(c));
        StartSync();
    }
}

void OnFrame() {
    const uint64_t frame = melange::events::FrameCount();
    if (frame < 2) return;  // the first frame's window is the one the engine just showed
    HWND hwnd = static_cast<HWND>(melange::events::GameWindow());
    if (!hwnd || !IsWindow(hwnd)) return;
    if (hwnd != g_hwnd) {
        if (g_hwnd) LOG_WARN("[display] the game window changed (%p -> %p)", static_cast<void*>(g_hwnd), static_cast<void*>(hwnd));
        g_hwnd = hwnd;
        g_on = false;
        g_saved = Saved{};
        g_exclusive = false;
        if (g_want) g_request = 1;
    }
    const int req = g_request.exchange(-1);
    if (req >= 0) {
        if (IsIconic(hwnd)) {
            g_request = req;  // minimised: once it is back
        } else if (req == 1) {
            if (!Enter(hwnd) && g_exclusive) g_want = false;
        } else {
            Leave(hwnd);
        }
    }
    Sync(hwnd, frame);
    Recheck(hwnd, frame);
}

void SetWanted(bool on, const char* why) {
    if (on && g_exclusive) {
        LOG_WARN("[display] %s: refused, the game runs in its own exclusive fullscreen (/FS in local.cfg)", why);
        return;
    }
    g_want = on;
    g_request = on ? 1 : 0;
    melange::config::SetString("Display", "Fullscreen", on ? "1" : "0");
    LOG_INFO("[display] %s: fullscreen %s", why, on ? "on" : "off");
}

void ToggleAction(void*) { SetWanted(!g_want, "toggle"); }
bool Checked(void*) { return g_want; }

// display.fullscreen 0|1|toggle; display.info logs the window, monitor and viewport.
bool VerbFullscreen(std::string_view args, void*) {
    if (args == "toggle") SetWanted(!g_want, "test verb");
    else if (args == "0" || args == "1") SetWanted(args == "1", "test verb");
    else return false;
    return true;
}
bool VerbInfo(std::string_view, void*) {
    HWND hwnd = static_cast<HWND>(melange::events::GameWindow());
    RECT win{}, c{}, mon{};
    if (hwnd) {
        GetWindowRect(hwnd, &win);
        GetClientRect(hwnd, &c);
        MonitorRects(hwnd, &mon, nullptr);
    }
    int vp[4] = {}, sw = 0, sh = 0;
    melange::mirage::engine::WindowViewport(vp);
    melange::mirage::engine::SceneSize(&sw, &sh);
    LOG_INFO("[display] want=%d on=%d exclusive=%d hotkey=%s window %dx%d at (%ld, %ld) client %dx%d monitor %dx%d at (%ld, %ld) "
             "viewport %d,%d %dx%d scene %dx%d syncPending=%d",
             g_want.load(), g_on, g_exclusive, g_hotkey.c_str(), logic::Width(win), logic::Height(win), win.left, win.top,
             logic::Width(c), logic::Height(c), logic::Width(mon), logic::Height(mon), mon.left, mon.top, vp[0], vp[1], vp[2],
             vp[3], sw, sh, g_syncPending);
    return true;
}

class Display final : public melange::Module {
public:
    const char* Name() const override { return "Display"; }
    const char* Description() const override {
        return "borderless fullscreen at the monitor's resolution (View > Fullscreen, Alt+Enter), remembered across starts";
    }
    bool RequiresKnownBuild() const override { return true; }  // the viewport and scene-target rebuild are engine fields
    int Order() const override { return 49; }                  // after MirageSupersample (48)

    bool Install() override {
        g_want = Bool("Fullscreen", false);
        melange::config::EnsureKey(Name(), "Hotkey", "Alt+RETURN");
        const std::string hk = melange::config::GetString(Name(), "Hotkey", "Alt+RETURN");
        std::string shortcut;
        if (!logic::HotkeyDisabled(hk)) {
            uint8_t dik = 0, mods = 0;
            if (!melange::render::ParseHotkeyText(hk.c_str(), &dik, &mods)) {
                LOG_WARN("[display] Hotkey=%s is not a valid hotkey, using Alt+RETURN", hk.c_str());
                melange::render::ParseHotkeyText("Alt+RETURN", &dik, &mods);
            }
            // The game has no Alt+Enter of its own (XomWndProc leaves WM_SYSKEYDOWN to DefWindowProc); as a hotkey
            // the key is kept from the game's DirectInput and from the window procedure.
            melange::overlay::AddHotkey(dik, mods, &ToggleAction, nullptr);
            g_hotkey = shortcut = melange::render::HotkeyLabel(dik, mods);
        }
        melange::overlay::AddToggleMenuItem("View/Fullscreen", &ToggleAction, nullptr, &Checked,
                                            shortcut.empty() ? nullptr : shortcut.c_str());
        melange::testcmd::Register("display.fullscreen", &VerbFullscreen);
        melange::testcmd::Register("display.info", &VerbInfo);
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        LOG_INFO("[display] ready (Fullscreen=%d Hotkey=%s)", g_want.load(), g_hotkey.c_str());
        return true;
    }
};
}  // namespace

MELANGE_MODULE(Display);
