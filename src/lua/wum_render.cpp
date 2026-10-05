// wum.draw, wum.render, wum.postfx, wum.graphics and wum.shaders.
#include <algorithm>
#include <cstring>
#include <memory>

#include "lua/sandbox_core.h"
#include "melange/draw.h"
#include "melange/graphics.h"
#include "melange/postfx.h"
#include "melange/render.h"
#include "melange/shaders.h"

namespace melange::sandbox {
namespace {
void* IdPtr(uint32_t id) { return reinterpret_cast<void*>(static_cast<uintptr_t>(id)); }

float Field(lua_State* L, int t, const char* name, int index, float def = 0) {
    if (lua_getfield(L, t, name) == LUA_TNIL) {
        lua_pop(L, 1);
        lua_rawgeti(L, t, index);
    }
    const float v = static_cast<float>(luaL_optnumber(L, -1, def));
    lua_pop(L, 1);
    return v;
}

void CheckVec(lua_State* L, int idx, float out[3]) {
    luaL_checktype(L, idx, LUA_TTABLE);
    idx = lua_absindex(L, idx);
    out[0] = Field(L, idx, "x", 1);
    out[1] = Field(L, idx, "y", 2);
    out[2] = Field(L, idx, "z", 3);
}

void PushVec(lua_State* L, const float v[3]) {
    lua_createtable(L, 0, 3);
    lua_pushnumber(L, v[0]);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, v[1]);
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, v[2]);
    lua_setfield(L, -2, "z");
}

uint32_t Pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

uint32_t Unit(float v) { return static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); }

// Colours: 0xRRGGBBAA, "#RRGGBB[AA]", or {r, g, b[, a]} in 0..1. Returned as draw::Rgba (0xAABBGGRR).
draw::Rgba CheckColor(lua_State* L, int idx, draw::Rgba def) {
    switch (lua_type(L, idx)) {
        case LUA_TNONE:
        case LUA_TNIL: return def;
        case LUA_TNUMBER: {
            const auto v = static_cast<uint32_t>(luaL_checkinteger(L, idx));
            return Pack(v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
        }
        case LUA_TSTRING: {
            const char* s = lua_tostring(L, idx);
            const size_t n = strlen(s);
            if (s[0] != '#' || (n != 7 && n != 9)) luaL_argerror(L, idx, "colour string must be #RRGGBB or #RRGGBBAA");
            const auto v = static_cast<uint32_t>(strtoul(s + 1, nullptr, 16));
            return n == 7 ? Pack(v >> 16, (v >> 8) & 255, v & 255, 255) : Pack(v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
        }
        case LUA_TTABLE: {
            idx = lua_absindex(L, idx);
            return Pack(Unit(Field(L, idx, "r", 1)), Unit(Field(L, idx, "g", 2)), Unit(Field(L, idx, "b", 3)),
                        Unit(Field(L, idx, "a", 4, 1.f)));
        }
        default: luaL_argerror(L, idx, "colour expected"); return def;
    }
}

int Frames(lua_State* L, int idx) { return static_cast<int>(std::clamp<lua_Integer>(luaL_optinteger(L, idx, 1), 1, 600)); }

// ---------------------------------------------------------------- wum.draw
int Line(lua_State* L) {
    float a[3], b[3];
    CheckVec(L, 1, a);
    CheckVec(L, 2, b);
    draw::Line(a, b, CheckColor(L, 3, 0xffffffff), static_cast<float>(luaL_optnumber(L, 4, 2)), draw::kDefaultWorld, Frames(L, 5));
    return 0;
}

int Box(lua_State* L) {
    float a[3], b[3];
    CheckVec(L, 1, a);
    CheckVec(L, 2, b);
    draw::Box(a, b, CheckColor(L, 3, 0xffffffff), static_cast<float>(luaL_optnumber(L, 4, 2)), draw::kDefaultWorld, Frames(L, 5));
    return 0;
}

int Sphere(lua_State* L) {
    float c[3];
    CheckVec(L, 1, c);
    draw::Sphere(c, static_cast<float>(luaL_checknumber(L, 2)), CheckColor(L, 3, 0xffffffff),
                 static_cast<float>(luaL_optnumber(L, 4, 2)), draw::kDefaultWorld, Frames(L, 5));
    return 0;
}

int Axes(lua_State* L) {
    float o[3];
    CheckVec(L, 1, o);
    draw::Axes(o, static_cast<float>(luaL_optnumber(L, 2, 50)), static_cast<float>(luaL_optnumber(L, 3, 2)), draw::kDefaultWorld,
               Frames(L, 4));
    return 0;
}

int Quad(lua_State* L) {
    float p[4][3];
    for (int i = 0; i < 4; ++i) CheckVec(L, i + 1, p[i]);
    draw::Quad(p, CheckColor(L, 5, 0xffffffff), draw::kDefaultWorld, Frames(L, 6));
    return 0;
}

int Text(lua_State* L) {
    float a[3];
    CheckVec(L, 1, a);
    draw::Text(a, luaL_checkstring(L, 2), CheckColor(L, 3, 0xffffffff), static_cast<float>(luaL_optnumber(L, 4, 16)),
               draw::kDefaultWorld | draw::kScreenSize, Frames(L, 5));
    return 0;
}

float Num(lua_State* L, int idx) { return static_cast<float>(luaL_checknumber(L, idx)); }

int HudLine(lua_State* L) {
    draw::HudLine(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), CheckColor(L, 5, 0xffffffff),
                  static_cast<float>(luaL_optnumber(L, 6, 1)), Frames(L, 7));
    return 0;
}

int HudRect(lua_State* L) {
    draw::HudRect(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), CheckColor(L, 5, 0xffffffff), lua_toboolean(L, 6) != 0,
                  static_cast<float>(luaL_optnumber(L, 7, 1)), Frames(L, 8));
    return 0;
}

int HudText(lua_State* L) {
    draw::HudText(Num(L, 1), Num(L, 2), luaL_checkstring(L, 3), CheckColor(L, 4, 0xffffffff),
                  static_cast<float>(luaL_optnumber(L, 5, 16)), Frames(L, 6));
    return 0;
}

int HudImage(lua_State* L) {
    const auto tex = static_cast<unsigned>(luaL_checkinteger(L, 5));
    draw::HudImage(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), tex, CheckColor(L, 6, 0xffffffff), Frames(L, 7));
    return 0;
}

constexpr int kMaxTexturesPerGen = 256;

int Texture(lua_State* L) {
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.draw.texture needs a mod context");
    if (g->textureCount >= kMaxTexturesPerGen) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many textures loaded");
        return 2;
    }
    const std::string rel = luaL_checkstring(L, 1);
    if (!SafeRelPath(rel)) return luaL_argerror(L, 1, "relative path inside the mod folder expected");
    std::wstring w = Widen(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    const unsigned tex = draw::LoadTexture((m->dir + L"\\" + w).c_str());
    if (!tex) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load %s", rel.c_str());
        return 2;
    }
    ++g->textureCount;
    g->cleanups.push_back([tex] { draw::FreeTexture(tex); });
    lua_pushinteger(L, tex);
    return 1;
}

bool ParseStage(const std::string& s, render::Stage* out) {
    if (s == "world") *out = render::Stage::World;
    else if (s == "worldLate") *out = render::Stage::WorldLate;
    else if (s == "hud") *out = render::Stage::Hud;
    else return false;
    return true;
}

const char* StageName(render::Stage s) {
    switch (s) {
        case render::Stage::World: return "world";
        case render::Stage::WorldLate: return "worldLate";
        case render::Stage::PostWorld: return "postWorld";
        case render::Stage::Hud: return "hud";
        case render::Stage::Final: return "final";
        default: return "?";
    }
}

void DrawTramp(render::Stage stage, void* user) {
    Callback* cb = FindCallback(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user)));
    if (!cb) return;
    Invoke(cb, [stage](lua_State* L) {
        lua_pushstring(L, StageName(stage));
        return 1;
    });
}

int On(lua_State* L) {
    render::Stage stage;
    const std::string s = luaL_checkstring(L, 1);
    if (!ParseStage(s, &stage)) return luaL_argerror(L, 1, "stage must be world, worldLate or hud");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    Callback* cb = NewCallback(L, 2, CbKind::Draw, s);
    const uint32_t cid = cb->id;
    auto handle = std::make_shared<int>(0);
    cb->attach = [stage, cid, handle] {
        *handle = draw::AddDrawCallback(stage, &DrawTramp, IdPtr(cid));
        return *handle != 0;
    };
    cb->revoke = [handle] { draw::RemoveDrawCallback(*handle); };
    if (!Activate(cb)) return luaL_error(L, "wum.draw.on: registration failed");
    lua_pushinteger(L, cid);
    return 1;
}

int Off(lua_State* L) {
    const lua_Integer h = luaL_checkinteger(L, 1);
    Callback* cb = h > 0 ? FindCallback(static_cast<uint32_t>(h)) : nullptr;
    const bool ok = cb && cb->kind == CbKind::Draw && cb->gen->mod == Current();
    if (ok) KillCallback(cb->id);
    lua_pushboolean(L, ok);
    return 1;
}

// ---------------------------------------------------------------- wum.render
int Camera(lua_State* L) {
    render::Camera c{};
    if (!render::GetCamera(&c) || !c.valid) return 0;
    lua_createtable(L, 0, 5);
    PushVec(L, c.pos);
    lua_setfield(L, -2, "pos");
    PushVec(L, c.fwd);
    lua_setfield(L, -2, "fwd");
    PushVec(L, c.up);
    lua_setfield(L, -2, "up");
    lua_pushnumber(L, c.nearZ);
    lua_setfield(L, -2, "near");
    lua_pushnumber(L, c.farZ);
    lua_setfield(L, -2, "far");
    return 1;
}

int WorldToScreen(lua_State* L) {
    float v[3], x, y, d;
    CheckVec(L, 1, v);
    if (!render::WorldToScreen(v, &x, &y, &d)) return 0;
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, d);
    return 3;
}

int WindowSize(lua_State* L) {
    int w = 0, h = 0;
    render::WindowSize(&w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

int Timing(lua_State* L) {
    const render::Timing t = render::GetTiming();
    lua_createtable(L, 0, 5);
    lua_pushnumber(L, t.busyMsP50);
    lua_setfield(L, -2, "busyMsP50");
    lua_pushnumber(L, t.busyMsP95);
    lua_setfield(L, -2, "busyMsP95");
    lua_pushnumber(L, t.frameMsP50);
    lua_setfield(L, -2, "frameMsP50");
    lua_pushnumber(L, t.fps);
    lua_setfield(L, -2, "fps");
    lua_pushinteger(L, static_cast<lua_Integer>(t.frames));
    lua_setfield(L, -2, "frames");
    return 1;
}

// ---------------------------------------------------------------- wum.postfx
bool Own(const char* id) {
    ModRec* m = Current();
    if (!m) return false;
    const std::string prefix = m->id + "/";
    return strncmp(id, prefix.c_str(), prefix.size()) == 0;
}

void CheckOwn(lua_State* L, const char* id, const char* fn) {
    if (!Own(id)) luaL_error(L, "wum.postfx.%s: '%s' is not one of this mod's effects", fn, id);
}

int PfxList(lua_State* L) {
    postfx::EffectInfo buf[128];
    const size_t n = postfx::ListEffects(buf, 128);
    lua_createtable(L, static_cast<int>(n), 0);
    for (size_t i = 0; i < n && i < 128; ++i) {
        const postfx::EffectInfo& e = buf[i];
        lua_createtable(L, 0, 7);
        lua_pushstring(L, e.id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, e.title ? e.title : "");
        lua_setfield(L, -2, "title");
        lua_pushstring(L, StageName(e.stage));
        lua_setfield(L, -2, "stage");
        lua_pushinteger(L, e.order);
        lua_setfield(L, -2, "order");
        lua_pushboolean(L, e.enabled);
        lua_setfield(L, -2, "enabled");
        lua_pushboolean(L, e.failed);
        lua_setfield(L, -2, "failed");
        lua_pushboolean(L, Own(e.id));
        lua_setfield(L, -2, "own");
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int PfxEnable(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    CheckOwn(L, id, "enable");
    lua_pushboolean(L, postfx::SetEnabled(id, lua_toboolean(L, 2) != 0));
    return 1;
}

int PfxSetParamImpl(lua_State* L, const char* fn, bool persist) {
    const char* id = luaL_checkstring(L, 1);
    const char* param = luaL_checkstring(L, 2);
    CheckOwn(L, id, fn);
    float v[16];
    int n = 0;
    if (lua_istable(L, 3)) {
        const lua_Integer len = std::min<lua_Integer>(luaL_len(L, 3), 16);
        for (lua_Integer i = 1; i <= len; ++i) {
            lua_geti(L, 3, i);
            v[n++] = static_cast<float>(luaL_checknumber(L, -1));
            lua_pop(L, 1);
        }
    } else {
        for (int i = 3; i <= lua_gettop(L) && n < 16; ++i) v[n++] = static_cast<float>(luaL_checknumber(L, i));
    }
    if (n == 0) return luaL_error(L, "wum.postfx.%s: no values", fn);
    lua_pushboolean(L, persist ? postfx::SetParam(id, param, v, n) : postfx::SetParamTransient(id, param, v, n));
    return 1;
}

int PfxSetParam(lua_State* L) { return PfxSetParamImpl(L, "setParam", true); }

int PfxSetTransient(lua_State* L) { return PfxSetParamImpl(L, "setTransient", false); }

int PfxGetParam(lua_State* L) {
    const char* id = luaL_checkstring(L, 1);
    const char* param = luaL_checkstring(L, 2);
    const int n = static_cast<int>(std::clamp<lua_Integer>(luaL_optinteger(L, 3, 1), 1, 16));
    float v[16] = {};
    if (!postfx::GetParam(id, param, v, n)) return 0;
    for (int i = 0; i < n; ++i) lua_pushnumber(L, v[i]);
    return n;
}

// ---------------------------------------------------------------- wum.graphics / wum.shaders
int GfxSetShadowMapSize(lua_State* L) {
    ModRec* m = Current();
    if (!m) return luaL_error(L, "wum.graphics.setShadowMapSize: no current mod");
    int size = -1;
    if (!lua_isnoneornil(L, 1)) {
        lua_Integer v = luaL_checkinteger(L, 1);
        if (v != 0 && v != 512 && v != 1024 && v != 2048 && v != 4096)
            return luaL_error(L, "wum.graphics.setShadowMapSize: size must be 512, 1024, 2048, 4096, 0 or nil");
        size = static_cast<int>(v);
    }
    lua_pushboolean(L, graphics::SetShadowMapRequest(m->id.c_str(), size));
    return 1;
}

int GfxShadowMap(lua_State* L) {
    const graphics::ShadowMapInfo i = graphics::GetShadowMapInfo();
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, i.size);
    lua_setfield(L, -2, "size");
    lua_pushinteger(L, i.effective);
    lua_setfield(L, -2, "effective");
    lua_pushinteger(L, i.vanilla);
    lua_setfield(L, -2, "vanilla");
    lua_pushboolean(L, i.modRequest);
    lua_setfield(L, -2, "modRequest");
    lua_pushboolean(L, i.available);
    lua_setfield(L, -2, "available");
    return 1;
}

int GfxSetSupersample(lua_State* L) {
    ModRec* m = Current();
    if (!m) return luaL_error(L, "wum.graphics.setSupersample: no current mod");
    int samples = 0;
    if (!lua_isnoneornil(L, 1)) {
        lua_Integer v = luaL_checkinteger(L, 1);
        if (v != 0 && v != 1 && v != 2 && v != 4)
            return luaL_error(L, "wum.graphics.setSupersample: samples must be 2, 4, 0 or nil");
        samples = v <= 1 ? 0 : static_cast<int>(v);
    }
    lua_pushboolean(L, graphics::SetSupersampleRequest(m->id.c_str(), samples));
    return 1;
}

int GfxSupersample(lua_State* L) {
    const graphics::SupersampleInfo i = graphics::GetSupersampleInfo();
    lua_createtable(L, 0, 8);
    auto num = [&](const char* k, int v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, k);
    };
    auto flag = [&](const char* k, bool v) {
        lua_pushboolean(L, v);
        lua_setfield(L, -2, k);
    };
    num("x", i.x);
    num("y", i.y);
    num("effective", i.effective);
    num("sceneWidth", i.sceneW);
    num("sceneHeight", i.sceneH);
    flag("multisampled", i.multisampled);
    flag("modRequest", i.modRequest);
    flag("available", i.available);
    return 1;
}

int ShSetParam(lua_State* L) {
    ModRec* m = Current();
    const char* file = luaL_checkstring(L, 1);
    const char* entry = luaL_checkstring(L, 2);
    const char* param = luaL_checkstring(L, 3);
    float v[16];
    int n = 0;
    for (int i = 4; i <= lua_gettop(L) && n < 16; ++i) v[n++] = static_cast<float>(luaL_checknumber(L, i));
    if (n == 0) return luaL_error(L, "wum.shaders.setParam: no values");
    if (!m || !shaders::SetOwnParam(m->id.c_str(), file, entry, param, v, n))
        return luaL_error(L, "wum.shaders.setParam: '%s' is not declared by this mod's shaders\\params.ini under [%s:%s]", param, file, entry);
    return 0;
}

int ShEnableGlsl(lua_State* L) {
    ModRec* m = Current();
    const char* file = luaL_checkstring(L, 1);
    const char* entry = luaL_checkstring(L, 2);
    bool on = lua_toboolean(L, 3) != 0;
    if (!m || !shaders::SetOwnGlslEnabled(m->id.c_str(), file, entry, on))
        return luaL_error(L, "wum.shaders.enableGlsl: this mod ships no shaders\\%s.%s.glsl replacement", file, entry);
    return 0;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kDraw[] = {{"line", Line},       {"box", Box},         {"sphere", Sphere},     {"axes", Axes},
                                     {"quad", Quad},       {"text", Text},       {"hudLine", HudLine},   {"hudRect", HudRect},
                                     {"hudText", HudText}, {"hudImage", HudImage}, {"texture", Texture}, {"on", On},
                                     {"off", Off},         {nullptr, nullptr}};
    static const luaL_Reg kRender[] = {{"camera", Camera}, {"worldToScreen", WorldToScreen}, {"windowSize", WindowSize},
                                       {"timing", Timing}, {nullptr, nullptr}};
    static const luaL_Reg kPostfx[] = {{"list", PfxList}, {"enable", PfxEnable}, {"setParam", PfxSetParam},
                                       {"setTransient", PfxSetTransient}, {"getParam", PfxGetParam}, {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kDraw);
    lua_setfield(L, wum, "draw");
    lua_newtable(L);
    RegisterFunctions(L, -1, kRender);
    lua_setfield(L, wum, "render");
    lua_newtable(L);
    RegisterFunctions(L, -1, kPostfx);
    lua_setfield(L, wum, "postfx");
    static const luaL_Reg kGraphics[] = {{"setShadowMapSize", GfxSetShadowMapSize}, {"shadowMap", GfxShadowMap},
                                         {"setSupersample", GfxSetSupersample},   {"supersample", GfxSupersample},
                                         {nullptr, nullptr}};
    static const luaL_Reg kShaders[] = {{"setParam", ShSetParam}, {"enableGlsl", ShEnableGlsl}, {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kGraphics);
    lua_setfield(L, wum, "graphics");
    lua_newtable(L);
    RegisterFunctions(L, -1, kShaders);
    lua_setfield(L, wum, "shaders");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
