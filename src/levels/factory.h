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
}  // namespace melange::levels::factory
