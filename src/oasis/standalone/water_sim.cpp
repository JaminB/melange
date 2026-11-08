#include "oasis/standalone/water_sim.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "tools/json_mini.h"

namespace melange::oasis::standalone {
namespace {
struct Timer {
    uint32_t id, due, period;   // period 0: once
    int ref;
    bool dead;
};
struct Schedule {
    bool on = false, hasFrom = false;
    uint32_t start = 1, every = 1, applied = 0;
    float step = 0, cap = 0, from = 0;
};
struct Sim {
    WaterSim o;
    float level = 0, rise = 0;
    uint32_t tick = 0, turns = 0, nextTimer = 1;
    long budget = 0;
    Schedule water;
    std::vector<Timer> timers;
    std::vector<int> onTurn;   // sim.turnStarted handlers (registry refs)
    jsonmini::Arr schedules, errors;
    std::map<std::string, int> calls;
    int errorCount = 0;
};
Sim* g_sim = nullptr;   // the run in progress, under g_mx
std::mutex g_mx;
constexpr long kBudget = 2000000;   // VM instructions for the main chunk, and for each turn

void Hook(lua_State* L, lua_Debug*) {
    if ((g_sim->budget -= 1000) < 0) luaL_error(L, "the script ran too long (an endless loop?)");
}

float Field(lua_State* L, const char* k, bool* has) {
    lua_pushstring(L, k);
    lua_rawget(L, 1);
    *has = lua_type(L, -1) == LUA_TNUMBER;
    const float v = *has ? static_cast<float>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    return v;
}

uint32_t Round(const Sim& s) {
    return s.turns ? (s.turns - 1) / static_cast<uint32_t>((std::max)(1, s.o.turnsPerRound)) + 1 : 0;
}

int LWaterSchedule(lua_State* L) {
    Sim& s = *g_sim;
    if (lua_gettop(L) == 0 || lua_isnil(L, 1)) {
        if (!s.water.on) return 0;
        lua_pushnumber(L, static_cast<lua_Number>(s.water.start));
        lua_pushnumber(L, s.water.cap);
        return 2;
    }
    if (lua_type(L, 1) == LUA_TBOOLEAN && !lua_toboolean(L, 1)) {
        lua_pushboolean(L, s.water.on);
        s.water.on = false;
        s.schedules.Raw(jsonmini::Obj().Int("turn", s.turns).Bool("stop", true).End());
        return 1;
    }
    if (!lua_istable(L, 1)) return luaL_error(L, "wum.sim.waterSchedule: expected an options table, false or nothing");
    float scale = 1;
    lua_pushstring(L, "units");
    lua_rawget(L, 1);
    if (!lua_isnil(L, -1)) {
        const char* u = lua_tostring(L, -1);
        if (!u || (std::strcmp(u, "map") != 0 && std::strcmp(u, "world") != 0))
            return luaL_error(L, "wum.sim.waterSchedule: units must be \"world\" or \"map\"");
        scale = std::strcmp(u, "map") == 0 ? 20.0f : 1.0f;
    }
    lua_pop(L, 1);
    bool hs = false, he = false, hst = false, hc = false, hf = false;
    const float start = Field(L, "startTurn", &hs), every = Field(L, "every", &he);
    const float step = Field(L, "step", &hst) * scale, cap = Field(L, "cap", &hc) * scale, from = Field(L, "from", &hf) * scale;
    if (!hst || !hc) return luaL_error(L, "wum.sim.waterSchedule: step and cap are required");
    const float st = hs ? start : 1, ev = he ? every : 1;
    if (st < 1 || std::floor(st) != st || ev < 1 || ev > 1000 || std::floor(ev) != ev || !(step > 0))
        return luaL_error(L, "wum.sim.waterSchedule: startTurn and every must be whole numbers of 1 or more, step above 0");
    s.water = Schedule{true, hf, static_cast<uint32_t>(st), static_cast<uint32_t>(ev), 0, step, cap, from};
    jsonmini::Obj j;
    j.Int("turn", s.turns).Int("startTurn", s.water.start).Int("every", s.water.every).Float("step", step).Float("cap", cap);
    if (hf) j.Float("from", from);
    s.schedules.Raw(j.End());
    lua_pushboolean(L, 1);
    return 1;
}

int LGetData(lua_State* L) {
    const char* k = luaL_checkstring(L, 1);
    if (std::strcmp(k, "Water.Level") == 0) lua_pushnumber(L, g_sim->level);
    else if (std::strcmp(k, "Water.RiseSpeed.Current") == 0) lua_pushnumber(L, g_sim->rise);
    else lua_pushnil(L);
    return 1;
}

int LSetData(lua_State* L) {
    const char* k = luaL_checkstring(L, 1);
    if (lua_type(L, 2) == LUA_TNUMBER) {
        if (std::strcmp(k, "Water.Level") == 0) g_sim->level = static_cast<float>(lua_tonumber(L, 2));
        else if (std::strcmp(k, "Water.RiseSpeed.Current") == 0) g_sim->rise = static_cast<float>(lua_tonumber(L, 2));
    }
    lua_pushboolean(L, 1);
    return 1;
}

int AddTimer(lua_State* L, bool repeat) {
    const lua_Number t = std::floor(luaL_checknumber(L, 1));
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (!(t >= 1)) return luaL_error(L, "wum.sim.%s: ticks must be 1 or more", repeat ? "every" : "after");
    lua_pushvalue(L, 2);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    Sim& s = *g_sim;
    const uint32_t n = static_cast<uint32_t>(t);
    s.timers.push_back(Timer{s.nextTimer, s.tick + n, repeat ? n : 0, ref, false});
    lua_pushnumber(L, static_cast<lua_Number>(s.nextTimer++));
    return 1;
}
int LAfter(lua_State* L) { return AddTimer(L, false); }
int LEvery(lua_State* L) { return AddTimer(L, true); }
int LCancel(lua_State* L) {
    const uint32_t id = static_cast<uint32_t>(luaL_checknumber(L, 1));
    bool found = false;
    for (auto& t : g_sim->timers)
        if (t.id == id && !t.dead) t.dead = found = true;
    lua_pushboolean(L, found);
    return 1;
}
int LTick(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(g_sim->tick));
    return 1;
}
int LTurn(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(g_sim->turns));
    return 1;
}
int LRound(lua_State* L) {
    lua_pushnumber(L, static_cast<lua_Number>(Round(*g_sim)));
    return 1;
}
int LRandom(lua_State* L) {
    static uint32_t st = 12345;
    st = st * 1664525u + 1013904223u;
    const double u = (st >> 8) / 16777216.0;
    const int n = lua_gettop(L);
    if (n == 0) {
        lua_pushnumber(L, static_cast<lua_Number>(u));
    } else {
        const double lo = n >= 2 ? lua_tonumber(L, 1) : 1, hi = n >= 2 ? lua_tonumber(L, 2) : lua_tonumber(L, 1);
        lua_pushnumber(L, static_cast<lua_Number>(std::floor(lo + u * (hi - lo + 1))));
    }
    return 1;
}
int LOn(lua_State* L) {
    const char* ev = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (std::strcmp(ev, "sim.turnStarted") == 0) {
        lua_pushvalue(L, 2);
        g_sim->onTurn.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    }
    lua_pushboolean(L, 1);
    return 1;
}
// Any other function: does nothing, answers true, counted by name.
int LNothing(lua_State* L) {
    ++g_sim->calls[lua_tostring(L, lua_upvalueindex(1))];
    lua_pushboolean(L, 1);
    return 1;
}
// __index of the stand-in tables: a do-nothing function named after the key.
int LIndex(lua_State* L) {
    const char* table = lua_tostring(L, lua_upvalueindex(1));
    const char* key = lua_tostring(L, 2);
    const std::string name = std::string(table ? table : "wum") + "." + (key ? key : "?");
    lua_pushstring(L, name.c_str());
    lua_pushcclosure(L, &LNothing, 1);
    return 1;
}

void Fn(lua_State* L, const char* name, lua_CFunction f) {
    lua_pushstring(L, name);
    lua_pushcclosure(L, f, 0);
    lua_rawset(L, -3);
}
// Pushes a table whose unknown keys are do-nothing functions.
void Table(lua_State* L, const char* name) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushstring(L, "__index");
    lua_pushstring(L, name);
    lua_pushcclosure(L, &LIndex, 1);
    lua_rawset(L, -3);
    lua_setmetatable(L, -2);
}

void Raised(lua_State* L, const char* where) {
    Sim& s = *g_sim;
    if (++s.errorCount <= 20)
        s.errors.Raw(jsonmini::Obj().Int("turn", s.turns).Str("in", where)
                         .Str("error", lua_isstring(L, -1) ? lua_tostring(L, -1) : "?").End());
    lua_pop(L, 1);
}

// Calls the registry function `ref` with the `nargs` values on the stack.
void Call(lua_State* L, int ref, int nargs, const char* where) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (nargs) lua_insert(L, -1 - nargs);
    if (lua_pcall(L, nargs, 0, 0)) Raised(L, where);
}

// The schedule's step at a turn start, as NoteTurnStart's (lua/sim/sim_events.cpp).
void StepWater(Sim& s) {
    Schedule& w = s.water;
    if (!w.on || s.turns < w.start || (s.turns - w.start) % w.every) return;
    const float cur = s.level;
    const float target = (std::min)(w.cap, w.hasFrom ? w.from + w.step * static_cast<float>(w.applied) : cur + w.step);
    ++w.applied;
    if (target > cur) s.level = target;
    if ((std::max)(cur, target) >= w.cap) w.on = false;
}

void Install(lua_State* L) {
    lua_pushstring(L, "wum");
    Table(L, "wum");
    lua_pushstring(L, "log");
    Table(L, "wum.log");
    lua_rawset(L, -3);
    lua_pushstring(L, "events");
    Table(L, "wum.events");
    Fn(L, "on", &LOn);
    lua_rawset(L, -3);
    lua_pushstring(L, "level");
    Table(L, "wum.level");
    lua_pushstring(L, "knots");
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_rawset(L, -3);
    lua_pushstring(L, "mod");
    lua_newtable(L);
    lua_rawset(L, -3);
    lua_pushstring(L, "sim");
    Table(L, "wum.sim");
    Fn(L, "waterSchedule", &LWaterSchedule);
    Fn(L, "getData", &LGetData);
    Fn(L, "setData", &LSetData);
    Fn(L, "after", &LAfter);
    Fn(L, "every", &LEvery);
    Fn(L, "cancel", &LCancel);
    Fn(L, "tick", &LTick);
    Fn(L, "turn", &LTurn);
    Fn(L, "round", &LRound);
    Fn(L, "random", &LRandom);
    lua_rawset(L, -3);
    lua_settable(L, LUA_GLOBALSINDEX);
}
}  // namespace

bool SimulateWater(const std::string& text, const WaterSim& o, std::string* json, std::string* err) {
    std::lock_guard lk(g_mx);
    Sim s;
    s.o = o;
    s.level = o.start;
    g_sim = &s;
    lua_State* L = lua_open();
    if (!L) {
        g_sim = nullptr;
        if (err) *err = "out of memory";
        return false;
    }
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    luaopen_math(L);
    lua_settop(L, 0);
    Install(L);
    lua_sethook(L, &Hook, LUA_MASKCOUNT, 1000);
    s.budget = kBudget;
    if (luaL_loadbuffer(L, text.data(), text.size(), "=script.lua")) {
        if (err) *err = lua_isstring(L, -1) ? lua_tostring(L, -1) : "the script does not compile";
        lua_close(L);
        g_sim = nullptr;
        return false;
    }
    if (lua_pcall(L, 0, 0, 0)) Raised(L, "the main chunk");
    jsonmini::Arr levels;
    const uint32_t per = static_cast<uint32_t>((std::max)(1, o.ticksPerTurn));
    for (int turn = 1; turn <= o.turns; ++turn) {
        s.budget = kBudget;
        float atStart = s.level;
        for (uint32_t k = 0; k < per; ++k) {
            ++s.tick;
            if (k == 0) {
                ++s.turns;
                StepWater(s);
                const std::vector<int> handlers = s.onTurn;
                for (const int ref : handlers) {
                    lua_pushstring(L, "sim.turnStarted");
                    lua_pushnumber(L, static_cast<lua_Number>(s.turns));
                    lua_pushnumber(L, static_cast<lua_Number>(Round(s)));
                    Call(L, ref, 3, "sim.turnStarted");
                }
            }
            for (size_t i = 0; i < s.timers.size(); ++i) {
                const Timer t = s.timers[i];
                if (t.dead || t.due != s.tick) continue;
                if (t.period) s.timers[i].due += t.period;
                else s.timers[i].dead = true;
                Call(L, t.ref, 0, t.period ? "wum.sim.every" : "wum.sim.after");
            }
            if (k == 0) atStart = s.level;
        }
        levels.Raw(jsonmini::Obj().Int("turn", turn).Float("world", atStart).Float("end", s.level).End());
        if (s.rise > 0) s.level += s.rise;
    }
    lua_close(L);
    g_sim = nullptr;
    jsonmini::Obj calls;
    for (const auto& [name, n] : s.calls) calls.Int(name, n);
    if (json)
        *json = jsonmini::Obj().Raw("levels", levels.End()).Raw("schedules", s.schedules.End()).Raw("errors", s.errors.End())
                    .Int("errorCount", s.errorCount).Raw("calls", calls.End()).Int("ticksPerTurn", per).End();
    return true;
}
}  // namespace melange::oasis::standalone
