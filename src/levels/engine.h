#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Engine glue for levels: SEH-guarded calls that refuse to run when the #1077 bytes of what they call differ, and
// the level-name and picker hooks (through weapons::engine::Mid, so they are listed, gated and suppressible).
namespace melange::levels::engine {
int LoadDataBank(const char* gameRelPath, uint32_t section = 12);   // 0x50cce0 with 0x20; refuses a '.' before the extension
bool AddString(const char* name, const char* value, uint32_t section = 12);   // DRM slot 10 (0x6a5830), flag 1
bool GetFloat(const char* name, float* v);                          // 0x50b7f0
bool SetFloat(const char* name, float v);                           // 0x50bac0
const char* CurrentLevelKey();                                      // WXD.Level.Current
bool AtFrontend();                                                  // Game.Scope == 0 and no match VM
bool AddRoot(const char* gameRelDir);                               // assets::searchpath::Add
constexpr uintptr_t kSetUp = 0x4ec370, kLevelName = 0x4ec46e, kPickerKeep = 0x4c2f48, kPickerLock = 0x72f96e,
    kPickerCommit = 0x4c34dc, kPickerAdd = 0x4bcad0, kLoadDataBank = 0x50cce0, kLandImport = 0x477f50,
    kGetFloat = 0x50b7f0, kSetFloat = 0x50bac0;
// Mid-hook at kLevelName: calls OnLevelStart observers, then replaces [esp+0x20] when an override is armed.
// Mid-hook at kPickerKeep: `bool (*Keep)(const char* key, uint32_t request)`; false takes the filter's reject path.

constexpr uintptr_t kGetInt = 0x50b790, kPickerReject = 0x4c3545;

bool SitesOk();                                  // every function and hook site above has its #1077 bytes
bool LevelNameIntact(const char* s);             // no '.' before the extension, printable, <= 200 characters

// Registry entry fields (WXFE_LevelDetails) of a data resource; false if absent or another class.
struct Details {
    std::string file, script, lock, frontendName;
    int levelType = -1, themeType = -1, previewType = -1;
};
bool LevelDetails(const char* key, Details* out);

// The level-name hook. `decide` gets the frontend's key and returns the key to load (the same pointer to keep it);
// the returned text is copied before it is written into the game's frame. Main thread.
using DecideFn = const char* (*)(const char* frontendKey, void* user);
bool InstallLevelHook(DecideFn decide, void* user);
void EnableLevelHook(bool on);
bool LevelHookEnabled();
void CurrentLevelHook(DecideFn* fn, void** user);  // for a caller that chains to the installed decision

using KeepFn = bool (*)(const char* key, uint32_t request);
bool InstallPickerHook(KeepFn keep);
void EnablePickerHook(bool on);
bool PickerHookEnabled();
KeepFn CurrentPickerKeep();

// The random pools (Quick Game, network quick starts): MissionService's per-type level lists are built by one
// ForEachResource callback. The mid-hook at kPoolEntry gets each candidate; `keep` returning false resumes at
// kPoolSkip, the callback's own "not added" exit. `request` is the entry's Level_Type.
constexpr uintptr_t kPoolEntry = 0x72faec, kPoolSkip = 0x72fce3, kMissionService = 0x97a998;
bool InstallPoolHook(KeepFn keep);
void EnablePoolHook(bool on);
bool PoolHookEnabled();
std::vector<std::string> PoolKeys();             // the type-0 list as it stands now

// Posts WXMsg.SetDataResource <name> <value> (the frontend's own way to change a data resource, persisted in the
// save). Main thread.
constexpr uintptr_t kMsgFactory = 0x96d14c, kMsgAlloc = 0x691705, kTwoStringMsgInit = 0x69154e, kMsgPost = 0x6910e4;
bool PostDataResource(const char* name, const char* value);

constexpr uintptr_t kSetString = 0x50bb80, kClearDataBank = 0x50cf00, kPoolRebuild = 0x72fcf4,
    kFrontendControl = 0x95a298;
bool SetString(const char* name, const char* v);        // cdecl(const char**, const char*); not persisted by itself
bool GetString(const char* name, std::string* out);
bool ClearDataBank(uint32_t section);                   // 12 only; refuses any other section
bool RebuildPools();                                    // __thiscall on [kMissionService]; main thread, frontend only
struct FrontendState { bool valid; uint32_t state; bool attractAllowed, attractRunning; uint32_t idleDeadline; };
FrontendState ReadFrontend();                           // FCS fields, byte-checked
bool Loading();                                         // between a SetUpLevelData and the match VM (or the frontend)
bool LoadingAtSetUp();                                  // in the level hook: an earlier set-up had not reached its match
using StartGameFn = void (*)(const char* text, void* user);   // WXMsg.StartGame observer (bus subscription)
int OnStartGame(StartGameFn fn, void* user);
void RemoveOnStartGame(int handle);

// Type lists of MissionService by offset (+0x2c multi, +0xbc survivor, ...): the keys it holds now.
std::vector<std::string> PoolKeysAt(uint32_t offset);
}  // namespace melange::levels::engine
