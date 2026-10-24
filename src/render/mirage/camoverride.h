#pragma once
// MirageCamera's link to render::GetCamera (stages.cpp): the override's matrices outside the engine's Render call,
// and the game's own matrices inside it.
namespace melange::mirage::camoverride {
// Outside Render, with an override set: replaces view, invView and proj (float[16] each) with the override's.
// Inside Render the engine camera already holds the override, so this does nothing there.
void Adjust(float view[16], float invView[16], float proj[16]);
// Inside Render, while the override is applied: replaces the three with the game's own (saved) matrices.
void GameView(float view[16], float invView[16], float proj[16]);
}  // namespace melange::mirage::camoverride
