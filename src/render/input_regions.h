#pragma once
// Mod input (Melange 0.12.0): claimed regions, match pointer mode, keyboard focus and paste for wum.input.
// Contract written by C0; C2 owns this header and implements it (input_regions.cpp, input_logic.h, fecursor.cpp, the
// hooks in input.cpp and controls.cpp). Only DrawModPointer is called from outside C2's files.
namespace melange::render {
// Draws the pointer-mode cursor (the owner's texture or the default arrow) when pointer mode is on. Called by the
// overlay's present hook right after mirage::RunScreenStage(), before ImGui. Main thread, GL context current.
void DrawModPointer();
}  // namespace melange::render
