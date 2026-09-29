#pragma once
#include <cstdint>
// Taps on the RNG draw wrappers 0x68c015/0x68c024 (logic) and 0x68c07b/0x68c0aa (second), and the seed setters
// 0x68c053 (logic) / 0x68c0b9 (second). Record-only unless the player owns the forced slots.
namespace melange::wormsign::rngtap {
bool Install();
// rng: 0 logic, 1 second. Return true to replace the draw: the owner has set the RNG state and writes the
// result bits (uint32 or float bits) to *bits.
using ForcedDrawFn = bool (*)(int rng, uint32_t ret, uint32_t* bits);
// kind: 0 logic, 1 second. Return true to replace the seed value with *value.
using ForcedSeedFn = bool (*)(int kind, uint32_t caller, uint32_t* value);
void SetForcedDraw(ForcedDrawFn fn);      // one owner (the player) while armed; null otherwise
void SetForcedSeed(ForcedSeedFn fn);
}  // namespace melange::wormsign::rngtap
