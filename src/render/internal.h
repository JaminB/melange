#pragma once
// Internal interfaces between the overlay's translation units (not part of the public SDK).
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace melange::render {
// ---- input.cpp
// IAT hooks on WormsMayhem.exe: DINPUT8!DirectInput8Create (keyboard filter chained after Automation) and
// USER32!SetCursorPos (no-op while capturing). Called once from Overlay::Install.
bool InstallInput();
bool InputInstalled();
// Hotkey actions are queued by the DirectInput poll and run from the Present hook (main thread).
bool HotkeysPending();
void RunPendingHotkeys();
// (Re)subclasses the game window; a no-op when `hwnd` is already subclassed.
void SubclassGameWindow(HWND hwnd);
// Set by overlay.cpp while the ImGui context and backends are ready to receive window messages.
void SetImGuiInputReady(bool ready);

struct InputStats {
    uint64_t keysDropped = 0, syntheticReleases = 0, mouseMsgsDropped = 0, keyMsgsDropped = 0, cursorPosBlocked = 0,
             hotkeysFired = 0, diPolls = 0;
    bool diHooked = false, cursorHooked = false, subclassed = false;
};
InputStats GetInputStats();
struct HotkeyInfo {
    int handle;
    std::string label;
};
std::vector<HotkeyInfo> ListHotkeys();

// ---- panels_builtin.cpp
void RegisterBuiltinPanels(bool demo);
void DrawBuiltinExtras();  // inside the ImGui frame (the demo window)

// ---- overlay.cpp
// First-use position/size of a panel window (otherwise panels cascade). imgui.ini wins once it has an entry.
void SetPanelDefaultRect(int handle, float x, float y, float w, float h);
std::string HotkeyText(int which);  // 0 = toggle key, 1 = pass-through key (display text)
double Fps();                       // engine frames per second over the last second
bool VerifyStateOn();
}  // namespace melange::render
