#pragma once
#include <string>

// Melange.exe's level-script syntax check: the script is compiled, never run, by a stock Lua 5.0.1 parser (lua50_ref),
// the dialect of the game's match VM, so `level.script.put` checks the syntax with the game closed as it does in the
// game. Not linked into melange.asi, which uses the engine's own compiler.
namespace melange::oasis::standalone {
// False on a syntax error, with its 1-based line (0 when the message names none) and the message without the
// "script.lua:<line>:" prefix. A script that cannot be checked (out of memory) passes: the game checks it on Test.
bool CompileLua50(const std::string& text, int* line, std::string* message);
}  // namespace melange::oasis::standalone
