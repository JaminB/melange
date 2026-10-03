#pragma once
#include <string>

namespace melange::launcher::setup {
// Melange's game-folder mutex is held (Melange is loaded in a running game).
bool MelangeLoaded(const std::wstring& dir);
// The game runs from `dir`: Melange's mutex, or any process whose image is <dir>\WormsMayhem.exe.
bool GameRunning(const std::wstring& dir);
}  // namespace melange::launcher::setup
