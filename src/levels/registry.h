#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

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
}  // namespace melange::levels::registry

namespace melange::levels::internal {
void FireTestState(TestState s, const char* key, const char* detail);   // main thread
void InstallHooks();                         // the level-name and picker hooks, once a mod level exists
}  // namespace melange::levels::internal
