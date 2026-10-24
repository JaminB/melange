#pragma once
#include <cstddef>
#include <cstdint>

#include "lua/engine50.h"

// wum.sim.worm(slot), wum.sim.worms() and wum.sim.land() for sim scripts, and the worm anchor behind worm:anchor().
// Everything that touches the game goes through Backend, so the offline self-test drives this file against a fake;
// the WormAnchor module (src/gameplay/worm_anchor.cpp) supplies the engine one and the hooks that call Repin.
//
// Determinism: every value read here is simulation state (the worm data containers), and the anchor set is only
// changed by sim-script calls, which every machine makes at the same tick. Nothing here depends on the camera, the
// frame rate or the local player.
namespace melange::simworms {
constexpr int kMaxWorms = 16;

// The engine's worm physics states (WXWormLogicEntity, container +0xf0; names from the kWPS_ table at 0x90d374).
enum State : uint32_t {
    kAmbulatory = 0, kDetectJump = 1, kBallistic = 2, kSliding = 3, kVaulting = 4, kOverride = 5, kPassive = 6,
    kDeathThroes = 7, kDrownFloat = 8
};
// WormDataContainer +0xec flag bits the engine's own "nailed" worms carry (No More Nails): 0x10 refuses the
// Ballistic/Sliding transitions and the explosion impulse, 0x20 makes the movement input a no-op.
constexpr uint32_t kNailFlags = 0x30;

struct WormInfo {
    bool active;
    uint32_t state;       // State
    uint32_t flags;       // container +0xec
    uint16_t health;
    uint8_t team, posInTeam;
    int32_t weapon;       // selected weapon index (kNames at 0x90c920), or -1
    float pos[3], vel[3]; // feet position, velocity (world units per ms)
    float yaw;
    char name[32];
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual bool Read(int slot, WormInfo* out) = 0;      // false: no container in that slot
    // Writes the container's position and velocity, and ORs `setFlags` into / ANDs `~clearFlags` from +0xec.
    virtual bool Write(int slot, const float pos[3], const float vel[3], uint32_t setFlags, uint32_t clearFlags) = 0;
    virtual bool Land(float out[6]) = 0;                 // Land.MinBounds then Land.MaxBounds
};
void SetBackend(Backend* b);  // nullptr: every call answers "unavailable"
Backend* GetBackend();
// Called after an anchor is set or cleared (on) and when a pin is released (on = false, released = true).
using AnchorFn = void (*)(int slot, bool on, bool released, void* user);
void SetAnchorObserver(AnchorFn fn, void* user);

// The anchor set of this match. Anchor() captures the pin from the worm's current position.
bool Anchor(int slot, bool on, const char** why);        // why: "no worm", "unavailable", "dead"
bool Anchored(int slot);
bool Pinned(int slot);                                   // anchored and still being pinned (alive, not drowning)
bool PinOf(int slot, float out[3]);
// Re-pins one anchored worm: writes the pin and a zero velocity and re-asserts the nail flags, unless the worm is
// dying or drowning (states 7 and 8), in which case the pin is released for the rest of the match so the engine can
// sink it. The engine hooks call this before and after the worm's own Update, and the sim tick hook calls RepinAll.
void Repin(int slot);
void RepinAll();
void ResetMatch();                                       // forgets every anchor (match VM closed)
int AnchoredCount();
// The anchor set as raw bytes for a Wormsign contributor (anchored/pinned flags and the pins); returns the count.
size_t HashBytes(uint8_t* out, size_t cap);
constexpr size_t kHashBytes = kMaxWorms * (2 + 12);

bool Alive(const WormInfo& w);                           // active, health > 0, not DeathThroes/DrownFloat

// The wum.sim functions (Lua 5.0 trampolines; upvalue 1 = the mod index, unused).
int __cdecl LWorm(lua50::State* L);
int __cdecl LWorms(lua50::State* L);
int __cdecl LLand(lua50::State* L);
bool AddSimFunctions();                                  // registers the three with the sim bridge; install time
}  // namespace melange::simworms
