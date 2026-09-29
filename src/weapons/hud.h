#pragma once

// The HUD weapon icon (0x5d7f8b): while the active worm holds a clone with a hudIcon, that file name is used.
namespace melange::weapons::hud {
bool Create();  // no-op (true) when no clone declares a hudIcon
bool Enable(bool on);
}  // namespace melange::weapons::hud
