#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "lua/engine50.h"

// The sim bridge without its engine hooks: the per-match lifecycle, mod environments, timers, events and sends.
// bridge.cpp drives it from the hooks; tests/sim_selftest.cpp drives it against a stock Lua 5.0.1 built with float
// numbers. Main thread only.
namespace melange::simcore {
struct ModSource {
    std::string id, version, chunkName, code;
};
struct Config {
    int instrPerCall = 200000;
    bool logTicks = false;
};
void Configure(const Config& c);
void SetSources(std::vector<ModSource> mods);  // load order; applied at the next Init
size_t SourceCount();                          // sim mods plus the level scripts of the level that loads next

// Level scripts: loaded after the sim mods, only when the match's level is `key` (or its Survivor twin).
struct LevelSource {
    std::string key, stem, sha256;
    std::vector<std::pair<std::string, std::string>> knots;  // knot name, kind
    ModSource src;                                           // id "<modId>:<slug>"
};
void SetLevelSources(std::vector<LevelSource> sources);  // load order
void SetLevel(const std::string& key);                   // the level being set up; "" for none
std::string LevelDigest();                               // sha256 of this match's level scripts, comma-joined
bool IsLevelMod(int mod);
void TurnStarted();                                      // GameLogic.Turn.Started: sim.turnStarted at the next tick

// Lifecycle, in engine order.
void ContextCreated(lua50::State* L);
// After the level's Initialise: loads the sim mods if the gate is open. `forwarded` = the engine's forwarded names.
bool Init(const std::vector<std::string>& forwarded, bool gateOpen);
void Message(uint16_t id);  // after HandleMessage; ignores ids that are not forwarded
void Update();              // after the engine's Update
void ContextClosing(lua50::State* L);

void NoteUpdateTime(double us);  // the whole hooked Update (engine + bridge), for sim::GetStats

bool Active();          // sim mods are loaded in this match
bool NeedsTickHooks();  // HandleMessage/Update hooks are needed for this match
bool HasTickHooks();    // C++ tick hooks exist
bool HasBeforeLoad();   // OnBeforeModsLoad observers exist (the Init hook and the gate are needed without sim mods)
lua50::State* MatchL();

// wum.log output of sim mods (level: 0 debug, 1 info, 2 warn, 3 error).
using LogSink = void (*)(const char* modId, int level, uint32_t tick, const char* text);
void SetLogSink(LogSink fn);
void SetHooksChanged(void (*fn)());  // called when NeedsTickHooks() may have changed outside a match start

// Mod message names (the registry keeps the pointer: names live in a never-freed arena).
bool RegisterModMessage(const char* name, uint16_t* idOut, const std::vector<std::string>& vanillaPrefixes);
void FreezeModMessages();
bool ModMessagesFrozen();
constexpr size_t kMaxModMessages = 48;

struct Counters {
    uint32_t refs, subs, timers, tickHooks, consoleEnvs, dispatchDepthDrops;
    double usBridgeLast;
};
Counters GetCounters();
}  // namespace melange::simcore
