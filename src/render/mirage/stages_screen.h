#pragma once
// The "screen" draw stage (Melange 0.12.0): mod drawing at present time, in window pixels, after the engine's
// Composite and every post effect, before ImGui; in the frontend, the lobby, loading screens and matches alike.
// Contract written by C0; C5 implements it in stages_screen.cpp (and the draw queue's Stage::Screen).
namespace melange::mirage {
// Called once per presented frame by the overlay's present hook, right after media::OnPresent() and before the mod
// pointer and ImGui, whether the overlay is shown or not. Main thread, GL context current, back buffer bound; runs
// wum.draw.on("screen") callbacks and the queued HUD primitives of that stage, then restores GL state.
void RunScreenStage();
}  // namespace melange::mirage
