#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lua/engine50.h"
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

// Added by C (additive; nothing above changed).
void RemoveOnMatch(int handle);
// M5 seam: raises `event` (a name starting with "sim.") in every sim mod that subscribed with wum.events.on, in mod
// load order, then registration order. Subscribers get (event, args...). Deterministic callers only.
void Dispatch(const char* event, const std::vector<float>& args);
// wum.sim.<name> = a C closure of `fn` with upvalue 1 = the mod's index (read it with lua50::A().tonumber(L,
// lua50::kGlobals - 1)), in every mod environment built after the call. `fn` follows the 5.0 rules: no live C++
// destructors when it raises. Install time only.
bool AddSimFunction(const char* name, lua50::CFunction fn);
const char* ModIdAt(int modIndex);    // nullptr when out of range or no match
const char* CurrentMod();             // id of the sim mod whose code is running, or nullptr
bool InTopLevelChunk();               // the running mod code is its entry.sim chunk at Init
std::vector<std::string> LoadedMods();                        // this match, load order
bool PushModEnv(const char* id);                              // pushes that mod's environment on the match VM's stack
std::vector<std::pair<std::string, uint16_t>> ModMessages();  // registered mod message names and ids, in order

// Added for weapon clones (additive).
using InitFn = void (*)(void* user);                    // at Init, after the gate allowed sim mods, before the first chunk
int OnBeforeModsLoad(InitFn fn, void* user);            // clones are created here, so top-level chunks can set their fields
void RemoveOnBeforeModsLoad(int handle);
struct Arg { enum { Num, Str } kind; float num; const char* str; };
void DispatchArgs(const char* event, const Arg* args, int n);   // Dispatch with string arguments
}
