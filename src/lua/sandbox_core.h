#pragma once
// Sandbox internals shared by sandbox*.cpp and wum_*.cpp. Not a contract: only the Sandbox's own files include it.
#include <lua.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "tools/json_read.h"

namespace melange::sandbox {
struct Limits {
    int64_t instrPerCall = 500000;
    size_t modBytes = 16u << 20;
    size_t totalBytes = 96u << 20;
    bool hotReload = true;
};

struct SettingDecl {
    std::string key, type, def;
    double min = 0, max = 0;
    bool hasMin = false, hasMax = false;
    std::vector<std::string> options;
};

struct Gen;
struct ModRec {
    std::string id, name, version;
    std::wstring dir;
    std::string entryClient, filesystem = "none";
    std::vector<SettingDecl> settings;
    bool unsafe = false, granted = false;
    int slot = 0;   // allocator owner slot, stable for the process
    int order = 0;  // load order from Thumper
    Gen* gen = nullptr;
    std::string error;
    uint32_t faults = 0;
    uint64_t instructions = 0;
    uint64_t frameInstructions = 0;  // reset every Frame(): bounds one mod's total work across all its callbacks
    double msFrame = 0, msAccum = 0;
    // wum.log rate limit
    uint64_t logWindow = 0;
    uint32_t logCount = 0, logDropped = 0;
    // wum.storage: key -> serialized JSON value
    std::map<std::string, std::string> storage;
    size_t storageBytes = 0;
    bool storageLoaded = false, storageDirty = false;
    double storageWritten = 0;
};

enum class CbKind : uint8_t { Event, Timer, Panel, Menu, Hotkey, Draw, Reload, WebMethod };
const char* KindName(CbKind k);

struct Callback {
    uint32_t id = 0;
    Gen* gen = nullptr;
    int ref = LUA_NOREF;  // the Lua function
    CbKind kind = CbKind::Event;
    std::string label;
    int faults = 0;
    bool disabled = false, dead = false, attached = false;
    std::function<bool()> attach;  // external registration (overlay, draw); run when the generation commits
    std::function<void()> revoke;
};

struct Gen {
    ModRec* mod = nullptr;
    uint32_t serial = 0;
    int envRef = LUA_NOREF;
    bool committed = false;
    std::vector<uint32_t> callbacks;
    int modTableRef = LUA_NOREF;  // this environment's wum.mod
    std::set<std::string> panelIds;
    std::vector<std::function<void()>> cleanups;  // run when the generation is revoked
    int textureCount = 0;  // wum.draw.texture: capped separately from kMaxHandlesPerGen (not a Callback)
};

// ---- VM and calls (sandbox.cpp)
bool Start(const Limits& lim);
void Stop();
bool Running();
void Frame();                        // main thread, once per frame: timers, queued events, melange.frame
lua_State* L();
const Limits& GetLimits();
ModRec* Current();                   // mod whose code runs now, or nullptr (console / host)
ModRec* FindMod(const std::string& id);
std::vector<ModRec*> LoadedMods();   // load order
Callback* FindCallback(uint32_t id);
// Registers a callback for the running mod from the Lua function at `fnIdx`. Lua error if no mod is running.
Callback* NewCallback(lua_State* L, int fnIdx, CbKind kind, std::string label);
// Runs cb->attach now if the generation is committed (else at commit). False (callback killed) if attach fails.
bool Activate(Callback* cb);
Gen* CurrentGen();
void KillCallback(uint32_t id);      // revoke + unref; the id is never reused
bool CallbackLive(const Callback* cb);  // not dead, not disabled, generation is the mod's current one
// Pushes the callback's function, then calls it with `nargs` arguments already pushed above it by `pushArgs`.
// Protected, budgeted, fault-counted. Returns false on error (already logged).
bool Invoke(Callback* cb, const std::function<int(lua_State*)>& pushArgs, int nresults = 0,
            const std::function<void(lua_State*)>& onResults = {});
// Runs host code that touches the Lua stack inside lua_pcall. Returns false (and logs) on a Lua error.
bool Protected(const char* what, const std::function<void(lua_State*)>& fn);
std::wstring Widen(const std::string& utf8);
bool ReadWhole(const std::wstring& path, size_t cap, std::string* out, std::string* err);
bool SafeRelPath(const std::string& rel);  // relative, no "..", no drive or root
void PushFrozen(lua_State* L, int realIdx);  // replaces nothing; pushes a read-only proxy of the table at realIdx
int ErrorF(lua_State* L, const char* fmt, ...);
std::string DescribeValue(lua_State* L, int idx, int depth);  // console formatting

// ---- limits (sandbox_limits.cpp)
struct Ctx {
    ModRec* mod = nullptr;
    Gen* gen = nullptr;
    Callback* cb = nullptr;
    int64_t budget = 0;
    bool exhausted = false, oom = false;
};
void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize);
void InstallHook(lua_State* L);
void PushCtx(ModRec* mod, Gen* gen, Callback* cb);
Ctx PopCtx();               // also re-arms the main thread's hook if it was sped up
Ctx* TopCtx();
int MaxSlots();
size_t SlotBytes(int slot);
size_t TotalBytes();
uint64_t TotalInstructions();
void ResetLimits();

// ---- data conversion (wum_core.cpp)
bool ToJson(lua_State* L, int idx, json::Value* out, std::string* err, int depth = 0);
void PushJson(lua_State* L, const json::Value& v);
std::string WriteJson(const json::Value& v);

// Reads entry.client, permissions and settings from <dir>\spice.json (Thumper validates the rest).
bool ReadManifest(ModRec* m, std::string* err);

// ---- dispatch (sandbox.cpp)
void QueueEvent(std::string name, json::Value payload);  // main thread
void AddListener(const std::string& name, uint32_t cbId);
void RemoveListener(uint32_t cbId);
uint32_t AddTimer(uint32_t cbId, double delay, double interval);
void FireDirect(const std::string& name, const json::Value& payload);  // melange.* events, now

// ---- captured output for Eval (print and wum.log go here too while set)
std::string* Capture();

// ---- game state that the glue feeds and wum.game reads
struct GameState {
    std::string scene = "boot";
    bool inMatch = false, online = false;
    int turn = 0;
    int team = -1;
    uint32_t tick = 0;
};
GameState& Game();

// ---- library registration: every wum_*.cpp adds its namespaces here (static registrars).
// Shared: fills fields of the shared wum table at index `wum` (namespaces are frozen after).
// PerEnv: fills fields of one environment's wum table (not frozen yet); mod may be nullptr for the console.
using SharedInit = void (*)(lua_State* L, int wum);
using EnvInit = void (*)(lua_State* L, int wum, ModRec* mod);
struct LibRegistrar {
    LibRegistrar(SharedInit shared, EnvInit perEnv);
};
void RegisterFunctions(lua_State* L, int tableIdx, const luaL_Reg* fns);

// ---- mod-facing logging (wum.log, print); jlog "mod"
void ModLog(ModRec* m, int level, const std::string& text);  // level: 0 debug, 1 info, 2 warn, 3 error
// Sandbox's own records; jlog "sandbox"
void SysLog(int level, const std::string& msg, const std::string& modId = {}, const std::string& detail = {});

// ---- storage (wum_core.cpp)
void StorageFlush(ModRec* m, bool force);
void FlushAllStorage();
std::vector<std::string> ApiNames();  // "wum.<ns>.<name>" in an environment with every namespace (docs check)
std::wstring StoragePath(const ModRec* m);

// ---- glue: implemented by sandbox_module.cpp in the game and by the offline self-test.
double NowSeconds();
void EngineSubscribe(const std::string& name, bool on);  // first listener / last listener of an engine message
}  // namespace melange::sandbox
