// wum.draw, wum.render, wum.postfx, wum.graphics and wum.shaders.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "lua/sandbox_core.h"
#include "lua/wum_media_handles.h"
#include "melange/draw.h"
#include "melange/graphics.h"
#include "melange/postfx.h"
#include "melange/render.h"
#include "melange/shaders.h"
#include "media/media.h"
#include "render/input_regions.h"
#include "render/mirage/camoverride_logic.h"
#include "render/mirage/draw_font_core.h"
#include "render/mirage/draw_queue.h"

namespace melange::sandbox {
namespace {
namespace camlogic = melange::mirage::camoverride::logic;
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

// Raises unless every key of the options table at `idx` is one of `keys`.
void CheckKeys(lua_State* L, int idx, std::initializer_list<const char*> keys, const char* msg) {
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        const char* k = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        bool known = false;
        for (const char* key : keys) known = known || strcmp(k, key) == 0;
        if (!known) luaL_argerror(L, idx, msg);
    }
}

// wum.draw.font ids: id -> the loading generation's serial (fonts die with their generation).
std::unordered_map<int, uint32_t> g_fontOwner;
constexpr int kMaxFontsPerGen = 4;

// opts.font: 0 for nil, else one of this generation's fonts (anything else raises).
int CheckFont(lua_State* L, int t, int argIdx) {
    const int type = lua_getfield(L, t, "font");
    int font = 0;
    if (type != LUA_TNIL) {
        int isInt = 0;
        const lua_Integer id = lua_tointegerx(L, -1, &isInt);
        Gen* g = CurrentGen();
        auto it = isInt ? g_fontOwner.find(static_cast<int>(id)) : g_fontOwner.end();
        if (!g || it == g_fontOwner.end() || it->second != g->serial)
            luaL_argerror(L, argIdx, "opts.font must be a font id from this mod's wum.draw.font");
        font = static_cast<int>(id);
    }
    lua_pop(L, 1);
    return font;
}

// hudText's options table (Melange 0.12): font, shadow, shadowOffset, align, maxWidth, ellipsis.
void CheckTextOptions(lua_State* L, int idx, draw::TextOptions* o) {
    idx = lua_absindex(L, idx);
    CheckKeys(L, idx, {"font", "shadow", "shadowOffset", "align", "maxWidth", "ellipsis"},
              "options are font, shadow, shadowOffset, align, maxWidth and ellipsis");
    o->font = CheckFont(L, idx, idx);
    const int st = lua_getfield(L, idx, "shadow");
    if (st == LUA_TBOOLEAN) {
        if (lua_toboolean(L, -1)) luaL_argerror(L, idx, "opts.shadow must be a colour or false");
    } else if (st != LUA_TNIL) {
        o->shadow = true;
        o->shadowColor = CheckColor(L, lua_gettop(L), 0xff000000);
    }
    lua_pop(L, 1);
    const int so = lua_getfield(L, idx, "shadowOffset");
    if (so == LUA_TTABLE) {
        const int t = lua_gettop(L);
        o->shadowDx = Field(L, t, "x", 1, 2);
        o->shadowDy = Field(L, t, "y", 2, 2);
    } else if (so != LUA_TNIL) {
        luaL_argerror(L, idx, "opts.shadowOffset must be {dx, dy}");
    }
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "align") != LUA_TNIL) {
        const char* a = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        if (strcmp(a, "left") == 0) o->align = draw::Align::Left;
        else if (strcmp(a, "center") == 0) o->align = draw::Align::Center;
        else if (strcmp(a, "right") == 0) o->align = draw::Align::Right;
        else luaL_argerror(L, idx, "opts.align must be \"left\", \"center\" or \"right\"");
    }
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "maxWidth") != LUA_TNIL) {
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_argerror(L, idx, "opts.maxWidth must be a number");
        const float w = static_cast<float>(lua_tonumber(L, -1));
        o->maxWidth = std::isfinite(w) && w > 0 ? w : 0;
    }
    lua_pop(L, 1);
    const int el = lua_getfield(L, idx, "ellipsis");
    if (el == LUA_TBOOLEAN) o->ellipsis = lua_toboolean(L, -1) != 0;
    else if (el != LUA_TNIL) luaL_argerror(L, idx, "opts.ellipsis must be a boolean");
    lua_pop(L, 1);
}

// hudText(x, y, text, color[, size[, frames]]) or, from Melange 0.12, hudText(x, y, text, color, size, opts).
int HudText(lua_State* L) {
    if (lua_type(L, 6) == LUA_TTABLE) {
        draw::TextOptions o;
        CheckTextOptions(L, 6, &o);
        draw::HudTextEx(Num(L, 1), Num(L, 2), luaL_checkstring(L, 3), CheckColor(L, 4, 0xffffffff),
                        static_cast<float>(luaL_optnumber(L, 5, 16)), o);
        return 0;
    }
    draw::HudText(Num(L, 1), Num(L, 2), luaL_checkstring(L, 3), CheckColor(L, 4, 0xffffffff),
                  static_cast<float>(luaL_optnumber(L, 5, 16)), Frames(L, 6));
    return 0;
}

bool OwnTexture(const Gen* g, lua_Integer t) {
    return g && t > 0 && std::find(g->textures.begin(), g->textures.end(), static_cast<unsigned>(t)) != g->textures.end();
}

// hudImageUV and hudNine: this mod's own 2D texture, else a raise.
unsigned CheckHudTexture(lua_State* L, int idx) {
    const lua_Integer t = luaL_checkinteger(L, idx);
    if (!OwnTexture(CurrentGen(), t)) luaL_argerror(L, idx, "texture id from this mod's wum.draw.texture expected");
    draw::TextureInfo info{};
    if (draw::GetTextureInfo(static_cast<unsigned>(t), &info) && info.layers > 0)
        luaL_argerror(L, idx, "a texture array (layers=) cannot be drawn on the HUD");
    return static_cast<unsigned>(t);
}

std::set<std::string> g_hudImageWarned;  // mods told once that hudImage dropped a texture that is not theirs

// From Melange 0.12 hudImage draws only the calling mod's own textures; anything else is dropped (a warning once per
// mod) rather than raised, so a mod written before the rule keeps running.
int HudImage(lua_State* L) {
    const lua_Integer tex = luaL_checkinteger(L, 5);
    if (!OwnTexture(CurrentGen(), tex)) {
        const ModRec* m = Current();
        const std::string id = m ? m->id : std::string();
        if (g_hudImageWarned.insert(id).second)
            SysLog(2, "wum.draw.hudImage: texture " + std::to_string(tex) +
                          " is not one of this mod's wum.draw.texture ids; not drawn (Melange 0.12)", id);
        return 0;
    }
    draw::HudImage(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), static_cast<unsigned>(tex), CheckColor(L, 6, 0xffffffff),
                   Frames(L, 7));
    return 0;
}

constexpr int kMaxTexturesPerGen = 256;

// wum.draw.texture's options table: {wrap = "repeat"|"clamp", filter = "linear"|"nearest", layers = 1..64,
// mips = false}. Returns false when the options ask for nothing but the default HUD texture ({mips = true}).
bool CheckTextureOptions(lua_State* L, int idx, draw::TextureOptions* o) {
    luaL_checktype(L, idx, LUA_TTABLE);
    idx = lua_absindex(L, idx);
    bool other = false;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        const char* k = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        if (strcmp(k, "wrap") && strcmp(k, "filter") && strcmp(k, "layers") && strcmp(k, "mips"))
            luaL_argerror(L, idx, "options are wrap, filter, layers and mips");
        other = other || strcmp(k, "mips") != 0;
    }
    const int mt = lua_getfield(L, idx, "mips");
    if (mt == LUA_TBOOLEAN) o->mips = lua_toboolean(L, -1) != 0;
    else if (mt != LUA_TNIL) luaL_argerror(L, idx, "mips must be a boolean");
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "wrap") != LUA_TNIL) {
        const char* s = lua_tostring(L, -1);
        if (!s || (strcmp(s, "repeat") && strcmp(s, "clamp"))) luaL_argerror(L, idx, "wrap must be \"repeat\" or \"clamp\"");
        o->repeat = strcmp(s, "repeat") == 0;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "filter") != LUA_TNIL) {
        const char* s = lua_tostring(L, -1);
        if (!s || (strcmp(s, "linear") && strcmp(s, "nearest"))) luaL_argerror(L, idx, "filter must be \"linear\" or \"nearest\"");
        o->nearest = strcmp(s, "nearest") == 0;
    }
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "layers") != LUA_TNIL) {
        int isInt = 0;
        const lua_Integer n = lua_tointegerx(L, -1, &isInt);
        if (!isInt || n < 1 || n > 64) luaL_argerror(L, idx, "layers must be an integer from 1 to 64");
        o->layers = static_cast<int>(n);
    }
    lua_pop(L, 1);
    return other || !o->mips;
}

int Texture(lua_State* L) {
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.draw.texture needs a mod context");
    if (g->textures.size() >= kMaxTexturesPerGen) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many textures loaded");
        return 2;
    }
    const std::string rel = luaL_checkstring(L, 1);
    if (!SafeRelPath(rel)) return luaL_argerror(L, 1, "relative path inside the mod folder expected");
    draw::TextureOptions opts;
    const bool shaderTex = !lua_isnoneornil(L, 2) && CheckTextureOptions(L, 2, &opts);
    std::wstring w = Widen(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    const std::wstring path = m->dir + L"\\" + w;
    const char* why = nullptr;
    const unsigned tex = shaderTex ? draw::LoadTextureEx(path.c_str(), opts, &why) : draw::LoadTexture(path.c_str());
    if (!tex) {
        lua_pushnil(L);
        if (why) lua_pushfstring(L, "cannot load %s: %s", rel.c_str(), why);
        else lua_pushfstring(L, "cannot load %s", rel.c_str());
        return 2;
    }
    g->textures.push_back(tex);
    g->cleanups.push_back([tex] {
        shaders::ForgetTexture(tex);
        draw::FreeTexture(tex);
    });
    lua_pushinteger(L, tex);
    return 1;
}

constexpr uint32_t kMaxSpritesPerMod = 4096;  // per frame, across all of the mod's draw callbacks

draw::SpriteBlend CheckSpriteMode(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return draw::SpriteBlend::Alpha;
    size_t n = 0;
    const char* s = luaL_checklstring(L, idx, &n);
    if (n == 5 && memcmp(s, "alpha", 5) == 0) return draw::SpriteBlend::Alpha;
    if (n == 6 && memcmp(s, "premul", 6) == 0) return draw::SpriteBlend::Premultiplied;
    if (n == 8 && memcmp(s, "additive", 8) == 0) return draw::SpriteBlend::Additive;
    luaL_argerror(L, idx, "mode must be \"alpha\", \"premul\" or \"additive\"");
    return draw::SpriteBlend::Alpha;
}

// wum.draw.sprite(tex, x, y, z, halfW, halfL, ax, ay, az[, color[, mode]]) -> true, or false when dropped.
int Sprite(lua_State* L) {
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.draw.sprite needs a mod context");
    draw::Sprite s{};
    const lua_Integer tex = luaL_checkinteger(L, 1);
    // Any id this generation loaded; the last one accepted is checked first (usually all a frame uses).
    if (tex <= 0 || static_cast<lua_Integer>(g->spriteTexHit) != tex) {
        if (tex <= 0 || std::find(g->textures.begin(), g->textures.end(), static_cast<unsigned>(tex)) == g->textures.end())
            return luaL_argerror(L, 1, "texture id from this mod's wum.draw.texture expected");
        draw::TextureInfo info{};
        if (draw::GetTextureInfo(static_cast<unsigned>(tex), &info) && info.layers > 0)
            return luaL_argerror(L, 1, "a texture array (layers=) cannot be a sprite");
        g->spriteTexHit = static_cast<unsigned>(tex);
    }
    s.texture = static_cast<unsigned>(tex);
    s.pos[0] = Num(L, 2);
    s.pos[1] = Num(L, 3);
    s.pos[2] = Num(L, 4);
    s.halfW = Num(L, 5);
    s.halfL = static_cast<float>(luaL_optnumber(L, 6, 0));
    for (int i = 0; i < 3; ++i) s.axis[i] = static_cast<float>(luaL_optnumber(L, 7 + i, 0));
    s.color = CheckColor(L, 10, 0xffffffff);
    s.blend = CheckSpriteMode(L, 11);
    const render::Stage stage = draw::CurrentStage();
    if (stage != render::Stage::World && stage != render::Stage::WorldLate)
        return luaL_error(L, "wum.draw.sprite: only inside a wum.draw.on(\"world\" or \"worldLate\") callback");
    const uint64_t frame = draw::FrameSerial();
    if (m->spriteFrame != frame) {
        m->spriteFrame = frame;
        m->spriteCount = 0;
    }
    bool ok = m->spriteCount < kMaxSpritesPerMod && draw::DrawSprite(s);
    if (ok) ++m->spriteCount;
    lua_pushboolean(L, ok);
    return 1;
}

bool ParseStage(const std::string& s, render::Stage* out) {
    if (s == "world") *out = render::Stage::World;
    else if (s == "worldLate") *out = render::Stage::WorldLate;
    else if (s == "hud") *out = render::Stage::Hud;
    else if (s == "screen") *out = render::Stage::Screen;
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
        case render::Stage::Screen: return "screen";
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
    if (!ParseStage(s, &stage)) return luaL_argerror(L, 1, "stage must be world, worldLate, hud or screen");
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
// wum.render.camera() and wum.render.gameCamera(): {pos, fwd, up, near, far, fov, overridden}.
int PushCamera(lua_State* L, const render::Camera& c) {
    lua_createtable(L, 0, 7);
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
    lua_pushnumber(L, camlogic::FovOf(c.proj));
    lua_setfield(L, -2, "fov");
    lua_pushboolean(L, render::CameraOverridden());
    lua_setfield(L, -2, "overridden");
    return 1;
}

int Camera(lua_State* L) {
    render::Camera c{};
    if (!render::GetCamera(&c) || !c.valid) return 0;
    return PushCamera(L, c);
}

int GameCamera(lua_State* L) {
    render::Camera c{};
    if (!render::GetGameCamera(&c) || !c.valid) return 0;
    return PushCamera(L, c);
}

// A {x, y, z} (or {1, 2, 3}) field of the table at `t`; false when absent.
bool VecField(lua_State* L, int t, const char* name, float out[3]) {
    const int type = lua_getfield(L, t, name);
    if (type == LUA_TNIL) {
        lua_pop(L, 1);
        return false;
    }
    if (type != LUA_TTABLE) luaL_error(L, "wum.render.cameraOverride: %s must be a {x, y, z} table", name);
    CheckVec(L, -1, out);
    lua_pop(L, 1);
    return true;
}

// wum.render.cameraOverride({pos=, target=[, up=][, fov=][, frames=]}) -> true | false, "busy"|"unavailable";
// wum.render.cameraOverride(nil) -> true if this mod's override was cleared.
int CameraOverride(lua_State* L) {
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.render.cameraOverride needs a mod context");
    if (lua_isnoneornil(L, 1)) {
        lua_pushboolean(L, render::ClearCameraOverride(m));
        return 1;
    }
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        lua_pop(L, 1);
        const char* k = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        if (strcmp(k, "pos") && strcmp(k, "target") && strcmp(k, "up") && strcmp(k, "fov") && strcmp(k, "frames"))
            return luaL_argerror(L, 1, "fields are pos, target, up, fov and frames");
    }
    render::CameraOverride o{};
    if (!VecField(L, 1, "pos", o.pos)) return luaL_argerror(L, 1, "pos = {x, y, z} expected");
    if (!VecField(L, 1, "target", o.target)) return luaL_argerror(L, 1, "target = {x, y, z} expected");
    if (!VecField(L, 1, "up", o.up)) o.up[1] = 1.f;
    int frames = 0;
    if (lua_getfield(L, 1, "fov") != LUA_TNIL) {
        if (!lua_isnumber(L, -1)) return luaL_argerror(L, 1, "fov must be a number of degrees");
        o.fovDeg = static_cast<float>(lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    if (lua_getfield(L, 1, "frames") != LUA_TNIL) {
        int isInt = 0;
        const lua_Integer n = lua_tointegerx(L, -1, &isInt);
        if (!isInt || n < 1 || n > camlogic::kMaxFrames) return luaL_argerror(L, 1, "frames must be an integer from 1 to 600");
        frames = static_cast<int>(n);
    }
    lua_pop(L, 1);
    camlogic::Spec spec{};
    memcpy(spec.pos, o.pos, sizeof spec.pos);
    memcpy(spec.target, o.target, sizeof spec.target);
    memcpy(spec.up, o.up, sizeof spec.up);
    spec.fovDeg = o.fovDeg;
    if (const char* why = camlogic::Validate(spec)) return luaL_argerror(L, 1, why);
    switch (render::SetCameraOverride(m, o, frames)) {
        case render::OverrideResult::Ok:
            if (!g->cameraCleanup) {
                g->cameraCleanup = true;
                g->cleanups.push_back([m] { render::ClearCameraOverride(m); });
            }
            lua_pushboolean(L, 1);
            return 1;
        case render::OverrideResult::Busy:
            lua_pushboolean(L, 0);
            lua_pushliteral(L, "busy");
            return 2;
        case render::OverrideResult::Invalid: return luaL_argerror(L, 1, "invalid camera");
        default:
            lua_pushboolean(L, 0);
            lua_pushliteral(L, "unavailable");
            return 2;
    }
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

// ---------------------------------------------------------------- wum.draw meshes
// Vertices: a flat array of numbers, 8 per vertex (x, y, z, nx, ny, nz, u, v) or, with format "pntc", 12 (then r, g,
// b, a in 0..1). Indices: a flat array of 1-based vertex numbers, three per triangle.
constexpr uint32_t kMaxMeshVerts = 16384;        // per mesh
constexpr uint32_t kMaxMeshIndices = 49152;      // per mesh (16384 triangles)
constexpr size_t kMaxMeshesPerGen = 64;          // wum.draw.newMesh handles alive at once
constexpr size_t kMaxMeshVertsPerGen = 262144;   // vertices held by those handles
constexpr uint32_t kMaxMeshCallsPerFrame = 256;  // wum.draw.mesh calls per mod and frame
constexpr uint32_t kMaxMeshFrameVerts = 131072;  // vertices drawn per mod and frame
constexpr float kDefaultLight[3] = {0.f, 0.936f, -0.351f};  // the landscape shader's world light (RE note U19)
constexpr float kDefaultAmbient = 0.6f;
}  // namespace

struct MeshData {
    std::vector<float> v;  // 12 floats per vertex: position, normal, uv, rgba
    std::vector<uint16_t> idx;
    uint32_t count = 0;
};

namespace {
void ParseMeshFormat(lua_State* L, int idx, int* stride) {
    *stride = 8;
    if (lua_isnoneornil(L, idx)) return;
    const char* f = luaL_checkstring(L, idx);
    if (strcmp(f, "pnt") == 0) *stride = 8;
    else if (strcmp(f, "pntc") == 0) *stride = 12;
    else luaL_argerror(L, idx, "format must be \"pnt\" or \"pntc\"");
}

float RawNumber(lua_State* L, int t, lua_Integer i, int argIdx) {
    lua_rawgeti(L, t, i);
    int ok = 0;
    const lua_Number n = lua_tonumberx(L, -1, &ok);
    lua_pop(L, 1);
    if (!ok || !std::isfinite(n)) luaL_argerror(L, argIdx, "finite numbers expected");
    return static_cast<float>(n);
}

void ParseMesh(lua_State* L, int vIdx, int iIdx, int stride, MeshData* out) {
    luaL_checktype(L, vIdx, LUA_TTABLE);
    luaL_checktype(L, iIdx, LUA_TTABLE);
    const lua_Integer nv = static_cast<lua_Integer>(lua_rawlen(L, vIdx));
    if (nv == 0 || nv % stride)
        luaL_argerror(L, vIdx, stride == 8 ? "8 numbers per vertex expected" : "12 numbers per vertex expected");
    const lua_Integer count = nv / stride;
    if (count > static_cast<lua_Integer>(kMaxMeshVerts)) luaL_argerror(L, vIdx, "too many vertices (at most 16384)");
    const lua_Integer ni = static_cast<lua_Integer>(lua_rawlen(L, iIdx));
    if (ni == 0 || ni % 3) luaL_argerror(L, iIdx, "3 indices per triangle expected");
    if (ni > static_cast<lua_Integer>(kMaxMeshIndices)) luaL_argerror(L, iIdx, "too many indices (at most 49152)");
    out->count = static_cast<uint32_t>(count);
    out->v.assign(static_cast<size_t>(count) * 12, 1.f);
    for (lua_Integer k = 0; k < count; ++k) {
        float* d = &out->v[static_cast<size_t>(k) * 12];
        for (int j = 0; j < stride; ++j) d[j] = RawNumber(L, vIdx, k * stride + j + 1, vIdx);
        float* n = d + 3;
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len > 1e-12f) {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        } else {
            n[0] = 0, n[1] = 1, n[2] = 0;
        }
        if (stride == 12)
            for (int j = 8; j < 12; ++j) d[j] = std::clamp(d[j], 0.f, 1.f);
    }
    out->idx.resize(static_cast<size_t>(ni));
    for (lua_Integer k = 0; k < ni; ++k) {
        lua_rawgeti(L, iIdx, k + 1);
        int isInt = 0;
        const lua_Integer x = lua_tointegerx(L, -1, &isInt);
        lua_pop(L, 1);
        if (!isInt || x < 1 || x > count) luaL_argerror(L, iIdx, "indices must be integers from 1 to the vertex count");
        out->idx[static_cast<size_t>(k)] = static_cast<uint16_t>(x - 1);
    }
}

// model: nil (identity), 16 numbers (glLoadMatrixf layout), or {pos=, yaw=, scale=}: translate * rotate about +Y *
// scale. `normal` gets the matrix that takes normals to world space (the inverse transpose of the 3x3 part, row-major).
void CheckModel(lua_State* L, int idx, float model[16], float normal[9]) {
    static const float kId[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(model, kId, sizeof kId);
    if (lua_isnoneornil(L, idx)) {
        const float n[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        memcpy(normal, n, sizeof n);
        return;
    }
    luaL_checktype(L, idx, LUA_TTABLE);
    idx = lua_absindex(L, idx);
    if (lua_rawlen(L, idx) == 16) {
        for (int i = 0; i < 16; ++i) model[i] = RawNumber(L, idx, i + 1, idx);
    } else {
        float pos[3] = {0, 0, 0}, s[3] = {1, 1, 1}, yaw = 0;
        if (lua_getfield(L, idx, "pos") == LUA_TTABLE) CheckVec(L, -1, pos);
        else if (!lua_isnil(L, -1)) luaL_argerror(L, idx, "model.pos must be {x, y, z}");
        lua_pop(L, 1);
        if (lua_getfield(L, idx, "yaw") != LUA_TNIL) {
            if (!lua_isnumber(L, -1)) luaL_argerror(L, idx, "model.yaw must be a number (radians)");
            yaw = static_cast<float>(lua_tonumber(L, -1));
        }
        lua_pop(L, 1);
        const int st = lua_getfield(L, idx, "scale");
        if (st == LUA_TNUMBER) s[0] = s[1] = s[2] = static_cast<float>(lua_tonumber(L, -1));
        else if (st == LUA_TTABLE) CheckVec(L, -1, s);
        else if (st != LUA_TNIL) luaL_argerror(L, idx, "model.scale must be a number or {x, y, z}");
        lua_pop(L, 1);
        const float c = std::cos(yaw), sn = std::sin(yaw);
        // Rotation about +Y by yaw takes +Z to (sin yaw, 0, cos yaw), a worm's facing at that yaw.
        const float m[16] = {c * s[0], 0, -sn * s[0], 0, 0, s[1], 0, 0, sn * s[2], 0, c * s[2], 0, pos[0], pos[1], pos[2], 1};
        memcpy(model, m, sizeof m);
    }
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(model[i])) luaL_argerror(L, idx, "model must be finite");
    // M = [a b c; d e f; g h k] (row, col) from column-major model[col*4+row].
    const float a = model[0], b = model[4], c = model[8], d = model[1], e = model[5], f = model[9], g = model[2], h = model[6],
                k = model[10];
    const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    if (std::fabs(det) < 1e-20f) luaL_argerror(L, idx, "model must not be singular");
    const float id = 1.f / det;
    // (M^-1)^T is the cofactor matrix over det, row-major here: n' = R n.
    const float r[9] = {(e * k - f * h) * id, -(d * k - f * g) * id, (d * h - e * g) * id,
                        -(b * k - c * h) * id, (a * k - c * g) * id,  -(a * h - b * g) * id,
                        (b * f - c * e) * id,  -(a * f - c * d) * id, (a * e - b * d) * id};
    memcpy(normal, r, sizeof r);
}

struct MeshOpts {
    bool lit = true, additive = false, depthWrite = true, cull = true, depthTest = true;
    float light[3] = {kDefaultLight[0], kDefaultLight[1], kDefaultLight[2]};
    float ambient = kDefaultAmbient;
    draw::Rgba color = 0xffffffff;
    int stride = 8;
};

bool BoolField(lua_State* L, int t, const char* k, bool def) {
    const int type = lua_getfield(L, t, k);
    bool v = def;
    if (type == LUA_TBOOLEAN) v = lua_toboolean(L, -1) != 0;
    else if (type != LUA_TNIL) luaL_error(L, "wum.draw.mesh: opts.%s must be a boolean", k);
    lua_pop(L, 1);
    return v;
}

// opts of the vertex-array form also take color and format.
void CheckMeshOpts(lua_State* L, int idx, bool arrayForm, MeshOpts* o) {
    if (lua_isnoneornil(L, idx)) return;
    luaL_checktype(L, idx, LUA_TTABLE);
    idx = lua_absindex(L, idx);
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        lua_pop(L, 1);
        const char* k = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
        static const char* kKeys[] = {"lit", "blend", "depthWrite", "cull", "xray", "light", "ambient", "color", "format"};
        bool known = false;
        for (const char* key : kKeys) known = known || strcmp(k, key) == 0;
        if (!known || (!arrayForm && (strcmp(k, "color") == 0 || strcmp(k, "format") == 0)))
            luaL_argerror(L, idx, arrayForm ? "options are lit, blend, depthWrite, cull, xray, light, ambient, color and format"
                                            : "options are lit, blend, depthWrite, cull, xray, light and ambient");
    }
    o->lit = BoolField(L, idx, "lit", o->lit);
    o->depthWrite = BoolField(L, idx, "depthWrite", o->depthWrite);
    o->cull = BoolField(L, idx, "cull", o->cull);
    o->depthTest = !BoolField(L, idx, "xray", false);
    if (lua_getfield(L, idx, "blend") != LUA_TNIL) {
        const char* b = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : nullptr;
        if (!b || (strcmp(b, "alpha") && strcmp(b, "additive"))) luaL_argerror(L, idx, "opts.blend must be \"alpha\" or \"additive\"");
        o->additive = strcmp(b, "additive") == 0;
    }
    lua_pop(L, 1);
    const int lt = lua_getfield(L, idx, "light");
    if (lt == LUA_TTABLE) {
        CheckVec(L, -1, o->light);
        const float l = std::sqrt(o->light[0] * o->light[0] + o->light[1] * o->light[1] + o->light[2] * o->light[2]);
        if (!(l > 1e-6f) || !std::isfinite(l)) luaL_argerror(L, idx, "opts.light must be a non-zero direction");
        for (float& c : o->light) c /= l;
    } else if (lt != LUA_TNIL) {
        luaL_argerror(L, idx, "opts.light must be {x, y, z}");
    }
    lua_pop(L, 1);
    if (lua_getfield(L, idx, "ambient") != LUA_TNIL) {
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_argerror(L, idx, "opts.ambient must be a number from 0 to 1");
        o->ambient = std::clamp(static_cast<float>(lua_tonumber(L, -1)), 0.f, 1.f);
    }
    lua_pop(L, 1);
    if (arrayForm) {
        lua_getfield(L, idx, "color");
        o->color = CheckColor(L, lua_gettop(L), 0xffffffff);
        lua_pop(L, 1);
        lua_getfield(L, idx, "format");
        ParseMeshFormat(L, lua_gettop(L), &o->stride);
        lua_pop(L, 1);
    }
}

// The mod's texture at `idx` (nil or 0 = untextured); a texture array is refused.
unsigned CheckMeshTexture(lua_State* L, Gen* g, int idx) {
    if (lua_isnoneornil(L, idx)) return 0;
    const lua_Integer t = luaL_checkinteger(L, idx);
    if (t == 0) return 0;
    if (t < 0 || std::find(g->textures.begin(), g->textures.end(), static_cast<unsigned>(t)) == g->textures.end())
        luaL_argerror(L, idx, "texture id from this mod's wum.draw.texture (or nil) expected");
    draw::TextureInfo info{};
    if (draw::GetTextureInfo(static_cast<unsigned>(t), &info) && info.layers > 0)
        luaL_argerror(L, idx, "a texture array (layers=) cannot texture a mesh");
    return static_cast<unsigned>(t);
}

std::vector<draw::Vertex> g_meshScratch;

// Lights (on the CPU, per vertex, in world space), tints and submits one mesh. False when over the frame budget.
bool SubmitMesh(ModRec* m, const MeshData& d, unsigned tex, const float model[16], const float nrm[9], const MeshOpts& o) {
    const uint64_t frame = draw::FrameSerial();
    if (m->meshFrame != frame) {
        m->meshFrame = frame;
        m->meshCount = 0;
        m->meshFrameVerts = 0;
    }
    if (m->meshCount >= kMaxMeshCallsPerFrame || m->meshFrameVerts + d.count > kMaxMeshFrameVerts) return false;
    ++m->meshCount;
    m->meshFrameVerts += d.count;
    const float tint[4] = {(o.color & 255) / 255.f, ((o.color >> 8) & 255) / 255.f, ((o.color >> 16) & 255) / 255.f,
                           ((o.color >> 24) & 255) / 255.f};
    g_meshScratch.resize(d.count);
    for (uint32_t i = 0; i < d.count; ++i) {
        const float* s = &d.v[static_cast<size_t>(i) * 12];
        draw::Vertex& v = g_meshScratch[i];
        v.pos[0] = s[0], v.pos[1] = s[1], v.pos[2] = s[2];
        v.uv[0] = s[6], v.uv[1] = s[7];
        float shade = 1.f;
        if (o.lit) {
            const float n[3] = {nrm[0] * s[3] + nrm[1] * s[4] + nrm[2] * s[5], nrm[3] * s[3] + nrm[4] * s[4] + nrm[5] * s[5],
                                nrm[6] * s[3] + nrm[7] * s[4] + nrm[8] * s[5]};
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            const float dot = len > 1e-12f ? (n[0] * o.light[0] + n[1] * o.light[1] + n[2] * o.light[2]) / len : 0.f;
            shade = o.ambient + (1.f - o.ambient) * (dot > 0.f ? dot : 0.f);
        }
        v.color = Pack(Unit(s[8] * tint[0] * shade), Unit(s[9] * tint[1] * shade), Unit(s[10] * tint[2] * shade),
                       Unit(s[11] * tint[3]));
    }
    uint32_t flags = 0;
    if (o.depthTest) flags |= draw::kDepthTest;
    if (o.depthWrite) flags |= draw::kDepthWrite;
    if (o.additive) flags |= draw::kAdditive;
    if (!o.cull) flags |= draw::kNoCull;
    draw::Mesh(g_meshScratch.data(), d.count, d.idx.data(), static_cast<uint32_t>(d.idx.size()), model, tex, flags, 1);
    return true;
}

// wum.draw.mesh(verts, idx, tex, model[, opts]) or wum.draw.mesh(handle, tex, model[, color[, opts]]) -> true, or
// false when over the per-frame budget. Only inside a "world" or "worldLate" callback.
int MeshDraw(lua_State* L) {
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.draw.mesh needs a mod context");
    float model[16], nrm[9];
    MeshOpts o;
    std::shared_ptr<MeshData> keep;
    MeshData local;
    unsigned tex = 0;
    if (lua_type(L, 1) == LUA_TNUMBER) {
        const lua_Integer h = luaL_checkinteger(L, 1);
        auto it = h > 0 ? g->meshes.find(static_cast<uint32_t>(h)) : g->meshes.end();
        if (it == g->meshes.end()) return luaL_argerror(L, 1, "mesh handle from this mod's wum.draw.newMesh expected");
        keep = it->second;
        tex = CheckMeshTexture(L, g, 2);
        CheckModel(L, 3, model, nrm);
        o.color = CheckColor(L, 4, 0xffffffff);
        CheckMeshOpts(L, 5, false, &o);
    } else {
        tex = CheckMeshTexture(L, g, 3);
        CheckModel(L, 4, model, nrm);
        CheckMeshOpts(L, 5, true, &o);
        ParseMesh(L, 1, 2, o.stride, &local);
    }
    const render::Stage stage = draw::CurrentStage();
    if (stage != render::Stage::World && stage != render::Stage::WorldLate)
        return luaL_error(L, "wum.draw.mesh: only inside a wum.draw.on(\"world\" or \"worldLate\") callback");
    lua_pushboolean(L, SubmitMesh(m, keep ? *keep : local, tex, model, nrm, o));
    return 1;
}

// wum.draw.newMesh(verts, idx[, format]) -> handle, or nil, "too many meshes"
int NewMesh(lua_State* L) {
    Gen* g = CurrentGen();
    if (!Current() || !g) return luaL_error(L, "wum.draw.newMesh needs a mod context");
    int stride = 8;
    ParseMeshFormat(L, 3, &stride);
    auto d = std::make_shared<MeshData>();
    ParseMesh(L, 1, 2, stride, d.get());
    if (g->meshes.size() >= kMaxMeshesPerGen || g->meshVerts + d->count > kMaxMeshVertsPerGen) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many meshes");
        return 2;
    }
    if (g->nextMesh == 1)
        g->cleanups.push_back([g] {
            g->meshes.clear();
            g->meshVerts = 0;
        });
    const uint32_t id = g->nextMesh++;
    g->meshVerts += d->count;
    g->meshes.emplace(id, std::move(d));
    lua_pushinteger(L, id);
    return 1;
}

// wum.draw.freeMesh(handle) -> true if it was one of this mod's live meshes
int FreeMesh(lua_State* L) {
    Gen* g = CurrentGen();
    const lua_Integer h = luaL_checkinteger(L, 1);
    bool ok = false;
    if (g && h > 0) {
        auto it = g->meshes.find(static_cast<uint32_t>(h));
        if (it != g->meshes.end()) {
            g->meshVerts -= it->second->count;
            g->meshes.erase(it);
            ok = true;
        }
    }
    lua_pushboolean(L, ok);
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

// wum.shaders.setTexture(file, entry, sampler, tex|nil) -> true, or false while GLSL replacements are off.
int ShSetTexture(lua_State* L) {
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.shaders.setTexture needs a mod context");
    const char* file = luaL_checkstring(L, 1);
    const char* entry = luaL_checkstring(L, 2);
    const char* sampler = luaL_checkstring(L, 3);
    unsigned tex = 0;
    if (!lua_isnoneornil(L, 4)) {
        const lua_Integer t = luaL_checkinteger(L, 4);
        if (t <= 0 || std::find(g->textures.begin(), g->textures.end(), static_cast<unsigned>(t)) == g->textures.end())
            return luaL_argerror(L, 4, "texture id from this mod's wum.draw.texture expected");
        tex = static_cast<unsigned>(t);
    }
    switch (shaders::SetOwnTexture(m->id.c_str(), file, entry, sampler, tex)) {
        case shaders::TextureResult::Ok: lua_pushboolean(L, 1); return 1;
        case shaders::TextureResult::Inactive: lua_pushboolean(L, 0); return 1;
        case shaders::TextureResult::BadName:
            return luaL_argerror(L, 3, "sampler name must be a GLSL identifier, not starting with gl_ or mg_");
        default:
            return luaL_error(L, "wum.shaders.setTexture: this mod ships no shaders\\%s.<entry>.glsl replacement matching '%s'", file, entry);
    }
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

// ---------------------------------------------------------------- Melange 0.12 drawing
// wum.draw.hudImageUV(x0, y0, x1, y1, tex, u0, v0, u1, v1[, tint])
int HudImageUV(lua_State* L) {
    const unsigned tex = CheckHudTexture(L, 5);
    draw::HudImageUV(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), tex, Num(L, 6), Num(L, 7), Num(L, 8), Num(L, 9),
                     CheckColor(L, 10, 0xffffffff));
    return 0;
}

// wum.draw.hudNine(x0, y0, x1, y1, tex, l, t, r, b[, {tint =, scale = 1, uv = {u0, v0, u1, v1}}])
int HudNine(lua_State* L) {
    const unsigned tex = CheckHudTexture(L, 5);
    draw::NineOptions o;
    if (!lua_isnoneornil(L, 10)) {
        luaL_checktype(L, 10, LUA_TTABLE);
        CheckKeys(L, 10, {"tint", "scale", "uv"}, "options are tint, scale and uv");
        lua_getfield(L, 10, "tint");
        o.tint = CheckColor(L, lua_gettop(L), 0xffffffff);
        lua_pop(L, 1);
        if (lua_getfield(L, 10, "scale") != LUA_TNIL) {
            const lua_Number sc = lua_tonumber(L, -1);
            if (lua_type(L, -1) != LUA_TNUMBER || !(sc > 0) || !(sc <= 64)) luaL_argerror(L, 10, "opts.scale must be a number above 0");
            o.scale = static_cast<float>(sc);
        }
        lua_pop(L, 1);
        const int ut = lua_getfield(L, 10, "uv");
        if (ut == LUA_TTABLE) {
            const int t = lua_gettop(L);
            for (int i = 0; i < 4; ++i) {
                lua_rawgeti(L, t, i + 1);
                if (lua_type(L, -1) != LUA_TNUMBER) luaL_argerror(L, 10, "opts.uv must be {u0, v0, u1, v1}");
                o.uv[i] = static_cast<float>(lua_tonumber(L, -1));
                lua_pop(L, 1);
            }
        } else if (ut != LUA_TNIL) {
            luaL_argerror(L, 10, "opts.uv must be {u0, v0, u1, v1}");
        }
        lua_pop(L, 1);
    }
    draw::HudNine(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), tex, Num(L, 6), Num(L, 7), Num(L, 8), Num(L, 9), o);
    return 0;
}

// wum.draw.font(atlasPng, metricsJson) -> font id, or nil, reason
int Font(lua_State* L) {
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return luaL_error(L, "wum.draw.font needs a mod context");
    const std::string png = luaL_checkstring(L, 1), metrics = luaL_checkstring(L, 2);
    if (!SafeRelPath(png)) return luaL_argerror(L, 1, "relative path inside the mod folder expected");
    if (!SafeRelPath(metrics)) return luaL_argerror(L, 2, "relative path inside the mod folder expected");
    int held = 0;
    for (const auto& [id, serial] : g_fontOwner) held += serial == g->serial;
    if (held >= kMaxFontsPerGen) {
        lua_pushnil(L);
        lua_pushliteral(L, "too many fonts loaded");
        return 2;
    }
    auto modPath = [m](const std::string& rel) {
        std::wstring w = Widen(rel);
        std::replace(w.begin(), w.end(), L'/', L'\\');
        return m->dir + L"\\" + w;
    };
    std::string text, err;
    if (!ReadWhole(modPath(metrics), mirage::drawfont::kMaxMetricsBytes, &text, &err)) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot read %s: %s", metrics.c_str(), err.c_str());
        return 2;
    }
    std::string why;
    const int font = draw::LoadFont(modPath(png).c_str(), text.data(), text.size(), &why);
    if (!font) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot load font %s: %s", png.c_str(), why.c_str());
        return 2;
    }
    g_fontOwner[font] = g->serial;
    g->cleanups.push_back([font] {
        draw::FreeFont(font);
        g_fontOwner.erase(font);
    });
    lua_pushinteger(L, font);
    return 1;
}

// wum.draw.textSize(text, size[, {font = f}]) -> w, h
int TextSize(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    const float size = static_cast<float>(luaL_optnumber(L, 2, 16));
    int font = 0;
    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        CheckKeys(L, 3, {"font"}, "the option is font");
        font = CheckFont(L, 3, 3);
    }
    const draw::TextExtent e = draw::MeasureText(text, size, font);
    lua_pushnumber(L, e.w);
    lua_pushnumber(L, e.h);
    return 2;
}

// wum.draw.hudClip(x0, y0, x1, y1) -> true, or false outside a "hud"/"screen" callback or past 4 deep.
int HudClip(lua_State* L) {
    lua_pushboolean(L, draw::HudClip(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4)));
    return 1;
}

int HudClipEnd(lua_State*) {
    draw::HudClipEnd();
    return 0;
}

// wum.draw.media(player, x0, y0, x1, y1[, {fit = "contain"|"cover"|"stretch", alpha = 1}]) -> px0, py0, px1, py1, or
// nil without a frame. Only the mod that opened the player gets its frame (media::FrameForDraw checks the owner).
int Media(lua_State* L) {
    const mediaud::PlayerUd* ud = mediaud::TestPlayer(L, 1);
    if (!ud) return luaL_argerror(L, 1, "wum.media player expected");
    const float x0 = Num(L, 2), y0 = Num(L, 3), x1 = Num(L, 4), y1 = Num(L, 5);
    mirage::drawqueue::Fit fit = mirage::drawqueue::Fit::Contain;
    float alpha = 1;
    if (!lua_isnoneornil(L, 6)) {
        luaL_checktype(L, 6, LUA_TTABLE);
        CheckKeys(L, 6, {"fit", "alpha"}, "options are fit and alpha");
        if (lua_getfield(L, 6, "fit") != LUA_TNIL) {
            const char* f = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
            if (strcmp(f, "contain") == 0) fit = mirage::drawqueue::Fit::Contain;
            else if (strcmp(f, "cover") == 0) fit = mirage::drawqueue::Fit::Cover;
            else if (strcmp(f, "stretch") == 0) fit = mirage::drawqueue::Fit::Stretch;
            else luaL_argerror(L, 6, "opts.fit must be \"contain\", \"cover\" or \"stretch\"");
        }
        lua_pop(L, 1);
        if (lua_getfield(L, 6, "alpha") != LUA_TNIL) {
            if (lua_type(L, -1) != LUA_TNUMBER) luaL_argerror(L, 6, "opts.alpha must be a number from 0 to 1");
            alpha = std::clamp(static_cast<float>(lua_tonumber(L, -1)), 0.f, 1.f);
        }
        lua_pop(L, 1);
    }
    ModRec* m = Current();
    Gen* g = CurrentGen();
    media::DrawFrame f;
    if (!m || !g || ud->id == 0 || ud->serial != g->serial || !media::FrameForDraw(m->id.c_str(), ud->id, &f) || !f.texture) {
        lua_pushnil(L);
        return 1;
    }
    const double aspect = f.aspect > 0 ? f.aspect : (f.h > 0 ? static_cast<double>(f.w) / f.h : 0);
    mirage::drawqueue::UvQuad q;
    if (!mirage::drawqueue::FitPicture(x0, y0, x1, y1, aspect, f.u1, f.v1, fit, &q)) {
        lua_pushnil(L);
        return 1;
    }
    draw::HudVideo(q.x0, q.y0, q.x1, q.y1, f.texture, q.u0, q.v0, q.u1, q.v1, alpha);
    if (alpha > 0.05f) melange::render::modinput::NoteCover(q.x0, q.y0, q.x1, q.y1);  // keeps the menu cursor visible on top
    lua_pushnumber(L, q.x0);
    lua_pushnumber(L, q.y0);
    lua_pushnumber(L, q.x1);
    lua_pushnumber(L, q.y1);
    return 4;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kDraw[] = {{"line", Line},       {"box", Box},         {"sphere", Sphere},     {"axes", Axes},
                                     {"quad", Quad},       {"text", Text},       {"hudLine", HudLine},   {"hudRect", HudRect},
                                     {"hudText", HudText}, {"hudImage", HudImage}, {"texture", Texture}, {"sprite", Sprite},
                                     {"mesh", MeshDraw},   {"newMesh", NewMesh}, {"freeMesh", FreeMesh},
                                     {"on", On},           {"off", Off},
                                     {"hudImageUV", HudImageUV}, {"hudNine", HudNine}, {"font", Font},
                                     {"textSize", TextSize}, {"hudClip", HudClip}, {"hudClipEnd", HudClipEnd},
                                     {"media", Media}, {nullptr, nullptr}};
    static const luaL_Reg kRender[] = {{"camera", Camera}, {"worldToScreen", WorldToScreen}, {"windowSize", WindowSize},
                                       {"timing", Timing}, {"cameraOverride", CameraOverride},
                                       {"gameCamera", GameCamera}, {nullptr, nullptr}};
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
    static const luaL_Reg kShaders[] = {{"setParam", ShSetParam}, {"enableGlsl", ShEnableGlsl}, {"setTexture", ShSetTexture},
                                        {nullptr, nullptr}};
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
