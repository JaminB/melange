#pragma once
#include <cstdint>
#include <string>
namespace melange::overlay {
// Everything here is called on the main thread inside the ImGui frame unless stated otherwise.
// Panels may call ImGui:: directly (#include <imgui.h>; one ImGui context, owned by the overlay).
using DrawFn = void (*)(void* user);
enum PanelFlags : uint32_t { kPanelNone = 0, kPanelOpenByDefault = 1 };
int AddPanel(const char* id, const char* title, DrawFn fn, void* user, uint32_t flags = kPanelNone);  // any thread
void RemovePanel(int handle);                                                                         // any thread
// Main-menu-bar item, e.g. "File/Save logs as...". `shortcut` is display text only.
using ActionFn = void (*)(void* user);
int AddMenuItem(const char* path, ActionFn fn, void* user, const char* shortcut = nullptr);  // any thread

// Hotkeys: matched on DirectInput key-down events (DIK codes, src/core/keys.h names) and swallowed so the game
// never sees them. Fire on the main thread whether or not the overlay is visible.
enum Mods : uint8_t { kNone = 0, kCtrl = 1, kShift = 2, kAlt = 4 };
int AddHotkey(uint8_t dik, uint8_t mods, ActionFn fn, void* user);  // any thread
bool ParseHotkey(const char* text, uint8_t* dik, uint8_t* mods);    // "Ctrl+Shift+F11"

bool Visible();
void SetVisible(bool v);     // showing the overlay turns capture on (see §3.A input rules)
bool Capturing();
void SetCapture(bool on);

struct GlInfo { std::string vendor, renderer, version, glsl; int viewportW = 0, viewportH = 0; bool valid = false; };
GlInfo Gl();  // captured on the first Present; any thread (copy)
struct Stats { double lastUs, p95Us; uint64_t frames; uint32_t stateMismatches, contextResets; };
Stats GetStats();
bool Installed();
}
