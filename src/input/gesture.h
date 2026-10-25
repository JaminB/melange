#pragma once
// User gestures: a click that core delivered to a mod (melange.input.mouse down+up inside one of the mod's regions).
// wum.media.install treats a recent consent-grade gesture as the user's consent to download the media pack
// (Critique S10). Written complete for Melange 0.12.0; C2 (input) marks, C4b (wum.media) asks. Main thread only.
#include <cstdint>

namespace melange::input::gesture {
// A click on one of `modId`'s regions was delivered. consentGrade: the region covered at most 25% of the window and
// had been claimed for 500 ms or more before the button went down (input_regions decides).
void Mark(const char* modId, bool consentGrade);
// A gesture for `modId` happened within the last `ms` milliseconds (consent-grade only if `needConsentGrade`).
bool Recent(const char* modId, uint32_t ms, bool needConsentGrade = true);
// Forgets every gesture of `modId` (after an install consumed one, on unload).
void Clear(const char* modId);
}  // namespace melange::input::gesture
