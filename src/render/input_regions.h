#pragma once
// Mod input (Melange 0.12.0): claimed regions, match pointer mode, keyboard focus and paste for wum.input.
// The state and the routing live in input_regions_core.cpp (game-free, linked by the self-tests) on top of the pure
// ModInput in input_logic.h. input_regions.cpp is the game's Host (cursors, window, match and turn signals, the
// clipboard) and draws the pointer; input.cpp feeds it window messages and DirectInput records and owns the warp hook;
// fecursor.cpp reads the frontend cursor. Everything runs on the main thread: the window procedure, the DirectInput
// poll, Lua and the present hook all do.
#include <cstdint>
#include <string>
#include <vector>

namespace melange::render {
// Draws the pointer-mode cursor (the owner's texture or the default arrow) when pointer mode is on. Called by the
// overlay's present hook right after mirage::RunScreenStage(), before ImGui. Main thread, GL context current.
void DrawModPointer();

// input.cpp, for the pointer mode (input_regions.cpp's Host).
bool InstallWarpHook();                             // the per-frame cursor warp slot (shared with MouseFix)
bool PointerExit(int* centreX, int* centreY);       // zero-delta exit; the warp centre in client px
void PostButtonUps(const std::vector<int>& buttons);  // a release message for each button (kBtn* of input_logic.h)

// fecursor.cpp: the frontend (menu, lobby) cursor in window px. False when no MouseEntity exists or the build differs.
bool FrontendCursor(int winW, int winH, float* x, float* y);
void RegisterFrontendCursorVerb();  // test verb input.fecursor: logs the raw and mapped position
// input_regions.cpp: sets the Host and subscribes the per-frame tick. Called once by InstallInput.
void InstallModInput();
}  // namespace melange::render

namespace melange::render::modinput {
enum class Result : uint8_t { Ok, Busy, Unavailable };
enum class Shape : uint8_t { Arrow, Hand, IBeam };  // region opts.cursor: the pointer-mode cursor over the region

// ---- wum.input (wum_input.cpp). `mod` is the calling mod's id, `order` its load order (a higher order wins).
// x, y, source ("frontend" | "pointer"); false = no cursor for this mod (a match without its pointer mode).
bool Cursor(const std::string& mod, float* x, float* y, const char** source);
Result Region(const std::string& mod, int order, const std::string& id, float x0, float y0, float x1, float y1,
              int frames, Shape shape);
std::string Hover(const std::string& mod);  // "" = none
// texture: a wum.draw.texture id of this mod (checked by the caller), 0 = the default cursor.
Result Pointer(const std::string& mod, bool on, bool clickOutside, unsigned texture);
bool PointerActive(const std::string& mod);
Result Focus(const std::string& mod, bool on);
// The mod's generation ended (unload, reload, disable): its regions, pointer mode, focus and gestures go.
void Forget(const std::string& mod);

// ---- core (input.cpp, controls.cpp)
bool OwnsMouse();          // pointer mode is on: no warp, no SetCursorPos, no mouse messages or raw input for the game
bool OwnsKeyboard();       // a mod has keyboard focus: DirectInput and key messages are kept from the game
bool SuppressBareFKeys();  // focus is held: bare F1..F12 hotkeys do not fire (modifier hotkeys still do)
// A bare Esc press must be eaten from DirectInput: pointer mode is on, or Esc just ended pointer mode or focus.
bool EatEsc();
void EscFromDirectInput();  // the DirectInput poll ate a bare Esc press (see EatEsc)
// Window messages; true = pass the message on to the game. Never called for messages the overlay captures.
bool RouteMouse(unsigned msg, uintptr_t wp, intptr_t lp);
bool RouteKey(unsigned msg, uintptr_t wp, intptr_t lp);
void NoteSwallowedByOverlay(unsigned msg, uintptr_t wp);  // a mouse message the overlay kept from the game
void FocusLost();                                          // WM_KILLFOCUS / WM_ACTIVATEAPP(FALSE)
void Tick();                                               // once per frame (Event::Frame)

// The game side, supplied once by input_regions.cpp (or a self-test). Every member must be set.
struct Host {
    bool (*allowed)();            // [Input] AllowModInput and the window procedure is hooked
    bool (*overlayCapturing)();
    bool (*foreground)();         // the game window (or another window of this process) is the foreground one
    bool (*inMatch)();
    uint64_t (*frame)();          // presented-frame serial
    uint64_t (*nowMs)();
    void (*windowSize)(int* w, int* h);
    bool (*feCursor)(float* x, float* y);  // the frontend cursor in window px; false = no frontend cursor
    bool (*osCursor)(float* x, float* y);  // the OS cursor in window px, clamped to the client area (pointer mode)
    uint8_t (*modifiers)();                // kModCtrl | kModShift | kModAlt held now
    bool (*clipboard)(std::wstring* text);  // CF_UNICODETEXT, at most kPasteMaxUnits UTF-16 units
    bool (*pointerHook)();                  // install what pointer mode needs; false = unavailable
    void (*pointerStarted)(const std::vector<int>& heldButtons);  // release each button the game holds
    bool (*pointerEnded)(int* centreX, int* centreY);             // zero-delta exit (PointerExit)
    bool (*myTurnStarted)(bool reset);  // a turn of this machine's player started since the last call
    void (*post)(const std::string& mod, const char* name, const std::string& json);  // owner-only event
};
void SetHost(const Host& host);
constexpr size_t kPasteMaxUnits = 8192;

// For DrawModPointer and the self-tests.
struct PointerView {
    bool on = false;
    std::string owner;
    unsigned texture = 0;
    Shape shape = Shape::Arrow;  // of the owner's region under the cursor
    float x = 0, y = 0;
};
PointerView CurrentPointer();
std::string FocusOwner();
}  // namespace melange::render::modinput
