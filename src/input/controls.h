#pragma once
// The Controls module's interface for wum.input (src/lua/wum_input.cpp). Everything here is local input handling on
// the sending side: it changes the numbers the game puts into its own mouse messages before they are built.
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
}  // namespace melange::controls
