#pragma once
#include <string>

#include "erg/scene.h"

// The level's mine factory in the match (build #1077). registry arms it at level start with the level's settings;
// the factory's own set-up then applies them (kMineFactoryData, written again just before the set-up reads it, so
// nothing between level start and the factory's placement can leave the game's values in force), and with keepKnot a
// drop that hits no land leaves the factory at its knot instead of the stale point of the match's last sweep. The
// last set-up is reported to level.test.applied. wum.level.mine and wum.level.fireFactory call the engine from here.
namespace melange::levels::factory {
void Arm(const std::string& stem, const erg::FactorySpec& f);   // at level start; a level without one disarms
void Disarm();
std::string ReportJson();   // the last set-up as JSON ("null" before any): asked, found, applied, the drop
// A fire on demand (sim tick, main thread): the idle countdown to 0 and the factory's own GameLogic.StartMineFactory
// handler called, so it fires as at a turn start when fewer mines than its target are in the level. count (1-10, 0 for
// the target's own rule) raises the target to the mines in the level plus count for that call alone. nullptr, or why
// it did not start a fire; *detail says what it found.
const char* FireNow(int count, std::string* detail);
// The fires seen (each PlaceMines: its mines, where each appeared and where it came to rest, levels/factory.fired on
// the session log) and the last on-demand trigger, as JSON {fires, trigger}.
std::string FiresJson();
// The fires seen this session (each PlaceMines) and, in *at, when the last was (GetTickCount64): a shot after a fire.
uint32_t FireSerial(unsigned long long* at);
// The last fire's first mine, for a camera that follows it in flight (render.fireShot {track}): where it appeared (world
// units), its upward speed then (world units per ms: the launchers' eruption, or -0.3 for a mine dropped from over the
// factory), the gravity it falls at, the fire's serial and when it was (GetTickCount64). false before any fire whose
// mines were seen.
struct Flight {
    float from[3] = {};
    float vy = 0, gravity = 0;
    uint32_t serial = 0;
    unsigned long long at = 0;
};
bool LastFlight(Flight* out);
// A mine at pos moving at vel (world units, per ms), made by the game's own mine maker as the factory's are; sim tick
// (main thread). nullptr, or why not.
const char* Mine(const float pos[3], const float vel[3]);
// Where the match's mine factory is now (world units), main thread; false when it has none.
bool Position(float out[3]);
}  // namespace melange::levels::factory
