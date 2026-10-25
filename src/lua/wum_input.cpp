// wum.input: control groups, key bindings and mouse look/aim options (the work is done by src/input/controls.cpp), and
// Melange 0.12's mod input: regions, pointer mode and keyboard focus (src/render/input_regions_core.cpp).
#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "input/controls.h"
#include "lua/sandbox_core.h"
#include "render/input_regions.h"

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
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "wum.input.setOptions: %s must be a number", key);
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

// ---- mod input (Melange 0.12): cursor, regions, pointer mode, focus (render/input_regions.h). Events are owner-only
// and come from the window procedure; the calls here only claim and query.
namespace mi = melange::render::modinput;
std::set<Gen*> g_inputHooked;  // generations whose teardown forgets their regions, pointer and focus

void PushResult(lua_State* L, mi::Result r) {
    if (r == mi::Result::Ok) {
        lua_pushboolean(L, 1);
        return;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, r == mi::Result::Busy ? "busy" : "unavailable");
}

int NotForConsole(lua_State* L) {
    lua_pushboolean(L, 0);
    lua_pushliteral(L, "unavailable");
    return 2;
}

// A failed hot reload revokes the new generation while the old one keeps running: only the mod's current (or last)
// generation forgets the mod's input.
void HookInputCleanup(ModRec* m, Gen* g) {
    if (!g || !g_inputHooked.insert(g).second) return;
    g->cleanups.push_back([m, g] {
        g_inputHooked.erase(g);
        if (!m->gen || m->gen == g) mi::Forget(m->id);
    });
}

int Cursor(lua_State* L) {
    ModRec* m = Current();
    float x = 0, y = 0;
    const char* source = nullptr;
    if (!m || !mi::Cursor(m->id, &x, &y, &source)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushstring(L, source);
    return 3;
}

int Region(lua_State* L) {
    size_t n = 0;
    const char* id = luaL_checklstring(L, 1, &n);
    if (n == 0 || n > 64) return luaL_argerror(L, 1, "region id of 1 to 64 bytes expected");
    const float x0 = static_cast<float>(luaL_checknumber(L, 2)), y0 = static_cast<float>(luaL_checknumber(L, 3));
    const float x1 = static_cast<float>(luaL_checknumber(L, 4)), y1 = static_cast<float>(luaL_checknumber(L, 5));
    int frames = 2;
    mi::Shape shape = mi::Shape::Arrow;
    if (!lua_isnoneornil(L, 6)) {
        luaL_checktype(L, 6, LUA_TTABLE);
        lua_getfield(L, 6, "frames");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1)) return luaL_error(L, "wum.input.region: frames must be an integer");
            const lua_Integer f = lua_tointeger(L, -1);
            frames = f < 1 ? 1 : f > 600 ? 600 : static_cast<int>(f);
        }
        lua_pop(L, 1);
        lua_getfield(L, 6, "cursor");
        if (!lua_isnil(L, -1)) {
            const char* c = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
            if (!strcmp(c, "arrow"))
                shape = mi::Shape::Arrow;
            else if (!strcmp(c, "hand"))
                shape = mi::Shape::Hand;
            else if (!strcmp(c, "ibeam"))
                shape = mi::Shape::IBeam;
            else
                return luaL_error(L, "wum.input.region: cursor must be \"arrow\", \"hand\" or \"ibeam\"");
        }
        lua_pop(L, 1);
    }
    ModRec* m = Current();
    if (!m) return NotForConsole(L);
    HookInputCleanup(m, CurrentGen());
    const mi::Result r = mi::Region(m->id, m->order, std::string(id, n), x0, y0, x1, y1, frames, shape);
    PushResult(L, r);
    return r == mi::Result::Ok ? 1 : 2;
}

int Hover(lua_State* L) {
    ModRec* m = Current();
    const std::string id = m ? mi::Hover(m->id) : std::string();
    if (id.empty())
        lua_pushnil(L);
    else
        lua_pushlstring(L, id.data(), id.size());
    return 1;
}

int Pointer(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    const bool on = lua_toboolean(L, 1) != 0;
    bool clickOutside = true;
    unsigned texture = 0;
    Gen* g = CurrentGen();
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        lua_getfield(L, 2, "release");
        if (!lua_isnil(L, -1)) {
            const char* r = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
            if (!strcmp(r, "manual"))
                clickOutside = false;
            else if (strcmp(r, "click-outside") != 0)
                return luaL_error(L, "wum.input.pointer: release must be \"click-outside\" or \"manual\"");
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "cursor");
        if (!lua_isnil(L, -1)) {
            const lua_Integer t = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0;
            if (t <= 0 || !g || std::find(g->textures.begin(), g->textures.end(), static_cast<unsigned>(t)) == g->textures.end())
                return luaL_error(L, "wum.input.pointer: cursor must be a texture of this mod (wum.draw.texture)");
            texture = static_cast<unsigned>(t);
        }
        lua_pop(L, 1);
    }
    ModRec* m = Current();
    if (!m) return NotForConsole(L);
    HookInputCleanup(m, g);
    const mi::Result r = mi::Pointer(m->id, on, clickOutside, texture);
    PushResult(L, r);
    return r == mi::Result::Ok ? 1 : 2;
}

int PointerActive(lua_State* L) {
    ModRec* m = Current();
    lua_pushboolean(L, m && mi::PointerActive(m->id));
    return 1;
}

int Focus(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    const bool on = lua_toboolean(L, 1) != 0;
    ModRec* m = Current();
    if (!m) return NotForConsole(L);
    HookInputCleanup(m, CurrentGen());
    const mi::Result r = mi::Focus(m->id, on);
    PushResult(L, r);
    return r == mi::Result::Ok ? 1 : 2;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kInput[] = {{"groups", Groups},
                                      {"binding", Binding},
                                      {"setOptions", SetOptions},
                                      {"options", Options},
                                      {"cursor", Cursor},
                                      {"region", Region},
                                      {"hover", Hover},
                                      {"pointer", Pointer},
                                      {"pointerActive", PointerActive},
                                      {"focus", Focus},
                                      {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kInput);
    lua_setfield(L, wum, "input");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
