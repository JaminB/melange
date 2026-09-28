#pragma once
#include <string>

// Pure text helpers for the console REPL (LuaConsole): no Lua, no game state, offline-testable.
namespace melange::console {
// "=expr" becomes "return expr" (leading/trailing whitespace trimmed); anything else passes through unchanged.
std::string ExpandShorthand(const std::string& code);

// The identifier chain (letters, digits, '_', '.', ':') ending exactly at `cursor`, or "" if the
// character right before the cursor cannot start one.
std::string CompletionWord(const std::string& text, size_t cursor);

// Replaces the segment of `word` after its last '.' or ':' with `candidate`; with no separator,
// `candidate` replaces `word` entirely. Used to splice a chosen completion back into the input.
std::string SpliceCompletion(const std::string& word, const std::string& candidate);
}  // namespace melange::console
