// wum.input: control groups, key bindings and mouse look/aim options. The work is done by src/input/controls.cpp.
#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include "input/controls.h"
#include "lua/sandbox_core.h"

namespace melange::sandbox {
namespace {
namespace ctl = melange::controls;

char g_consoleOwner;       // owner token of the console environment (no mod)
std::set<Gen*> g_hooked;   // generations whose teardown clears their options

int Unavailable(lua_State* L) {
    lua_pushnil(L);
    lua_pushliteral(L, "unavailable");
    return 2;
}

int Groups(lua_State* L) {
    std::vector<std::string> names;
    if (!ctl::Available() || !ctl::ActiveGroups(&names)) return Unavailable(L);
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (size_t i = 0; i < names.size(); ++i) {
        lua_pushstring(L, names[i].c_str());
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int Binding(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::string label;
    if (ctl::Available() && ctl::Binding(name, &label))
        lua_pushstring(L, label.c_str());
    else
        lua_pushnil(L);
    return 1;
}

bool GetMode(lua_State* L, int t, const char* key, ctl::InvertMode* out) {
    lua_getfield(L, t, key);
    bool ok = true;
    if (!lua_isnil(L, -1)) {
        const char* s = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        ok = ctl::ParseInvertMode(s, out);
    }
    lua_pop(L, 1);
    return ok;
}

void GetSens(lua_State* L, int t, const char* key, float* out) {
    lua_getfield(L, t, key);
    if (!lua_isnil(L, -1)) {
        if (!lua_isnumber(L, -1)) luaL_error(L, "wum.input.setOptions: %s must be a number", key);
        *out = ctl::ClampSensitivity(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
}

int SetOptions(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        ctl::ClearAllOptions();
        lua_pushboolean(L, 1);
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    ctl::Options o;
    if (!GetMode(L, 1, "cameraInvertY", &o.cameraInvertY))
        return luaL_error(L, "wum.input.setOptions: cameraInvertY must be \"game\", \"standard\" or \"inverted\"");
    if (!GetMode(L, 1, "aimInvertY", &o.aimInvertY))
        return luaL_error(L, "wum.input.setOptions: aimInvertY must be \"game\", \"standard\" or \"inverted\"");
    lua_getfield(L, 1, "blimpInvert");
    if (!lua_isnil(L, -1)) {
        if (!lua_isboolean(L, -1)) return luaL_error(L, "wum.input.setOptions: blimpInvert must be a boolean");
        o.blimpInvert = lua_toboolean(L, -1) != 0;
    }
    lua_pop(L, 1);
    GetSens(L, 1, "cameraSensitivity", &o.cameraSensitivity);
    GetSens(L, 1, "aimSensitivity", &o.aimSensitivity);

    Gen* g = CurrentGen();
    const void* owner = g ? static_cast<const void*>(g) : static_cast<const void*>(&g_consoleOwner);
    ctl::SetOptions(o, owner);
    if (g && g_hooked.insert(g).second)
        g->cleanups.push_back([g] {
            g_hooked.erase(g);
            ctl::ClearOptions(g);
        });
    lua_pushboolean(L, 1);
    return 1;
}

int Options(lua_State* L) {
    const ctl::Options o = ctl::Effective();
    lua_createtable(L, 0, 6);
    lua_pushstring(L, ctl::InvertModeName(o.cameraInvertY));
    lua_setfield(L, -2, "cameraInvertY");
    lua_pushstring(L, ctl::InvertModeName(o.aimInvertY));
    lua_setfield(L, -2, "aimInvertY");
    lua_pushboolean(L, o.blimpInvert);
    lua_setfield(L, -2, "blimpInvert");
    lua_pushnumber(L, o.cameraSensitivity);
    lua_setfield(L, -2, "cameraSensitivity");
    lua_pushnumber(L, o.aimSensitivity);
    lua_setfield(L, -2, "aimSensitivity");
    lua_pushboolean(L, ctl::SmoothMouse());
    lua_setfield(L, -2, "smoothMouse");
    return 1;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kInput[] = {{"groups", Groups},
                                      {"binding", Binding},
                                      {"setOptions", SetOptions},
                                      {"options", Options},
                                      {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kInput);
    lua_setfield(L, wum, "input");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
