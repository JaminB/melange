#pragma once
#include <cstddef>
#include <cstdint>
// Levels: map packs from content mods, Erg Test levels and the online map gate. Build #1077 only.
namespace melange::levels {
bool Enabled();                                  // [Levels] Enabled and every site check passed

enum class Source : uint8_t { Vanilla, Pack, Test };
struct LevelInfo {
    char key[80];                                // registry name, "Multi.<stem>"
    char stem[64];                               // level file stem
    char mod[64];                                // owning mod id; "" for Vanilla and Test
    char title[64];                              // the FETXT string
    Source source; uint8_t levelType, themeType;
    bool registered;                             // present in the data store now
    bool live;                                   // enabled or disabled at the menu this session (offline only)
    char levelKind[16];                          // "multi" | "survivor"
};
int List(LevelInfo* out, int max, bool includeVanilla = false);   // main thread; returns the total
bool IsModLevel(const char* key);                // Pack or Test

// Test workspace (<game>\Melange\erg\test). Registers Multi.<stem> for an ergtest_ stem once per launch, at the
// frontend. Any thread (queued to the main thread); the result arrives through OnTestState.
bool RegisterTest(const char* stem, const char* title, char* err, size_t errLen);
// One-shot override: the next offline SetUpLevelData loads `key` instead of the frontend's choice. Refused in a lobby
// or network session; disarmed on a lobby join, after one use, or after timeoutS.
bool ArmNextLevel(const char* key, int timeoutS = 120);
enum class Tod : uint8_t { Default, Day, Evening, Night };      // Default = what the frontend chose (Quick Game: DAY)
struct ArmOptions { int timeoutS = 120; Tod tod = Tod::Default; };
bool ArmNextLevel(const char* key, const ArmOptions& o);
void Disarm();
bool Armed(char* key, size_t keyLen);
enum class TestState : uint8_t { Idle, Registering, Registered, Armed, Starting, Playing, Ended, Failed };
using TestStateFn = void (*)(TestState s, const char* key, const char* detail, void* user);
int OnTestState(TestStateFn fn, void* user);
void RemoveOnTestState(int handle);

struct LevelStart { const char* key; const char* stem; Source source; bool online; };
using StartFn = void (*)(const LevelStart& s, void* user);        // main thread, at level set-up
int OnLevelStart(StartFn fn, void* user);
void RemoveOnLevelStart(int handle);

bool WaterLevel(float* out);                     // Water.Level in a match
bool SetWaterLevelOffline(float v);              // dev only: offline match and [Levels] DevWater=1

enum class Online : uint8_t { Allowed, NotAllMatch, TestLevel, NotInLobby, LivePack };
Online OnlineStatus(const char* key);            // what the start hold would decide for this level now

struct Stats {
    uint32_t packs, levels, testLevels, cshDeleted, starts, heldStarts; double msRegister;
    uint32_t livePacks, livePackChanges, attractRefusals;
};
Stats GetStats();

// Live packs, offline at the frontend only; main-thread work is queued. A live-changed pack is offline-only until
// restart (Online::LivePack). Refused with a reason: in a lobby, not at the frontend, a Test pending, a pack with
// `sim`/`entry.sim` (restart required), a prefix or key collision.
bool EnablePackLive(const char* modId, char* err, size_t errLen);
bool DisablePackLive(const char* modId, char* err, size_t errLen);
using PacksChangedFn = void (*)(void* user);
int OnPacksChanged(PacksChangedFn fn, void* user);
void RemoveOnPacksChanged(int handle);
}
