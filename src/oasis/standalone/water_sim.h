#pragma once
#include <string>

// The water a level script makes, turn by turn, for erg-mcp's check_flood and place_spawn_knots (Oasis
// `level.script.water`, Melange.exe only). The script runs in a stock Lua 5.0.1 (lua50_ref, the game's dialect) with a
// stand-in for the match's wum table: wum.sim.waterSchedule stepped as the game steps it (lua/sim/sim_events.cpp),
// setData/getData of Water.Level and Water.RiseSpeed.Current, after/every/cancel timers in ticks (50 a second),
// wum.events.on("sim.turnStarted") with the turn and the round, turn/tick/round/random; every other wum function does
// nothing and answers true (counted in `calls`), and engine data other than the water reads as nil. Turn n starts at
// tick 1 + (n - 1) * ticksPerTurn: the schedule steps, then the turnStarted handlers run, then the turn's timers;
// Water.RiseSpeed.Current adds at each turn's end, as the game's does. A stand-in, not the game: a script that reads
// worms, the scheme or other engine state gets nil there.
namespace melange::oasis::standalone {
struct WaterSim {
    int turns = 40, ticksPerTurn = 2500;
    // turns a round for wum.sim.round and sim.turnStarted's round: the teams (the game's round goes up when a team
    // starts a turn it already had in the round, sim_events.cpp NoteTurnStart), so 2 for the Quick Game
    int turnsPerRound = 2;
    float start = 0;         // world units: the level's water before the script
};
// JSON: `levels` [{turn, world, end}] (world: the water once the turn's start has run, its schedule step, handlers and
// timers of that tick; end: at its last tick, before Water.RiseSpeed.Current adds),
// `schedules` (each waterSchedule call with the turn it ran in), `errors` (what raised, with the turn; the run goes
// on), `calls`. False and `err` when the script does not compile.
bool SimulateWater(const std::string& text, const WaterSim& o, std::string* json, std::string* err);
}  // namespace melange::oasis::standalone
