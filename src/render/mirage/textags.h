#pragma once
// Texture tags (MirageTexTags): enabled mods' spice.json "graphics": {"textureTags": "<file>"} tables map a digest of
// a texture's base level to 8 numbers, and a GLSL replacement that declares `uniform vec4 mg_texTag[2]` gets the tag
// of the texture bound on its texture0 unit with every draw. See textags_logic.h for the digest.
#include <cstdint>

namespace melange::mirage::textags {
// A table is loaded and the GL hooks are in. When false, ForDraw returns zeros at once.
bool Active();
// The tag (8 floats) for the texture bound on `unit` now, all zero when it is unknown, untagged or no table is loaded.
// Called from a replaced program's Upload() before each draw, GL context current; counts the draw for tags.stats.
const float* ForDraw(uint32_t unit);
}  // namespace melange::mirage::textags
