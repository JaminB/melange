#pragma once

// spawns.mode areas in the game: the global ergSpawnDeal(spec, teams) in every match VM, which the level's generated
// chunk calls in its setup to learn each team's spawn area (erg/luagen.h DealAreas over the match's logic seed, so every
// peer and a replay deal alike).
namespace melange::levels::spawndeal {
void OnFrame();                                       // installs the match-VM observer once (main thread)
}  // namespace melange::levels::spawndeal
