#pragma once
// The Controls module's interface for wum.input (src/lua/wum_input.cpp). Everything here is local input handling on
// the sending side: it changes the numbers the game puts into its own mouse messages before they are built.
#include <cstdint>
#include <string>
#include <vector>

#include "input/controls_logic.h"

namespace melange::controls {
bool Available();  // module installed (known build, [Controls] Enabled=1)
// Last caller wins. `owner` identifies the caller (a mod generation); ClearOptions only acts if it still owns them.
void SetOptions(const Options& o, const void* owner);
void ClearOptions(const void* owner);
void ClearAllOptions();
Options Effective();
bool SmoothMouse();  // [Controls] SmoothMouse, and Raw Input registered
// Names of the active control groups in index order. False when unavailable; an empty list when there is no service.
bool ActiveGroups(std::vector<std::string>* out);
// Short label of the first keyboard key mapped to an engine message, or false when unknown.
bool Binding(const char* messageName, std::string* label);
// The drive: hold the engine's own inputs (Input.MoveForward, Input.MoveBackward, Input.MoveLeft, Input.MoveRight,
// Input.Jump, Input.JumpBack, Input.Fire, Input.AimUp, Input.AimDown) for `ms` from now, as held keys post them
// (<name>Pressed, then <name>Released at the deadline), from the input service's tick, so the window's focus does not
// matter. `replace` releases every held input not in the list. Offline matches only. False with *why when refused.
bool Drive(const std::vector<std::string>& inputs, int ms, bool replace, std::string* why);
struct DriveHeld { std::string name; bool posted; int msLeft; };
struct DriveState { std::vector<DriveHeld> held; uint64_t posts = 0; std::string lastError; };
DriveState DriveNow();
}  // namespace melange::controls
