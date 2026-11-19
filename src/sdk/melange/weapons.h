#pragma once
#include <cstddef>
#include <cstdint>
namespace melange::weapons {
constexpr int32_t kVidBase = 0x100;          // virtual id = kVidBase + k; never in 0..0x44
constexpr int kFreeCellCount = 3;
constexpr int kFreeCells[kFreeCellCount] = {29, 39, 40};  // the panel's empty cells
// A vanilla cell a clone may take over, only when its manifest entry says so ("cell": 41, "replaces": "kUtilityBinoculars").
// While such a clone is live the item is off the panel and a selection of it is refused (registry_core.cpp).
struct TakeableCell { int cell; int32_t id; const char* weapon; };
constexpr TakeableCell kTakeableCells[] = {{41, 43, "kUtilityBinoculars"}};
constexpr int kTakeableCount = static_cast<int>(sizeof kTakeableCells / sizeof kTakeableCells[0]);
constexpr int kMaxClones = kFreeCellCount + kTakeableCount;  // 0.16: the free cells and the takeable ones

struct CloneInfo {
    uint16_t k; int32_t vid, base;           // base: WeaponNameEnum id
    char name[48];                           // resource name, "kWeaponMegaBazooka"
    char mod[64];                            // owning mod id
    int8_t cell; uint32_t iconCode;          // iconCode 0 = the base's icon
    bool live;                               // created in this match
    uintptr_t container, descriptor;         // 0 unless live
};
bool Enabled();                              // [Weapons] Enabled and every site check passed
int Declared(CloneInfo* out, int max);       // from enabled content mods, frozen per launch, k order; returns the total
bool Live();                                 // this match has clones (sim mods allowed and every declared clone created)
int ActiveClone();                           // k held by the active worm this turn, -1 none (sim state)
// The clone the worm in `slot` wields (its selected weapon is `weaponId`, a base id): the active worm's ActiveClone() when
// that clone's base is weaponId, else nullptr. The returned name lives as long as the registry.
const char* WieldedCloneName(int slot, int32_t weaponId, int activeSlot);
bool IsVid(int32_t v);
int32_t BaseOf(int32_t v);                   // a vid's base id (defensive: an unknown vid maps to kWeaponBazooka, logged)

// Behaviour events: main thread, logic clock, deterministic observers only (no RNG draws, no posts).
enum class Event : uint8_t { Fire, Tick, Impact, Explosion };
struct EventArgs {
    Event ev; uint16_t k; uint32_t tick;     // tick = sim::Tick()
    uintptr_t entity;                        // weapon logic entity (Fire) or payload (others)
    float pos[3]; bool hasPos;               // Explosion: CreateExplosion's position; Tick: no position yet
    uint16_t msgId;                          // Impact: the Payload.* message id
};
using EventFn = void (*)(const EventArgs& a, void* user);
int On(EventFn fn, void* user, int order = 0);
void RemoveOn(int handle);
// During an Explosion event only: one more explosion at pos + d, run after the original in the same tick.
enum class QueueResult : uint8_t { Ok, NotInExplosion, Full, OutOfRange };
QueueResult QueueExplosion(const float d[3]); // |d| <= 2000 per axis, <= [Weapons] ExtraPerExplosion (8)

// Weapon containers by schema field name (offsets from the engine's class field list).
enum class FieldType : uint8_t { None, F32, I32, U32, U16, U8, Bool, String };
uintptr_t Container(const char* resourceName);                  // 0 if absent
FieldType Field(uintptr_t container, const char* field, uint32_t* offset);
}
