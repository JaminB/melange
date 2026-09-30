#pragma once
#include <optional>
#include <string>
#include <string_view>

#include "erg/scene.h"

// The generated level chunk (plain Lua 5.0 source). A pack's chunk is run only when its text is exactly what the
// generator writes for its stem.
namespace melange::erg::luagen {
constexpr size_t kMaxChunkBytes = 4096;

bool Needed(const Scene& s);                               // knot spawns, placed objects or a water level
std::string Chunk(const Scene& s);
std::string Text(std::string_view stem, bool knots, bool objects, std::optional<double> water);   // "" when nothing to do
std::string Stub(std::string_view stem);                   // the chunk of a Test level that needs none

// True when `text` is the generator's output for `stem` (any settings, or the stub). Bytecode is refused.
bool IsGenerated(std::string_view stem, std::string_view text, std::string* why);
}  // namespace melange::erg::luagen
