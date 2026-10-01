#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "erg/scene.h"

// The generated level chunk (plain Lua 5.0 source). A pack's chunk is run only when its text is exactly what the
// generator writes for its stem.
namespace melange::erg::luagen {
constexpr size_t kMaxChunkBytes = 32768;

// Everything the chunk encodes: knot spawns, PlaceObjects, the level objects (in order) and the water level.
struct ChunkSpec {
    bool knots = false;
    bool placeObjects = false;                             // mine, oildrum
    std::vector<ObjectSpec> objects;
    std::optional<double> water;
};
ChunkSpec SpecOf(const Scene& s);

bool Needed(const Scene& s);                               // knot spawns, placed objects, level objects or a water level
std::string Chunk(const Scene& s);
std::string Text(std::string_view stem, const ChunkSpec& spec);   // "" when nothing to do or an object is refused
std::string Text(std::string_view stem, bool knots, bool objects, std::optional<double> water);   // "" when nothing to do
std::string Stub(std::string_view stem);                   // the chunk of a Test level that needs none

// The pre-M6.2 form (the setup wrap at load time), kept only to recognise chunks built by earlier packs.
std::string LegacyText(std::string_view stem, const ChunkSpec& spec);

// True when `text` is the generator's output for `stem` (any settings, or the stub): the chunk's own literal lines are
// parsed, the chunk is generated again from them and the two must be byte-equal. Bytecode is refused.
bool IsGenerated(std::string_view stem, std::string_view text, std::string* why);
// Like IsGenerated, but a byte-exact legacy chunk is accepted too; *out is the chunk to run (always the current form).
bool Upgrade(std::string_view stem, std::string_view text, std::string* out, std::string* why);
bool Parse(std::string_view stem, std::string_view text, ChunkSpec* out);   // the literal lines only; no equality check
}  // namespace melange::erg::luagen
