#pragma once
#include <cstddef>
#include <cstdint>

// The engine's Lua 5.0.1 (the match VM) and XScriptService, behind prologue checks. Build #1077 only.
// lua_Number is float in this build. lua_error is a longjmp: never let it cross a C++ frame with live destructors.
namespace melange::lua50 {
using State = void;                                   // engine lua_State
using CFunction = int (*)(State*);
using Hook = void (*)(State*, void*);                 // void* = lua_Debug*
struct Api {                                          // cdecl
    int (*gettop)(State*); void (*settop)(State*, int); int (*type)(State*, int);
    int (*isnumber)(State*, int); int (*isstring)(State*, int);
    float (*tonumber)(State*, int); int (*toboolean)(State*, int); const char* (*tostring)(State*, int);
    void* (*touserdata)(State*, int);
    void (*pushnil)(State*); void (*pushnumber)(State*, float); void (*pushstring)(State*, const char*);
    void (*pushcclosure)(State*, int (*)(State*), int); void (*pushboolean)(State*, int);
    void (*pushlightuserdata)(State*, void*);
    void (*gettable)(State*, int); void (*settable)(State*, int); void (*rawget)(State*, int); void (*rawset)(State*, int);
    void (*rawgeti)(State*, int, int); void (*rawseti)(State*, int, int); void (*newtable)(State*);
    int (*getmetatable)(State*, int); int (*setmetatable)(State*, int);
    int (*pcall)(State*, int, int, int); void (*error)(State*); int (*next)(State*, int);
    int (*sethook)(State*, void (*)(State*, void*), int, int);
    int (*ref)(State*, int); void (*unref)(State*, int, int);
    int (*loadbuffer)(State*, const char*, size_t, const char*);
    // Added by S after the contract freeze (additive).
    int (*checkstack)(State*, int); size_t (*strlen)(State*, int);
    void (*pushvalue)(State*, int); void (*insert)(State*, int); void (*remove)(State*, int);
    void (*replace)(State*, int); void (*pushlstring)(State*, const char*, size_t);
    int (*setfenv)(State*, int); void (*getfenv)(State*, int);
    Hook (*gethook)(State*); int (*gethookmask)(State*); int (*getgccount)(State*);
};
const Api& A();
constexpr int kGlobals = -10001, kRegistry = -10000;
enum : int { kTNone = -1, kTNil = 0, kTBoolean, kTLightUserdata, kTNumber, kTString, kTTable, kTFunction, kTUserdata, kTThread };
enum : int { kMaskCall = 1, kMaskRet = 2, kMaskLine = 4, kMaskCount = 8 };
bool Check();                        // every prologue; the bridge and console refuse to run on failure
int EntryPoints();                   // number of prologues Check() verifies
uintptr_t ScriptService();           // current XScriptService or 0
State* MatchState();                 // ss+0x38 or nullptr
int RunState();                      // ss+0x3c (1 running, 2 halted); 0 without a script service
bool AllowAll();                     // ss+0x1b0
// The engine's gate with its real semantics (every second entry), so a pre-check agrees with the engine.
// Deny lists only: the engine skips them when AllowAll() is set.
bool EngineWouldDenySend(const char* name, const char* param);
bool EngineWouldDenyData(const char* name);
uint16_t Lookup(const char* name);   // 0x690d44 semantics without its miss log; 0xffff if unregistered
bool Register(const char* name);     // 0x690bf7; `name` must outlive the process
uint32_t RegistryCount();            // names in the message registry
uint32_t RegistryCapacity();         // MRS.MaxMessages
uint32_t LogicSeed();                // logic RNG state when the current match VM was created; needs Track()
uint32_t LogicRng();                 // logic RNG state now (0x96d034)

// Match VM tracking: a post-hook on CreateContext and a pre-hook on the XLuaContext dtor that only record.
// Installed by the first Track() call; observers run on the thread that creates or closes the VM (main).
bool Track();
using ContextFn = void (*)(bool created, State* L, void* user);
int OnContext(ContextFn fn, void* user);
void RemoveOnContext(int handle);
uint32_t ContextSerial();            // +1 per match VM seen by Track()

// Standard libraries the engine compiles in but does not open in the match VM (luaL_reg arrays).
struct LibReg { const char* name; CFunction fn; };
const LibReg* StringLib();           // len sub lower upper char rep byte format dump find gfind gsub
const LibReg* TableLib();            // concat foreach foreachi getn setn sort insert remove

// Hook targets (installed by the sim bridge; Check() covers their prologues).
constexpr uintptr_t kCreateContext = 0x6958d2, kInit = 0x6956b9, kLoadChunk = 0x697d04, kHandleMessage = 0x6953e9,
                    kUpdate = 0x69566a, kCallGlobal = 0x698e88, kEval = 0x6994c0, kLuaCtxDtor = 0x78a202;
}
