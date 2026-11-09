#pragma once
// Scenery tints (module MirageTints): a scenery detail whose name carries "_tint<rrggbb>" is drawn recoloured toward
// that colour by GLSL replacements that declare `uniform vec4 mg_tint` (Sunstone's model shaders). The table is the
// tinted details of the level being played, read from its .xan when the match's level changes (tints_logic.h has the
// tag and the matching). Render state only: the simulation never reads it.
#include <cstdint>

namespace melange::mirage::tints {
// The mg_tint value for a draw whose vertex program's `model` and `view` registers are these (16 floats each, Cg rows,
// row vectors; nullptr when the program has none): the object's world matrix is model * view * the main camera's inverse
// view (the engine sends the object's transform in either), matched to the table. The detail's colour with a = 1, or
// (1, 1, 1, 0). Main thread, GL context current.
const float* ForDraw(const float* model, const float* view);
// The landscape tint ("_ltint" tags, tints_logic.h) of the level being played for a texture of this digest (textags.h):
// the colour with a = 1, or (1, 1, 1, 0). 0 (unknown texture) is never tinted.
const float* ForLandTexture(uint64_t digest);
// True while the level being played has tinted details (Upload can skip the lookup when false).
bool Any();
}  // namespace melange::mirage::tints
