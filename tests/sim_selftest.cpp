// Offline self-test of the sim bridge (src/lua/sim/sim_*.cpp) against a stock Lua 5.0.1 built with float numbers,
// which stands in for the engine's match VM. This file also provides the lua50 functions the bridge uses, with the
// engine's behaviour where it matters: the Send*/GetData/SetData globals halt the "level script" (run state 2) on an
// unregistered name, a denied name or a bad data id, exactly the failures the bridge must pre-check.
extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_core.h"
#include "lua/sim/sim_hash.h"
#include "melange/sim.h"
#include "melange/wormsign.h"
#include "wormsign/contrib.h"

namespace l5 = melange::lua50;
namespace core = melange::simcore;

namespace fake {
lua_State* L = nullptr;
int runState = 1;
bool allowAll = true;
uint32_t seed = 0x1234abcd;
std::map<std::string, uint16_t> registry;
uint16_t nextSlot = 10;
std::set<std::string> forwarded = {"GameLogic.Turn.Ended", "GameLogic.Turn.Started", "Weapon.Fired"};
struct Datum {
    int type;
    float num;
    std::string str;
};
std::map<std::string, Datum> data;
std::vector<std::string> sent;
std::vector<l5::LibReg> strlib, tablib;
std::vector<std::string> halts;

void Halt(const std::string& why) {
    runState = 2;
    halts.push_back(why);
}

void Reg(const char* n) { registry.emplace(n, static_cast<uint16_t>(0x8000 | nextSlot++)); }

std::string Underscored(std::string s) {
    std::replace(s.begin(), s.end(), '.', '_');
    return s;
}

// The engine's HandleMessage for a forwarded name: CallGlobal(name with _), then the bridge's post-hook.
void Deliver(const char* name) {
    const std::string g = Underscored(name);
    lua_pushstring(L, g.c_str());
    lua_gettable(L, LUA_GLOBALSINDEX);
    if (lua_isnil(L, -1))
        lua_pop(L, 1);
    else if (lua_pcall(L, 0, 0, 0))
        lua_pop(L, 1);
    core::Message(registry[name]);
}

int SendImpl(lua_State* S, int kind) {
    const int n = lua_gettop(S);
    if (n != (kind ? 2 : 1) || !lua_isstring(S, 1)) {
        Halt("bad args");
        return 0;
    }
    const std::string name = lua_tostring(S, 1);
    if (!registry.count(name)) {
        Halt(name + " : Message name not registered");
        return 0;
    }
    std::string param = kind ? lua_tostring(S, 2) : "";
    if (!allowAll && name == "GameLogic.PauseGame") {
        Halt(name + " : Message permission denied");
        return 0;
    }
    sent.push_back(name + (kind ? "|" + param : ""));
    if (forwarded.count(name)) Deliver(name.c_str());
    return 0;
}
int FSend(lua_State* S) { return SendImpl(S, 0); }
int FSendValue(lua_State* S) { return SendImpl(S, 1); }

int FGetData(lua_State* S) {
    const std::string name = lua_tostring(S, 1) ? lua_tostring(S, 1) : "";
    auto it = data.find(name);
    if (it == data.end()) {
        Halt("Incorrect DataID in function 'GetData'");
        return 0;
    }
    if (!allowAll && name == "GameLogic.ArtilleryMode") {
        Halt(name + " : Data Access Denied");
        return 0;
    }
    if (it->second.type == 4)
        lua_pushstring(S, it->second.str.c_str());
    else
        lua_pushnumber(S, it->second.num);
    return 1;
}

int FSetData(lua_State* S) {
    const std::string name = lua_tostring(S, 1) ? lua_tostring(S, 1) : "";
    auto it = data.find(name);
    if (it == data.end()) {
        Halt("Incorrect DataID in function 'SetData'");
        return 0;
    }
    if (it->second.type == 4) {
        if (!lua_isstring(S, 2)) return Halt("Data is not a string"), 0;
        it->second.str = lua_tostring(S, 2);
    } else {
        if (!lua_isnumber(S, 2)) return Halt("Data is not a number"), 0;
        it->second.num = lua_tonumber(S, 2);
    }
    return 0;
}

void CollectLib(lua_State* S, const char* name, std::vector<l5::LibReg>& out) {
    lua_pushstring(S, name);
    lua_gettable(S, LUA_GLOBALSINDEX);
    lua_pushnil(S);
    while (lua_next(S, -2)) {
        if (lua_iscfunction(S, -1)) out.push_back({_strdup(lua_tostring(S, -2)), reinterpret_cast<l5::CFunction>(lua_tocfunction(S, -1))});
        lua_pop(S, 1);
    }
    lua_pop(S, 1);
    out.push_back({nullptr, nullptr});
}

const char kLevelScript[] = R"(
turnsSeen = 0
levelValue = { a = 1, b = "two" }
function GameLogic_Turn_Ended() turnsSeen = turnsSeen + 1 end
)";

lua_State* NewMatchVM() {
    lua_State* S = lua_open();
    luaopen_base(S);
    luaopen_math(S);
    lua_settop(S, 0);
    auto reg = [S](const char* n, lua_CFunction f) {
        lua_pushstring(S, n);
        lua_pushcclosure(S, f, 0);
        lua_settable(S, LUA_GLOBALSINDEX);
    };
    reg("SendMessage", &FSend);
    reg("SendIntMessage", &FSendValue);
    reg("SendFloatMessage", &FSendValue);
    reg("SendStringMessage", &FSendValue);
    reg("GetData", &FGetData);
    reg("SetData", &FSetData);
    luaL_loadbuffer(S, kLevelScript, sizeof kLevelScript - 1, "=level");
    lua_pcall(S, 0, 0, 0);
    lua_settop(S, 0);
    return S;
}
}  // namespace fake

#define API(m, f) .m = reinterpret_cast<decltype(l5::Api::m)>(&f)
namespace melange::lua50 {
const Api& A() {
    static const Api api = {
        API(gettop, lua_gettop), API(settop, lua_settop), API(type, lua_type), API(isnumber, lua_isnumber),
        API(isstring, lua_isstring), API(tonumber, lua_tonumber), API(toboolean, lua_toboolean),
        API(tostring, lua_tostring), API(touserdata, lua_touserdata), API(pushnil, lua_pushnil),
        API(pushnumber, lua_pushnumber), API(pushstring, lua_pushstring), API(pushcclosure, lua_pushcclosure),
        API(pushboolean, lua_pushboolean), API(pushlightuserdata, lua_pushlightuserdata), API(gettable, lua_gettable),
        API(settable, lua_settable), API(rawget, lua_rawget), API(rawset, lua_rawset), API(rawgeti, lua_rawgeti),
        API(rawseti, lua_rawseti), API(newtable, lua_newtable), API(getmetatable, lua_getmetatable),
        API(setmetatable, lua_setmetatable), API(pcall, lua_pcall), API(error, lua_error), API(next, lua_next),
        API(sethook, lua_sethook), API(ref, luaL_ref), API(unref, luaL_unref), API(loadbuffer, luaL_loadbuffer),
        API(checkstack, lua_checkstack), API(strlen, lua_strlen), API(pushvalue, lua_pushvalue),
        API(insert, lua_insert), API(remove, lua_remove), API(replace, lua_replace), API(pushlstring, lua_pushlstring),
        API(setfenv, lua_setfenv), API(getfenv, lua_getfenv), API(gethook, lua_gethook),
        API(gethookmask, lua_gethookmask), API(getgccount, lua_getgccount),
    };
    return api;
}
bool Check() { return true; }
int EntryPoints() { return 0; }
uintptr_t ScriptService() { return fake::L ? 1 : 0; }
State* MatchState() { return fake::L; }
int RunState() { return fake::L ? fake::runState : 0; }
bool AllowAll() { return fake::allowAll; }
bool EngineWouldDenySend(const char* name, const char*) { return std::strcmp(name, "GameLogic.PauseGame") == 0; }
bool EngineWouldDenyData(const char* name) { return std::strcmp(name, "GameLogic.ArtilleryMode") == 0; }
uint16_t Lookup(const char* name) {
    auto it = fake::registry.find(name ? name : "");
    return it == fake::registry.end() ? 0xffff : it->second;
}
bool Register(const char* name) {
    fake::Reg(name);
    return true;
}
uint32_t RegistryCount() { return static_cast<uint32_t>(fake::registry.size()); }
uint32_t RegistryCapacity() { return 1300; }
uint32_t LogicSeed() { return fake::seed; }
uint32_t LogicRng() { return 0; }
bool Track() { return true; }
int OnContext(ContextFn, void*) { return 1; }
void RemoveOnContext(int) {}
uint32_t ContextSerial() { return 0; }
const LibReg* StringLib() { return fake::strlib.data(); }
const LibReg* TableLib() { return fake::tablib.data(); }
}  // namespace melange::lua50

namespace melange::simcore {
int DataType(const char* name) {
    auto it = fake::data.find(name ? name : "");
    return it == fake::data.end() ? -1 : it->second.type;
}
}  // namespace melange::simcore

namespace {
int g_pass = 0, g_fail = 0;
void Expect(bool ok, const std::string& what) {
    if (getenv("SIM_TRACE")) fprintf(stderr, "check: %s\n", what.c_str());
    ok ? ++g_pass : ++g_fail;
    if (!ok) printf("FAIL: %s\n", what.c_str());
}

std::vector<std::string> g_modLog, g_log;
void Sink(const char* id, int level, uint32_t tick, const char* text) {
    g_modLog.push_back(std::string(id) + "|" + std::to_string(level) + "|" + std::to_string(tick) + "|" + text);
}
void Tap(const char*, const char* msg) {
    g_log.push_back(msg);
    if (getenv("SIM_TRACE")) fprintf(stderr, "log: %s\n", msg);
}
bool Logged(const std::string& needle) {
    return std::any_of(g_log.begin(), g_log.end(), [&](const std::string& l) { return l.find(needle) != std::string::npos; });
}

std::string Scalar(lua_State* S, int i) {
    switch (lua_type(S, i)) {
    case LUA_TNUMBER:
    case LUA_TSTRING: return lua_tostring(S, i);
    case LUA_TBOOLEAN: return lua_toboolean(S, i) ? "true" : "false";
    case LUA_TNIL: return "nil";
    default: return lua_typename(S, lua_type(S, i));
    }
}

std::string Digest(lua_State* S, int t, int depth) {
    std::vector<std::string> v;
    lua_pushnil(S);
    while (lua_next(S, t < 0 && t > LUA_REGISTRYINDEX ? t - 1 : t)) {
        const int top = lua_gettop(S);
        lua_pushvalue(S, top - 1);
        std::string k = Scalar(S, top + 1) + "=";
        lua_pop(S, 1);
        if (lua_istable(S, top) && depth > 0)
            k += "{" + Digest(S, top, depth - 1) + "}";
        else
            k += lua_type(S, top) == LUA_TFUNCTION ? "function" : Scalar(S, top);
        v.push_back(k);
        lua_pop(S, 1);
    }
    std::sort(v.begin(), v.end());
    std::string out;
    for (auto& s : v) out += s + ";";
    return out;
}
std::string GlobalsDigest() { return Digest(fake::L, LUA_GLOBALSINDEX, 1); }

std::string Storage(const char* mod, const char* key) {
    lua_State* S = fake::L;
    if (!melange::simbridge::PushModEnv(mod)) return "<no env>";
    lua_pushstring(S, "wum");
    lua_gettable(S, -2);
    lua_pushstring(S, "sim");
    lua_gettable(S, -2);
    lua_pushstring(S, "storage");
    lua_gettable(S, -2);
    lua_pushstring(S, key);
    lua_gettable(S, -2);
    std::string r = lua_istable(S, -1) ? "{" + Digest(S, lua_gettop(S), 0) + "}" : Scalar(S, -1);
    lua_settop(S, 0);
    return r;
}

bool GlobalNil(const char* n) {
    lua_pushstring(fake::L, n);
    lua_rawget(fake::L, LUA_GLOBALSINDEX);
    const bool nil = lua_isnil(fake::L, -1);
    lua_pop(fake::L, 1);
    return nil;
}

int __cdecl ProbeFn(l5::State* S) {
    static char buf[64];
    const char* m = melange::simbridge::CurrentMod();
    snprintf(buf, sizeof buf, "%s:%s", melange::simbridge::InTopLevelChunk() ? "top" : "cb", m ? m : "-");
    l5::A().pushstring(S, buf);
    return 1;
}

const char kAlpha[] = R"(
local s = wum.sim.storage
s.order = {}
s.ticks = 0
s.turns = 0
s.draws = {}
wum.events.on("tick", function(t) s.ticks = s.ticks + 1 end)
wum.sim.after(3, function(t) table.insert(s.order, "after3@" .. t) end)
wum.sim.after(2, function(t) table.insert(s.order, "after2@" .. t) end)
wum.sim.every(50, function(t) table.insert(s.order, "every@" .. t) end)
wum.sim.after(2, function(t) table.insert(s.order, "after2b@" .. t) end)
local once = wum.sim.every(7, function(t) table.insert(s.order, "cancelled@" .. t) end)
wum.sim.cancel(once)
wum.events.on("GameLogic.Turn.Ended", function(name)
  s.turns = s.turns + 1
  local r = wum.sim.random(1, 100)
  table.insert(s.draws, r)
  wum.log.info("turn", s.turns, "draw", r, "d6", math.random(6), "f", wum.sim.randomFloat() < 1)
  local ok, why = wum.sim.sendInt("Melange.Test.Ping", s.turns)
  if not ok then wum.log.error("ping failed", why) end
end)
wum.events.on("Melange.Test.Ping", function(name, v) s.selfPing = v end)
wum.events.on("sim.test", function(name, a, b) s.dispatched = name .. ":" .. a .. ":" .. b end)
leak = 1
s.fmt = string.format("%d-%s", 7, "x") .. string.rep("ab", 2) .. table.getn({1, 2, 3})
s.absent = type(string.dump) .. type(loadstring) .. type(setfenv) .. type(SendMessage) .. type(io) .. type(coroutine) .. type(getfenv)
s.gIsEnv = tostring(_G.leak == 1 and rawget(_G, "leak") == 1)
s.seed = tostring(pcall(math.randomseed, 1))
s.unknown = tostring(wum.events.on("No.Such.Event", function() end))
s.top = wum.sim.probe()
s.id = wum.mod.id .. "@" .. wum.mod.version
s.weapons = tostring(pcall(function() return wum.sim.weapons.bazooka end))
wum.events.on("tick", function(t) if t == 1 then s.inTick = wum.sim.probe() end end)
)";

const char kBeta[] = R"(
local s = wum.sim.storage
s.pings = 0
s.loops = 0
wum.events.on("Melange.Test.Ping", function(name, v) s.pings = s.pings + 1 s.lastPing = v end)
wum.events.on("GameLogic.Turn.Ended", function() s.loops = s.loops + 1 while true do end end)
)";

const char kGamma[] = R"(
local s = wum.sim.storage
s.runs = 0
wum.sim.every(10, function()
  s.runs = s.runs + 1
  local ok = pcall(function() while true do end end)
  s.after = "reached"
  while true do pcall(function() while true do end end) end
end)
)";

const char kSender[] = R"(
local s = wum.sim.storage
local function r(ok, why) return tostring(ok) .. ":" .. tostring(why) end
s.unreg = r(wum.sim.send("Not.Registered.Name"))
s.pause = r(wum.sim.send("GameLogic.PauseGame"))
s.badData = r(wum.sim.getData("No.Such.Data"))
s.getInt = r(wum.sim.getData("Test.Int"))
s.setWrong = r(wum.sim.setData("Test.Int", "text"))
s.setOk = r(wum.sim.setData("Test.Int", 7))
s.getInt2 = r(wum.sim.getData("Test.Int"))
s.artillery = r(wum.sim.getData("GameLogic.ArtilleryMode"))
s.str = r(wum.sim.sendString("Melange.Test.Ping", "hello"))
s.echo = 0
wum.events.on("Melange.Test.Echo", function(name, v)
  s.echo = s.echo + 1
  wum.sim.sendInt("Melange.Test.Echo", v + 1)
end)
)";

const char kBadTop[] = R"(
wum.events.on("tick", function() wum.sim.storage.x = 1 end)
error("boom")
)";

struct RunResult {
    std::vector<std::string> modLog;
    std::string alphaDraws;
};

RunResult RunMatch(uint32_t seed, bool full) {
    g_modLog.clear();
    fake::seed = seed;
    fake::runState = 1;
    fake::allowAll = false;
    fake::sent.clear();
    fake::halts.clear();
    fake::data = {{"Test.Int", {0, 42, ""}}, {"GameLogic.ArtilleryMode", {0, 1, ""}}};
    fake::L = fake::NewMatchVM();
    lua_State* S = fake::L;
    const std::string before = GlobalsDigest();

    std::vector<core::ModSource> mods = {
        {"alpha", "1.2.3", "@alpha/sim/main.lua", kAlpha},
        {"beta", "1.0.0", "@beta/sim/main.lua", kBeta},
        {"gamma", "1.0.0", "@gamma/sim/main.lua", kGamma},
        {"broken", "1.0.0", "@broken/sim/main.lua", "this is not lua ("},
        {"badtop", "1.0.0", "@badtop/sim/main.lua", kBadTop},
        {"bytecode", "1.0.0", "@bytecode/sim/main.lua", std::string("\x1bLua\x50", 5)},
        {"sender", "1.0.0", "@sender/sim/main.lua", kSender},
    };
    core::SetSources(mods);
    core::ContextCreated(S);
    const bool active = core::Init({"GameLogic.Turn.Ended", "GameLogic.Turn.Started", "Not.Registered.Forward"}, true);
    Expect(active && core::Active() && melange::sim::ModsActive(), "mods active after Init");
    const auto loaded = melange::simbridge::LoadedMods();
    Expect(loaded == std::vector<std::string>({"alpha", "beta", "gamma", "sender"}),
           "loaded mods: alpha beta gamma sender");
    Expect(GlobalsDigest() == before, "no global of the match VM added or changed by Init");
    Expect(lua_gettop(S) == 0, "stack balanced after Init");
    Expect(lua_gethookmask(S) == 0, "no hook left after Init");

    if (full) {
        Expect(Storage("alpha", "fmt") == "7-xabab3", "string/table in a mod environment: " + Storage("alpha", "fmt"));
        Expect(Storage("alpha", "absent") == "nilnilnilnilnilnilnil", "absent functions: " + Storage("alpha", "absent"));
        Expect(Storage("alpha", "gIsEnv") == "true", "_G is the mod's environment");
        Expect(Storage("alpha", "seed") == "false", "math.randomseed raises");
        Expect(Storage("alpha", "unknown") == "nil", "unknown event refused");
        Expect(Storage("alpha", "top") == "top:alpha", "InTopLevelChunk/CurrentMod in the chunk: " + Storage("alpha", "top"));
        Expect(Storage("alpha", "id") == "alpha@1.2.3", "wum.mod id and version");
        Expect(Storage("alpha", "weapons") == "false", "wum.sim.weapons raises");
        Expect(GlobalNil("leak") && GlobalNil("wum") && GlobalNil("string") && GlobalNil("table"),
               "mod globals stay in its environment");
        Expect(Storage("sender", "unreg") == "nil:not registered", "unregistered send: " + Storage("sender", "unreg"));
        Expect(Storage("sender", "pause") == "nil:denied", "denied send (allowAll 0): " + Storage("sender", "pause"));
        Expect(Storage("sender", "badData") == "nil:unknown data id", "unknown data id: " + Storage("sender", "badData"));
        Expect(Storage("sender", "getInt") == "42:nil", "getData: " + Storage("sender", "getInt"));
        Expect(Storage("sender", "setWrong") == "nil:the value must be a number", "setData type: " + Storage("sender", "setWrong"));
        Expect(Storage("sender", "setOk") == "true:nil" && Storage("sender", "getInt2") == "7:nil", "setData then getData");
        Expect(Storage("sender", "artillery") == "nil:denied", "denied data: " + Storage("sender", "artillery"));
        Expect(Storage("sender", "str") == "true:nil", "sendString of a mod message");
        Expect(fake::runState == 1 && fake::halts.empty(), "run state stays 1 after refused sends and data access");
    }

    std::vector<std::string> hookTicks;
    const int th = melange::sim::AddTickHook([](uint32_t t, void* u) {
        if (t <= 2) static_cast<std::vector<std::string>*>(u)->push_back("b" + std::to_string(t));
    }, &hookTicks, 5);
    const int ta = melange::sim::AddTickHook([](uint32_t t, void* u) {
        if (t <= 2) static_cast<std::vector<std::string>*>(u)->push_back("a" + std::to_string(t));
    }, &hookTicks, -5);
    for (int i = 0; i < 200; ++i) {
        core::Update();
        if (melange::sim::Tick() % 50 == 0) {
            lua_pushstring(S, "SendMessage");
            lua_gettable(S, LUA_GLOBALSINDEX);
            lua_pushstring(S, "GameLogic.Turn.Ended");
            lua_pcall(S, 1, 0, 0);
        }
    }
    melange::sim::RemoveTickHook(th);
    melange::sim::RemoveTickHook(ta);
    Expect(hookTicks == std::vector<std::string>({"a1", "b1", "a2", "b2"}), "C++ tick hooks by order, every tick");
    Expect(melange::sim::Tick() == 200, "200 ticks");
    Expect(lua_gettop(S) == 0 && lua_gethookmask(S) == 0, "stack balanced and hook off after the ticks");
    Expect(fake::runState == 1 && fake::halts.empty(), "run state stays 1 through the match");
    RunResult res;
    res.modLog = g_modLog;

    if (full) {
        Expect(Storage("alpha", "ticks") == "200", "tick subscriber ran every tick");
        Expect(Storage("alpha", "order") ==
                   "{1=after2@2;2=after2b@2;3=after3@3;4=every@50;5=every@100;6=every@150;7=every@200;}",
               "timers by due tick then creation: " + Storage("alpha", "order"));
        Expect(Storage("alpha", "inTick") == "cb:alpha", "CurrentMod in a callback: " + Storage("alpha", "inTick"));
        Expect(Storage("alpha", "turns") == "4" && Storage("alpha", "selfPing") == "4", "forwarded message, 4 turns");
        Expect(Storage("beta", "pings") == "5" && Storage("beta", "lastPing") == "4", "mod message delivered to another mod");
        Expect(Storage("beta", "loops") == "3", "runaway handler disabled after 3 faults: " + Storage("beta", "loops"));
        Expect(Storage("gamma", "runs") == "3" && Storage("gamma", "after") == "nil",
               "pcall cannot hold the budget; timer disabled after 3 faults");
        Expect(GlobalsDigest().find("turnsSeen=4;") != std::string::npos, "level script handler still runs");
        Expect(melange::sim::GetStats().faults >= 6 + 3, "faults counted");

        const auto r1 = melange::sim::SendInt("Melange.Test.Ping", 99);
        Expect(r1 == melange::sim::SendResult::Ok && Storage("alpha", "selfPing") == "99", "C++ sim::SendInt");
        Expect(melange::sim::Send("Not.There.At.All") == melange::sim::SendResult::NotRegistered, "C++ NotRegistered");
        Expect(melange::sim::Send("GameLogic.PauseGame") == melange::sim::SendResult::Denied, "C++ Denied");
        melange::sim::SendInt("Melange.Test.Echo", 1);
        Expect(Storage("sender", "echo") == "8", "self-recursive send stops at depth 8: " + Storage("sender", "echo"));
        Expect(core::GetCounters().dispatchDepthDrops == 1, "one depth drop");
        melange::simbridge::Dispatch("sim.test", {1, 2});
        Expect(Storage("alpha", "dispatched") == "sim.test:1:2", "Dispatch reaches subscribers");
        Expect(lua_gettop(S) == 0 && fake::runState == 1, "stack and run state after C++ sends");

        auto e = melange::simbridge::EvalMatch("return 1+1");
        Expect(e.ok && e.text == "2", "EvalMatch expression: " + e.text);
        e = melange::simbridge::EvalMatch("print('hi', 3) x = 5");
        Expect(e.ok && e.text == "hi\t3\n", "EvalMatch print capture: " + e.text);
        e = melange::simbridge::EvalMatch("=x");
        Expect(e.ok && e.text == "5" && GlobalNil("x"), "console environment persists, no global: " + e.text);
        e = melange::simbridge::EvalMatch("return turnsSeen, mods.alpha ~= nil");
        Expect(e.ok && e.text == "4\ttrue", "console reads globals and mods[id]: " + e.text);
        e = melange::simbridge::EvalMatch("if then");
        Expect(!e.ok && !e.text.empty(), "syntax error reported: " + e.text);
        e = melange::simbridge::EvalMatch("while true do end");
        Expect(!e.ok && e.text.find("budget") != std::string::npos, "console loop stopped: " + e.text);
        std::vector<std::string> c;
        melange::simbridge::CompleteMatch("math.fl", &c);
        Expect(std::find(c.begin(), c.end(), "math.floor") != c.end(), "completion math.fl");
        c.clear();
        melange::simbridge::CompleteMatch("mo", &c);
        Expect(std::find(c.begin(), c.end(), "mods") != c.end(), "completion mo");
        c.clear();
        melange::simbridge::CompleteMatch("mods.al", &c);
        Expect(c == std::vector<std::string>({"mods.alpha"}), "completion mods.al");
        Expect(lua_gettop(S) == 0 && lua_gethookmask(S) == 0, "stack balanced and hook off after the console");

        const uint32_t a1 = melange::sim::Random(7), a2 = melange::sim::Random(7), b1 = melange::sim::Random(8);
        Expect(a1 != a2 && a1 != b1, "sim::Random streams advance and differ by key");
    }

    res.alphaDraws = Storage("alpha", "draws");
    const auto names = GlobalsDigest();
    Expect(names.find("leak") == std::string::npos && names.find("wum") == std::string::npos, "no mod global at the end");
    g_log.clear();
    core::ContextClosing(S);
    Expect(Logged("refs left") && Logged(" 0 refs left"), "every registry ref dropped at close");
    Expect(!melange::sim::ModsActive() && melange::simbridge::LoadedMods().empty(), "state cleared at close");
    lua_close(S);
    fake::L = nullptr;
    return res;
}

void TestRegistration() {
    const std::vector<std::string> vanilla = {"GameLogic", "Weapon"};
    uint16_t id = 0;
    Expect(core::RegisterModMessage("Melange.Test.Ping", &id, vanilla) && id == l5::Lookup("Melange.Test.Ping"),
           "register a mod message");
    Expect(core::RegisterModMessage("Melange.Test.Echo", &id, vanilla), "register a second one");
    Expect(core::RegisterModMessage("Melange.Sample.Ping", &id, vanilla), "register the sample's message");
    Expect(!core::RegisterModMessage("Melange.Test.Ping", &id, vanilla), "duplicate refused");
    Expect(!core::RegisterModMessage("GameLogic.Mine", &id, vanilla), "vanilla prefix refused");
    Expect(!core::RegisterModMessage("melange.lower", &id, vanilla), "lower-case first segment refused");
    Expect(!core::RegisterModMessage("NoDot", &id, vanilla), "no dot refused");
    Expect(!core::RegisterModMessage("A.b.c.d.e.f.g", &id, vanilla), "more than 5 dots refused");
    Expect(!core::RegisterModMessage("Mod.bad-char", &id, vanilla), "bad character refused");
    int ok = 3;
    for (int i = 0; i < 60; ++i) ok += core::RegisterModMessage(("Melange.Cap.N" + std::to_string(i)).c_str(), &id, vanilla);
    Expect(ok == 48, "48 names at most: " + std::to_string(ok));
    const auto msgs = melange::simbridge::ModMessages();
    Expect(msgs.size() == 48 && msgs[0].first == "Melange.Test.Ping" && msgs[3].first == "Melange.Cap.N0",
           "registration order kept");
    core::FreezeModMessages();
    Expect(core::ModMessagesFrozen(), "frozen");
}
}  // namespace

void TestSample() {
    std::string code;
    if (FILE* f = fopen(MELANGE_SOURCE_DIR "/dist/Mods/sim-sampler/sim/main.lua", "rb")) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) code.append(b, n);
        fclose(f);
    }
    Expect(!code.empty(), "sim-sampler source found");
    g_modLog.clear();
    fake::runState = 1;
    fake::L = fake::NewMatchVM();
    core::SetSources({{"sim-sampler", "1.0.0", "@sim-sampler/sim/main.lua", code}});
    core::ContextCreated(fake::L);
    Expect(core::Init({"GameLogic.Turn.Ended", "Weapon.Fired"}, true), "sim-sampler loads");
    for (int i = 0; i < 300; ++i) {
        core::Update();
        if (melange::sim::Tick() % 100 == 0) {
            lua_pushstring(fake::L, "SendMessage");
            lua_gettable(fake::L, LUA_GLOBALSINDEX);
            lua_pushstring(fake::L, i == 199 ? "Weapon.Fired" : "GameLogic.Turn.Ended");
            lua_pcall(fake::L, 1, 0, 0);
        }
    }
    auto has = [](const std::string& prefix) {
        return std::any_of(g_modLog.begin(), g_modLog.end(), [&](const std::string& s) { return s.rfind(prefix, 0) == 0; });
    };
    Expect(has("sim-sampler|1|0|sim-sampler 1.0.0 loaded at tick 0"), "sim-sampler: loaded");
    Expect(has("sim-sampler|1|100|turn 1 ended at tick 100 ticks this turn 100 draw "), "sim-sampler: turn log");
    Expect(has("sim-sampler|1|100|ping 1 received at tick 100"), "sim-sampler: ping");
    Expect(has("sim-sampler|1|200|weapon fired at tick 200"), "sim-sampler: weapon fired");
    Expect(has("sim-sampler|1|300|turn 2 ended at tick 300 ticks this turn 200 draw "), "sim-sampler: second turn");
    Expect(melange::sim::GetStats().faults == 0 && fake::runState == 1, "sim-sampler: no faults, run state 1");
    core::ContextClosing(fake::L);
    lua_close(fake::L);
    fake::L = nullptr;
}

// Mod hash contributors: the environment digest of every sim mod and wum.sim.hash.
const char kHashMod[] = R"(
counter = 0
handler = function() end
t = { a = 1, b = { c = "x", d = { e = true, f = { g = 1 } } } }
wum.events.on("sim.test.bump", function(ev, n) counter = counter + n end)
wum.events.on("sim.test.deep4", function() t.b.d.f.g = 2 end)
wum.events.on("sim.test.deep3", function() t.b.d.e = false end)
wum.events.on("sim.test.fn", function() handler = function() return 1 end end)
wum.events.on("sim.test.store", function() wum.sim.storage.turn = (wum.sim.storage.turn or 0) + 1 end)
wum.events.on("sim.test.hash", function(ev, v) wum.sim.hash(v, "s", true, nil) end)
wum.events.on("sim.test.badhash", function() bad = pcall(wum.sim.hash, {}) end)
wum.events.on("sim.test.zero", function() counter = 0 end)
wum.events.on("sim.test.negzero", function() counter = 0 * -1 end)
wum.events.on("sim.test.remove", function() counter = nil end)
)";
const char kOrder1[] = "x = 1 y = 'two' z = { p = 1, q = { 2, 3 } } w = true";
const char kOrder2[] = "w = true z = {} z.q = { 2, 3 } z.p = 1 y = 'two' x = 1";
const char kBig[] = R"(
wum.events.on("sim.test.grow", function(ev, k)
  local t = {} for i = 1, 20000 do t[i] = i end
  _G["big" .. k] = t
end)
)";

namespace ws = melange::wormsign;

std::map<std::string, uint64_t> Comps(uint32_t tick) {
    ws::contrib::Entry e[32];
    const size_t n = ws::contrib::HashesAt(tick, e, 32);
    std::map<std::string, uint64_t> m;
    for (size_t i = 0; i < n; ++i) m[e[i].name] = e[i].hash;
    return m;
}

std::vector<std::string> Changed(uint32_t a, uint32_t b) {
    const auto ma = Comps(a), mb = Comps(b);
    std::vector<std::string> v;
    for (auto& [k, h] : mb)
        if (!ma.count(k) || ma.at(k) != h) v.push_back(k);
    return v;
}

std::string ChangeText(uint32_t tick) {
    melange::simhash::EnvChange c[16];
    const size_t n = melange::simhash::EnvChanges(tick, tick, c, 16);
    std::string s;
    for (size_t i = 0; i < n; ++i) s += melange::simhash::Format(c[i]) + "\n";
    return s;
}

// Runs the scripted sequence and returns the mods hash of every tick.
std::vector<uint64_t> RunHashMatch(melange::simhash::EnvMode mode, bool checks) {
    melange::simhash::Install(mode);
    fake::runState = 1;
    fake::L = fake::NewMatchVM();
    lua_State* S = fake::L;
    core::SetSources({{"hashmod", "1.0.0", "@hashmod/sim.lua", kHashMod},
                      {"order1", "1.0.0", "@order1/sim.lua", kOrder1},
                      {"order2", "1.0.0", "@order2/sim.lua", kOrder2}});
    core::ContextCreated(S);
    Expect(core::Init({"GameLogic.Turn.Ended"}, true), "hash mods load");
    std::vector<uint64_t> out;
    uint32_t tick = 0;
    auto step = [&](const char* ev, std::vector<float> args = {}) {
        if (ev) melange::simbridge::Dispatch(ev, args);
        out.push_back(ws::contrib::HashTick(++tick));
        return tick;
    };
    const uint32_t t1 = step(nullptr);
    const uint32_t t2 = step(nullptr);
    const uint32_t tBump = step("sim.test.bump", {1});
    const uint32_t tDeep4 = step("sim.test.deep4");
    const uint32_t tDeep3 = step("sim.test.deep3");
    const uint32_t tFn = step("sim.test.fn");
    const uint32_t tStore = step("sim.test.store");
    const uint32_t tHash = step("sim.test.hash", {5});
    const uint32_t tAfter = step(nullptr);
    const uint32_t tHash2 = step("sim.test.hash", {6});
    const uint32_t tBad = step("sim.test.badhash");
    const uint32_t tZero = step("sim.test.zero");
    const uint32_t tNeg0 = step("sim.test.negzero");
    const uint32_t tRemove = step("sim.test.remove");
    if (checks) {
        ws::contrib::Info info[16];
        const size_t n = ws::contrib::List(info, 16);
        std::string names;
        for (size_t i = 0; i < n; ++i) names += std::string(info[i].name) + " ";
        Expect(names == "mod.hashmod.env mod.hashmod.hash mod.order1.env mod.order1.hash mod.order2.env mod.order2.hash ",
               "a contributor pair per sim mod, in name order: " + names);
        Expect(out[t1 - 1] && out[t1 - 1] == out[t2 - 1], "no change: same mods hash");
        Expect(Changed(t2, tBump) == std::vector<std::string>{"mod.hashmod.env"}, "a global change flags the env");
        Expect(ChangeText(tBump).find("mod.hashmod.env global counter: 0 -> 1") != std::string::npos,
               "the change names the key: " + ChangeText(tBump));
        Expect(out[tDeep4 - 1] == out[tBump - 1], "below depth 3: not hashed");
        Expect(Changed(tDeep4, tDeep3) == std::vector<std::string>{"mod.hashmod.env"}, "at depth 3: hashed");
        Expect(ChangeText(tDeep3).find("global t: table -> table") != std::string::npos, "nested change named at the top key");
        Expect(out[tFn - 1] == out[tDeep3 - 1], "functions are hashed by type only");
        Expect(Changed(tFn, tStore) == std::vector<std::string>{"mod.hashmod.env"}, "wum.sim.storage is hashed");
        Expect(ChangeText(tStore).find("storage.turn added: 1") != std::string::npos, "storage key named");
        Expect(Changed(tStore, tHash) == std::vector<std::string>{"mod.hashmod.hash"}, "wum.sim.hash feeds its contributor");
        Expect(Changed(tHash, tAfter) == std::vector<std::string>{"mod.hashmod.hash"} &&
                   Comps(tAfter)["mod.hashmod.hash"] == Comps(t1)["mod.hashmod.hash"],
               "wum.sim.hash counts for its tick only");
        Expect(Comps(tHash2)["mod.hashmod.hash"] != Comps(tHash)["mod.hashmod.hash"], "the values matter");
        Expect(Changed(tHash2, tBad) == std::vector<std::string>{"mod.hashmod.env", "mod.hashmod.hash"} ||
                   Changed(tHash2, tBad) == std::vector<std::string>{"mod.hashmod.hash", "mod.hashmod.env"},
               "a table argument raises (bad = false is a new global)");
        melange::simbridge::PushModEnv("hashmod");
        lua_pushstring(S, "bad");
        lua_rawget(S, -2);
        Expect(lua_isboolean(S, -1) && !lua_toboolean(S, -1), "wum.sim.hash({}) raised");
        lua_settop(S, 0);
        Expect(Changed(tZero, tNeg0) == std::vector<std::string>{"mod.hashmod.env"}, "-0 is hashed by its bits");
        Expect(ChangeText(tRemove).find("global counter removed (was -0)") != std::string::npos, "removal named");
        const auto c = Comps(t1);
        Expect(c.at("mod.order1.env") == c.at("mod.order2.env"), "the digest does not depend on insertion order");
        Expect(lua_gettop(S) == 0, "the digest leaves the stack as it was");
        Expect(melange::simhash::GetCost().digests > 0, "digest cost measured");
    }
    core::ContextClosing(S);
    lua_close(S);
    fake::L = nullptr;
    if (checks) Expect(ws::contrib::Count() == 0, "contributors removed at match end");
    return out;
}

void TestModHash() {
    const auto changed = RunHashMatch(melange::simhash::EnvMode::Changed, true);
    const auto always = RunHashMatch(melange::simhash::EnvMode::Always, false);
    Expect(changed == always, "digesting only after mod code ran gives the same hashes as every tick");

    fake::runState = 1;
    fake::L = fake::NewMatchVM();
    core::SetSources({{"big", "1.0.0", "@big/sim.lua", kBig}});
    core::ContextCreated(fake::L);
    Expect(core::Init({"GameLogic.Turn.Ended"}, true), "big mod loads");
    const uint64_t h0 = ws::contrib::HashTick(1);
    melange::simbridge::Dispatch("sim.test.grow", {1});
    const uint64_t h1 = ws::contrib::HashTick(2);
    melange::simbridge::Dispatch("sim.test.grow", {2});
    melange::simbridge::Dispatch("sim.test.grow", {3});
    const uint64_t h2 = ws::contrib::HashTick(3);
    melange::simbridge::Dispatch("sim.test.grow", {4});
    const uint64_t h3 = ws::contrib::HashTick(4);
    Expect(h0 != h1 && h1 != h2 && h2 == h3, "past the value cap the digest is a fixed marker");
    Expect(Logged("hold over 50000 values"), "the cap is logged");
    Expect(lua_gettop(fake::L) == 0, "stack clean after the cap");
    core::ContextClosing(fake::L);
    lua_close(fake::L);
    fake::L = nullptr;
    melange::simhash::Uninstall();
}

std::string Global(const char* mod, const char* name) {
    lua_State* S = fake::L;
    if (!melange::simbridge::PushModEnv(mod)) return "<no env>";
    lua_pushstring(S, name);
    lua_gettable(S, -2);
    std::string r = Scalar(S, -1);
    lua_settop(S, 0);
    return r;
}

// desync-probe runs clean until sim.test.desync, then changes a global (kind 0) or its random stream (kind 2).
std::vector<std::string> RunProbe(int kind, int atTick) {
    std::string code;
    if (FILE* f = fopen(MELANGE_SOURCE_DIR "/dist/Mods/desync-probe/sim/main.lua", "rb")) {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) code.append(b, n);
        fclose(f);
    }
    Expect(!code.empty(), "desync-probe source found");
    fake::runState = 1;
    fake::seed = 0x5eed;
    fake::L = fake::NewMatchVM();
    core::SetSources({{"desync-probe", "1.0.0", "@desync-probe/sim/main.lua", code}});
    core::ContextCreated(fake::L);
    Expect(core::Init({}, true), "desync-probe loads");
    std::vector<std::string> states;
    for (int i = 1; i <= 200; ++i) {
        core::Update();
        if (kind >= 0 && i == atTick) melange::simbridge::Dispatch("sim.test.desync", {static_cast<float>(kind)});
        states.push_back(Global("desync-probe", "desyncs") + "/" + Global("desync-probe", "lastDraw"));
    }
    Expect(melange::sim::GetStats().faults == 0 && fake::runState == 1, "desync-probe: no faults");
    core::ContextClosing(fake::L);
    lua_close(fake::L);
    fake::L = nullptr;
    return states;
}

void TestDesyncProbe() {
    const auto clean = RunProbe(-1, 0), clean2 = RunProbe(-1, 0);
    Expect(clean == clean2 && clean.back() != "0/0", "desync-probe: deterministic without the event");
    const auto env = RunProbe(0, 120), rng = RunProbe(2, 130);
    Expect(std::equal(clean.begin(), clean.begin() + 119, env.begin()) && env[119] != clean[119] &&
               env[119].rfind("1/", 0) == 0,
           "desync-probe env: equal before the tick, the global changes at it");
    Expect(std::equal(clean.begin(), clean.begin() + 129, rng.begin()) && rng[129] != clean[129] && rng[199] != clean[199],
           "desync-probe rng: equal before the tick, the stream differs from it on");
}

int main() {
    melange::log::SetTap(&Tap);
    {
        lua_State* S = lua_open();
        luaopen_string(S);
        luaopen_table(S);
        fake::CollectLib(S, "string", fake::strlib);
        fake::CollectLib(S, "table", fake::tablib);
    }
    for (const char* n : {"GameLogic.Turn.Ended", "GameLogic.Turn.Started", "GameLogic.PauseGame", "Weapon.Fired"})
        fake::Reg(n);
    melange::simbridge::AddSimFunction("probe", &ProbeFn);
    core::SetLogSink(&Sink);
    core::Config cfg;
    cfg.instrPerCall = 200000;
    core::Configure(cfg);

    TestRegistration();
    const RunResult a = RunMatch(0x1234abcd, true);
    const RunResult b = RunMatch(0x1234abcd, false);
    const RunResult c = RunMatch(0x0badf00d, false);
    Expect(a.modLog.size() > 10 && a.modLog == b.modLog, "same seed: identical mod log (tick, values)");
    Expect(a.alphaDraws == b.alphaDraws && a.alphaDraws != c.alphaDraws, "draws follow the logic seed: " + a.alphaDraws + " / " + c.alphaDraws);
    Expect(std::any_of(a.modLog.begin(), a.modLog.end(), [](const std::string& s) { return s.rfind("alpha|1|50|turn 1 draw", 0) == 0; }),
           "wum.log records carry the tick");

    // Gate closed: nothing loads and nothing is touched.
    fake::L = fake::NewMatchVM();
    const std::string before = GlobalsDigest();
    core::ContextCreated(fake::L);
    Expect(!core::Init({"GameLogic.Turn.Ended"}, false) && !core::Active(), "gate closed: no mods");
    core::Update();
    Expect(GlobalsDigest() == before && lua_gettop(fake::L) == 0, "gate closed: VM untouched");
    core::ContextClosing(fake::L);
    lua_close(fake::L);
    fake::L = nullptr;

    TestSample();
    TestModHash();
    TestDesyncProbe();
    printf("sim_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
