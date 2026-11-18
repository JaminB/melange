#pragma once
#include <cstddef>
#include <cstdint>

#include "lua/engine50.h"

// wum.sim.carve / wum.sim.carveLine: deterministic terrain removal for sim scripts (Melange 0.16.0, the Grave Spade).
// Everything that touches the game goes through Backend, so the offline self-test drives this file against a fake;
// the LandCarve module (src/gameplay/land_carve.cpp) supplies the engine one: a direct call of the landscape's own
// Explosion handler with a zero-damage, zero-impulse message that never goes on the bus.
//
// Determinism: the carve runs inside the sim tick (only sim scripts can call it), at the same tick on every machine,
// with float32 arguments; the land is the result of the same history everywhere. Every carve folds its tick, the
// argument bits and the removed count into a digest that the "melange.land" Wormsign contributor hashes each tick.
// The budget is compile-time (kCallsPerTick, kSpheresPerTurn), never read from a player's ini, so every peer takes
// the same branch; the contributor also hashes the limits and whether the engine backend is live.
namespace melange::simland {
struct CarveOut {
    int removed = 0;          // voxels removed (+1 when only the heightmap changed)
    const char* why = nullptr;  // set when refused: "indestructible", "unavailable"
};
class Backend {
public:
    virtual ~Backend() = default;
    // Removes the destructible voxels whose centres are within `radius` of `pos` (world units). Never touches water.
    virtual bool Carve(const float pos[3], float radius, CarveOut* out) = 0;
};
void SetBackend(Backend* b);  // nullptr: every call answers "unavailable"

// The budget: carve and carveLine calls per sim tick (a stroke is one call), spheres per turn (a stroke counts each).
constexpr int kCallsPerTick = 4;
constexpr int kSpheresPerTurn = 900;
struct Limits {
    int perTick = kCallsPerTick;
    int perTurn = kSpheresPerTurn;
    bool log = false;         // [LandCarve] Log (logging only: never changes what a carve does)
};
// The game always runs the constants; only the self-test lowers them (and sets log from the ini, which is harmless).
void SetLimits(const Limits& l);
constexpr float kMinRadius = 1.f, kMaxRadius = 200.f;
constexpr int kMaxLineSpheres = 16;

void ResetMatch();            // match VM created or closed
// The digest of every carve of this match and the carve count, for the Wormsign contributor.
size_t HashBytes(uint8_t* out, size_t cap);
constexpr size_t kHashBytes = 8 + 4 + 4;
struct Stats { uint32_t carves, refused, removed; };
Stats GetStats();

// The engine-facing carve, with the budget and the digest (the Lua functions call it). Returns removed, or -1 with
// *why set ("budget", "bad arguments", "not in a match", "in hit", "indestructible", "unavailable").
int Carve(float x, float y, float z, float radius, const char** why);

int __cdecl LCarve(lua50::State* L);
int __cdecl LCarveLine(lua50::State* L);
bool AddSimFunctions();       // registers carve and carveLine with the sim bridge; install time
}  // namespace melange::simland
