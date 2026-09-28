#pragma once
#include <cstdint>
namespace melange::sim {
bool InMatch();              // a match lua_State exists
uint32_t MatchSerial();      // +1 per match VM
uint32_t Tick();             // Update calls since this match VM was created
bool ModsActive();           // sim mods are loaded in this match (== mods::SimAllowedThisMatch())
using TickFn = void (*)(uint32_t tick, void* user);
int AddTickHook(TickFn fn, void* user, int order = 0);  // after the engine's Update; deterministic code only
void RemoveTickHook(int handle);
// Deterministic Melange stream, independent of the engine's RNGs: seeded from the logic seed + key.
uint32_t Random(uint32_t streamKey);
enum class SendResult : uint8_t { Ok, NotRegistered, Denied, NotInMatch, Halted, BadArgs };
SendResult Send(const char* name);
SendResult SendInt(const char* name, int32_t v);
SendResult SendFloat(const char* name, float v);
SendResult SendString(const char* name, const char* v);
// Mod message names. Thumper calls these at start-up, in canonical order; names are copied into a
// never-freed arena (the registry keeps the pointer).
bool RegisterModMessage(const char* name, uint16_t* idOut);
void FreezeModMessages();    // after this, RegisterModMessage fails
struct Stats { uint32_t ticks, simMods, faults; double usLastTick, usP95Tick; uint32_t heapKB; };
Stats GetStats();
}
