#pragma once
// What other Mirage modules read of MirageTextures' effective setting.
namespace melange::mirage::textures {
// The anisotropy to give a texture now: the effective setting clamped to the driver's maximum, or 1 (off). Needs a
// current GL context.
float EffectiveAnisotropy();
}  // namespace melange::mirage::textures
