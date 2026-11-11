#pragma once
// MirageCamera's link to render::GetCamera (stages.cpp): the override's matrices outside the engine's Render call,
// and the game's own matrices inside it.
namespace melange::mirage::camoverride {
// Outside Render, with an override set: replaces view, invView and proj (float[16] each) with the override's.
// Inside Render the engine camera already holds the override, so this does nothing there.
void Adjust(float view[16], float invView[16], float proj[16]);
// Inside Render, while the override is applied: replaces the three with the game's own (saved) matrices.
void GameView(float view[16], float invView[16], float proj[16]);
// A render-only correction drawn while no override is set (LevelCamera): given the game's eye, target and up (world),
// it fills new ones and returns true to draw this frame from them. Called on the main thread inside the engine's
// Render, once a frame, in a match. Setting one installs the Render hook; null removes it.
using AutoFn = bool (*)(const float eye[3], const float target[3], const float up[3], float outEye[3], float outTarget[3]);
void SetAuto(AutoFn fn);
}  // namespace melange::mirage::camoverride
