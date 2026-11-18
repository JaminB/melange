#pragma once
#include <cstddef>
#include <cstdint>

#include "lua/engine50.h"

// wum.sim.melee (Melange 0.16.0): sim-script control of a vanilla melee weapon (Bat, Fire Punch, Prod, No More Nails).
// - A fire gate: on a claimed weapon with dig = true, a fire press either goes to the vanilla swing ("strike") or is
//   swallowed and reported to Lua ("dig"), so a mod can turn the weapon into a tool that does not end the turn.
// - Events press, release, strike, hit and done; a hit can be cancelled or its damage/impulse overridden, or turned
//   into the engine's InstantKill, from inside the hit handler (wum.sim.melee.result).
// - strike(): start the vanilla swing from the sim (for example when a held dig reaches a worm).
// Everything that touches the game goes through Backend (the Melee module, src/gameplay/melee.cpp, or the self-test's
// fake). Determinism: the gate runs where the engine delivers the active worm's inputs (the same tick on every
// machine), events are dispatched from a sim tick hook, hits synchronously inside the engine's per-target call; the
// claims, the gate modes, the held state and the hit count are hashed as "melange.melee".
namespace melange::simmelee {
enum class Mode : uint8_t { Auto = 0, Dig = 1, Strike = 2 };
const char* ModeName(Mode m);

class Backend {
public:
    virtual ~Backend() = default;
    virtual int WeaponIndex(const char* name) = 0;   // the kNames index (1..0x44) of a melee weapon, -1 otherwise
    virtual int ActiveSlot() = 0;                    // the worm whose turn it is, -1
    virtual int WeaponOf(int slot) = 0;              // its selected weapon index, -1
    // A worm (not the attacker, not a crate, drum, mine or the land) was in the swing at the wielded entity's last
    // idle sweep (the engine's sweep, filtered to worm records for a claimed weapon).
    virtual bool WormInRange() = 0;
    virtual int EntityState() = 0;                   // the wielded melee entity's state (0 idle), -1 when none
    virtual const char* Strike() = 0;                // starts the vanilla swing; nullptr, or "not wielded" / "busy"
};
void SetBackend(Backend* b);

constexpr int kMaxClaims = 4;
struct ClaimInfo {
    char name[48];
    int index;     // kNames index
    int mod;       // claiming mod's index
    bool dig;      // the fire gate is on
    Mode mode;
};
int Claims(ClaimInfo* out, int max);
using ClaimFn = void (*)(int claims, void* user);    // after a claim (the engine module enables its hooks)
void SetClaimObserver(ClaimFn fn, void* user);

// Engine side (the module's hooks). OnFire returns true to swallow the input.
bool OnFire(int slot, bool pressed);
void OnStrike(int slot);                             // the swing reached its strike (state 2 left)
void OnDone(int slot);                               // the swing finished (back to idle)
struct HitResult {
    bool cancel = false;
    bool kill = false;
    bool setDamage = false, setImpulse = false;
    float damage = 0, impulse = 0;
};
// A claimed weapon struck a worm: runs the "hit" handlers synchronously and returns what they asked for. Not claimed
// (attacker's weapon): an empty result.
HitResult OnHit(int attacker, int victim, const float pos[3], float damage, float impulse);
// True while a "hit" handler runs (inside the engine's strike loop): engine-calling sim functions refuse ("in hit").
bool InHit();
bool Claimed(int weaponIndex);
void Tick();                                         // sim tick hook: held validation, then the queued events
void ResetMatch();

size_t HashBytes(uint8_t* out, size_t cap);
constexpr size_t kHashBytes = kMaxClaims * 4 + 7 + 4 + 2 + 1;
uint32_t HitCount();

// Called from sim_api.cpp's PushWum: the mod's wum.sim.melee table.
void PushTable(lua50::State* L, int mod);
}  // namespace melange::simmelee
