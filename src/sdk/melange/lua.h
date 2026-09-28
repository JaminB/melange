#pragma once
#include <cstdint>
struct lua_State;  // Lua 5.4.7, the Sandbox VM (include <lua.hpp> from the fetched Lua for the C API)
namespace melange::lua {
bool Ready();
lua_State* ClientState();                     // null before Sandbox starts
using OpenFn = int (*)(lua_State* L);         // pushes one table
enum LibFlags : uint32_t { kLibNone = 0, kLibDeepDesert = 1 };
// Adds wum.<name> to every mod environment created after the call (and to the console). Install time or main thread.
bool AddLibrary(const char* name, OpenFn fn, uint32_t flags = kLibNone);
const char* CurrentMod();                     // id of the mod whose code is running, or nullptr
// Client events (wum.events): delivered to Lua subscribers at the next Frame event, in post order.
bool PostEvent(const char* name, const char* jsonObject);  // any thread
struct Stats { uint32_t mods, callbacks, faults, disabledCallbacks; uint64_t instructions, bytes; double msLastFrame; };
Stats GetStats();
}
