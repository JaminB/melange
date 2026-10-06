#pragma once
// MirageSupersample's internal entry for the Display module (borderless fullscreen). Usable whether or not the
// MirageSupersample module is installed: it only needs the engine checks.
namespace melange::mirage::supersample {
// Main thread, GL context current, between frames. Queues the engine's own scene-target rebuild (the pp+0x78
// switch) for a window of w x h; the caller has set GL_VIEWPORT to (0, 0, w, h) first, since the rebuild reads it.
// Lands on the supersampling Melange wants when it fits the GPU's largest texture at that size, else on the
// engine's current factors, else on 1x1. True when queued (or a rebuild is already pending); false when the engine
// switch is not recognised or the renderer is not up yet.
bool RebuildTargets(int w, int h);
}  // namespace melange::mirage::supersample
