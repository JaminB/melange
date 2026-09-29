#pragma once
#include "melange/weapons.h"

// wum.sim.weapons in the sim mods, and the Lua side of the clone behaviour events. The weapons layer is reached
// through Api so the sim bridge links without it (the plugin sets it at install; the self-test sets a fake).
namespace melange::simweapons {
struct Api {
    int (*declared)(weapons::CloneInfo* out, int max);
    int (*active)();                                    // k, -1 none
    weapons::QueueResult (*explode)(const float d[3]);
    bool (*allowed)(const char* modId);                 // the mod declares clones or depends on a mod that does
    const char* (*baseName)(int id);
};
void SetApi(const Api* api);  // nullptr: no clones (list() is empty, on() refused)

const char* EventName(weapons::Event e);                // "fire", "tick", "impact", "explosion"
// Raises sim.weapon.<event> in the sim mods with (clone name, tick, ...): tick adds x, y, z when hasPos, impact the
// message name, explosion x, y, z.
void Dispatch(const weapons::EventArgs& a, const char* cloneName, const char* msgName);
}  // namespace melange::simweapons
