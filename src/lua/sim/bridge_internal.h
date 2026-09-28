#pragma once
#include <string>
#include <vector>

#include "lua/sandbox_internal.h"

// What Thumper, the console, the handshake and data tweaks call on the sim bridge (match VM).
namespace melange::simbridge {
void SetModList(const std::vector<std::string>& idsInLoadOrder);  // content mods with entry.sim, applied at next match
using Gate = bool (*)();                                          // may sim mods run in this match?
void SetGate(Gate g);
sandbox::EvalOut EvalMatch(const std::string& code);              // console; our loadbuffer + pcall, never 0x6994c0
void CompleteMatch(const std::string& prefix, std::vector<std::string>* out);
using MatchFn = void (*)(bool begin, void* user);                 // after sim mods load / before lua_close
int OnMatch(MatchFn fn, void* user);
}
