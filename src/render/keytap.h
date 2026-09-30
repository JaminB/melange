#pragma once
#include <cstdint>

namespace melange::render {
// Feeds one press and release of a DirectInput key to the game's keyboard; false when the keyboard is not hooked or a
// tap is still under way. Main thread or any thread.
bool TapKey(uint8_t dik, int holdMs = 80);
}  // namespace melange::render
