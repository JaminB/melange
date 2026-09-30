#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "levels/roots.h"
#include "melange/levels.h"

// Level registration in the game (map packs, the Test workspace, the online map gate). The Levels module owns the
// ini, the hooks and the observers and calls these; the registration component implements them.
namespace melange::levels::registry {
struct Config {
    bool online = true;        // [Levels] Online
    bool randomPool = false;   // [Levels] RandomPool
    bool devWater = false;     // [Levels] DevWater
};
void Install(const Config& cfg);             // once, after the site checks passed
void OnFrame();                              // main thread, every frame (cheap when idle)
bool HasModLevels();                         // an enabled pack or a registered Test level exists
int List(LevelInfo* out, int max, bool includeVanilla);
bool Find(const char* key, LevelInfo* out);
Stats GetStats();

// Test levels and the one-shot override.
bool RegisterTest(const char* stem, const char* title, char* err, size_t errLen);
bool Arm(const char* key, int timeoutS);
void Disarm();
bool Armed(char* key, size_t keyLen);
const char* TakeOverride(const char* frontendKey);   // at level set-up: the armed key (and disarms), or nullptr

// The online map gate.
bool Keep(const char* key, uint32_t request);        // picker policy
Online Status(const char* key);

// Map packs. Thumper's scan asks which mods' levels are refused; the first answer of a launch is frozen and every
// later scan gets it again (the level set follows the per-launch content freeze).
struct Refusal {
    std::string mod, reason;
};
std::vector<Refusal> CheckPacks(const std::vector<roots::PackInput>& inLoadOrder);

// Internal to the registration component.
bool KeepInPool(const char* key, uint32_t levelType);   // random pool policy ([Levels] RandomPool)
bool Lookup(const char* key, Source* source);            // a declared or registered mod level (any thread)
bool RegisterTestLevel(const std::string& stem, const std::string& title, std::string* err);   // main thread
const Config& Settings();
}  // namespace melange::levels::registry

namespace melange::levels::internal {
void FireTestState(TestState s, const char* key, const char* detail);   // main thread
void InstallHooks();                         // the level-name, picker and random-pool hooks, once a mod level exists
}  // namespace melange::levels::internal
