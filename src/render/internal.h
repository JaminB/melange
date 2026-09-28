#pragma once
// Internal interfaces between the overlay's translation units.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace melange::render {
// input.cpp
// IAT hooks: DINPUT8!DirectInput8Create (keyboard filter) and USER32!SetCursorPos (no-op while capturing).
bool InstallInput();
bool InputInstalled();
// Hotkey actions are queued by the DirectInput poll and run from the Present hook.
bool HotkeysPending();
void RunPendingHotkeys();
// (Re)subclasses the game window; a no-op when `hwnd` is already subclassed.
void SubclassGameWindow(HWND hwnd);
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

// panels_builtin.cpp
void RegisterBuiltinPanels(bool demo);
void DrawBuiltinExtras();

// overlay.cpp
// First-use rect of a panel window; imgui.ini wins once it has an entry.
void SetPanelDefaultRect(int handle, float x, float y, float w, float h);
std::string HotkeyText(int which);  // 0 = toggle key, 1 = pass-through key
double Fps();
bool VerifyStateOn();
}  // namespace melange::render
