#pragma once
// Shared state of the sim bridge files (src/lua/sim/sim_*.cpp). Not an interface for other components.
//
// Lua 5.0.1 raises errors with longjmp. Every function that Lua calls (the wum.* trampolines, the protected builders)
// keeps no object with a destructor alive across a call that can raise, and C++ helpers they call are noexcept and
// never call into Lua. Host code that calls into Lua holds C++ objects only outside the pcall it makes.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "lua/engine50.h"
#include "lua/sim/sim_core.h"
#include "melange/sim.h"

namespace melange::simcore {
namespace l5 = lua50;
constexpr int Upvalue(int i) { return l5::kGlobals - i; }
constexpr int kMaxDepth = 8;

struct Mod {
    std::string id, version, chunkName;
    int envRef = -1;
    bool loaded = false;
    uint32_t rng = 0, faults = 0;
    int heapKB = 0;
    uint64_t hashAcc = 0;            // wum.sim.hash values of the current tick (sim_hash.cpp)
    uint32_t hashCalls = 0;
    bool envDirty = true;            // mod code ran since the last environment digest
    uint64_t envDigest = 0;
    bool level = false;              // a level script (wum.level, "level." contributors)
    std::string levelKey, stem, sha256;
    std::vector<std::pair<std::string, std::string>> knots;
};

struct Callback {
    int mod = -1;
    int fnRef = -1;
    uint32_t faults = 0;
    bool dead = false;
};
struct Sub : Callback {
    std::string event;
};
struct Timer : Callback {
    uint32_t due = 0, period = 0;  // period 0 = once
};

struct TickHook {
    int handle;
    int order;
    void (*fn)(uint32_t, void*);
    void* user;
};

struct Match {
    l5::State* L = nullptr;
    uint32_t serial = 0, tick = 0, seed = 0;
    bool initDone = false, active = false;
    std::vector<Mod> mods;
    std::map<uint32_t, Sub> subs;      // handle order = registration order
    std::map<uint32_t, Timer> timers;  // handle order = creation order
    uint32_t nextHandle = 1;
    std::map<uint16_t, std::string> forwarded;  // engine id -> name
    int engineRef[6] = {-1, -1, -1, -1, -1, -1};  // SendMessage, SendIntMessage, SendFloatMessage, SendStringMessage, GetData, SetData
    int consoleRef = -1;
    int refs = 0;
    uint32_t faults = 0, depthDrops = 0;
    int baseHeapKB = 0;
    bool heapWarned = false;
    std::map<uint32_t, uint32_t> cppStreams;  // sim::Random stream key -> LCG state
    bool turnPending = false;
    uint32_t turns = 0;
};
extern Match g;
extern Config g_cfg;
enum EngineFn { kSend, kSendInt, kSendFloat, kSendString, kGetData, kSetData };

// Execution context of the mod code that is running (a stack: sends can deliver to other mods synchronously).
struct Frame {
    int mod;
    int left;  // instruction chunks of 1000
    bool exhausted, suspended, topLevel;
};
extern Frame g_frames[kMaxDepth];
extern int g_depth;
int CurrentModIndex();

// sim_env.cpp
bool BuildEnv(int mod);                         // wum, base, math, string, table into a registry ref
bool LoadChunk(int mod, const std::string& code, std::string* err);  // runs entry.sim in the mod's environment
bool BuildConsoleEnv();
void DropRef(int& ref);
int TakeRef();  // pops the top of the stack into a registry ref

// Calls the function under the registry ref `fnRef` in mod `mod` with the arguments pushed by `push` (after the
// function), under the instruction budget. Returns false on an error or an exhausted budget (`err` gets the text).
using PushArgs = int (*)(l5::State* L, const void* ctx);
bool Invoke(int mod, int fnRef, PushArgs push, const void* ctx, bool topLevel, std::string* err);
// Runs a chunk that is already on the stack (consumed) with `nres` results kept on the stack on success.
int PcallBudgeted(int mod, int nargs, int nres, bool topLevel, bool* exhausted);
void SuspendBudget();
void ResumeBudget();

// sim_events.cpp (noexcept helpers: trampolines call them)
bool KnownEvent(const char* name) noexcept;
bool IsModMessage(const char* name) noexcept;
uint32_t AddSub(int mod, const char* event, int fnRef) noexcept;  // 0 when the mod has too many
bool RemoveSub(int mod, uint32_t handle) noexcept;
uint32_t AddTimer(int mod, uint32_t delay, uint32_t period, int fnRef) noexcept;
bool CancelTimer(int mod, uint32_t handle) noexcept;
void DropModCallbacks(int mod);
void DeliverEvent(const char* event, PushArgs push, const void* ctx);
void RecordFault(int mod, Callback* cb, const char* where, const std::string& err);
void ModLog(int mod, int level, const char* text) noexcept;
constexpr size_t kMaxCallbacksPerMod = 1024;

// sim_api.cpp
void PushWum(int mod);  // pushes the mod's wum table (runs inside the protected env builder)
void CaptureEngineRefs();
enum class SendKind : uint8_t { Plain, Int, Float, String };
struct SendArgs {
    SendKind kind;
    const char* name;
    float f;
    int32_t i;
    const char* s;
};
sim::SendResult DoSend(const SendArgs& a);
const char* SetDataAt(l5::State* L, const char* name, int v);  // pre-checked SetData of stack slot v; nullptr or why
const char* SendResultText(sim::SendResult r);

// sim_level.cpp
void PushLevel(int mod);  // pushes the level script's wum.level table

// sim_weapons.cpp
void PushWeapons(int mod);                 // pushes the mod's wum.sim.weapons table
bool WeaponsAllowed(int mod) noexcept;    // the mod may subscribe to weapon events

// sim_hash.cpp
int __cdecl LHash(l5::State* L);  // wum.sim.hash (upvalue 1 = mod)
void NoteModRuns(int mod);        // mod code is about to run (-1: any mod's environment may change)

// sim_rng.cpp
uint32_t Fnv1a(const char* s);
uint32_t LcgNext(uint32_t& state);
uint32_t Draw24(uint32_t& state);  // 24 random bits (exact in a float)
bool RandomRange(uint32_t& state, double m, double n, double* out, const char** err);

// Engine data containers (sim_data.cpp in the plugin; replaced in the self-test).
int DataType(const char* name);  // -1: unknown DataID, else the container's value type (0/1 int, 2 float, 4 string)
}  // namespace melange::simcore
