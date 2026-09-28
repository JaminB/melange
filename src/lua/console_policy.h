#pragma once
#include <string>

// The console's rule for the match target, shared with Oasis's lua.eval so both refuse the same things.
namespace melange::console {
// In a match, engine Lua available on this build, and not online unless [LuaConsole] MatchConsoleOnline=1.
// Main thread. False with the console's reason otherwise.
bool MatchAllowed(std::string* reason);
}  // namespace melange::console
