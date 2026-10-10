// wum.audio: client-side sound for mods. Presentation only, in the client Sandbox VM; the sim VM has no such library.
// The work is done by src/audio/audio.cpp; this file scopes clips and voices to the calling mod generation.
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <string>

#include "audio/audio.h"
#include "audio/clips.h"
#include "audio/wav.h"
#include "lua/sandbox_core.h"

namespace melange::sandbox {
namespace {
namespace au = melange::audio;

char g_consoleOwner;      // the console has no mod and so no clips; its token only keeps stop() well-defined
std::set<Gen*> g_hooked;  // generations whose teardown releases their clips and voices

const void* Owner() {
    Gen* g = CurrentGen();
    return g ? static_cast<const void*>(g) : static_cast<const void*>(&g_consoleOwner);
}

int Fail(lua_State* L, const char* why) {
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

// A mod's generation is revoked on unload, reload and disable: that is the one notification the Sandbox gives, so the
// voices are stopped there. Registered once per generation, on its first load or play.
void Hook(Gen* g) {
    if (g && g_hooked.insert(g).second)
        g->cleanups.push_back([g] {
            g_hooked.erase(g);
            au::Release(g);
        });
}

int Ready(lua_State* L) {
    lua_pushboolean(L, au::Available());
    return 1;
}

int Load(lua_State* L) {
    const std::string rel = luaL_checkstring(L, 1);
    ModRec* m = Current();
    Gen* g = CurrentGen();
    if (!m || !g) return Fail(L, "needs a mod context");
    if (!SafeRelPath(rel)) return Fail(L, "path must be relative and inside the mod folder");
    // Parsing needs no output device, so a mod that loads at boot gets its handles even if the device comes up late;
    // only play reports "audio unavailable".
    Hook(g);

    const std::string key = au::NormalizeClipKey(rel);
    if (const uint32_t have = au::FindClip(g, key)) {
        lua_pushinteger(L, have);
        return 1;
    }
    std::wstring w = Widen(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    std::string bytes, err;
    // One byte over the limit so that "too large" is told apart from a file that is exactly at it.
    if (!ReadWhole(m->dir + L"\\" + w, au::kMaxWavBytes + 1, &bytes, &err))
        return Fail(L, err.rfind("file too large", 0) == 0 ? "too large" : "cannot open file");
    std::string why;
    const uint32_t id = au::LoadClip(g, key, reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), &why);
    if (!id) return Fail(L, why.c_str());
    lua_pushinteger(L, id);
    return 1;
}

float Num(lua_State* L, int t, const char* key, float def, float lo, float hi) {
    lua_getfield(L, t, key);
    float v = def;
    if (!lua_isnil(L, -1)) {
        if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "wum.audio.play: %s must be a number", key);
        const double d = lua_tonumber(L, -1);
        // +/-infinity clamps like any other out-of-range number; only NaN has no side to clamp to.
        v = std::isnan(d) ? def : static_cast<float>(std::clamp(d, static_cast<double>(lo), static_cast<double>(hi)));
    }
    lua_pop(L, 1);
    return v;
}

float Coord(lua_State* L, int t, const char* name, int index) {
    if (lua_getfield(L, t, name) == LUA_TNIL) {
        lua_pop(L, 1);
        lua_rawgeti(L, t, index);
    }
    if (!lua_isnil(L, -1) && lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "wum.audio.play: pos.%s must be a number", name);
    const double d = lua_isnil(L, -1) ? 0.0 : lua_tonumber(L, -1);
    lua_pop(L, 1);
    // Clamped before the cast (a double beyond float range is undefined behaviour). NaN stays NaN and plays silent.
    if (std::isnan(d)) return std::numeric_limits<float>::quiet_NaN();
    constexpr double kMax = std::numeric_limits<float>::max();
    return static_cast<float>(std::clamp(d, -kMax, kMax));
}

int Play(lua_State* L) {
    const lua_Integer clip = luaL_checkinteger(L, 1);
    au::PlayOpts o;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TTABLE);
        o.volume = Num(L, 2, "volume", 1.0f, 0.0f, 2.0f);
        o.pitch = Num(L, 2, "pitch", 1.0f, 0.5f, 2.0f);
        lua_getfield(L, 2, "loop");
        if (!lua_isnil(L, -1)) {
            if (!lua_isboolean(L, -1)) luaL_error(L, "wum.audio.play: loop must be a boolean");
            o.loop = lua_toboolean(L, -1) != 0;
        }
        lua_pop(L, 1);
        lua_getfield(L, 2, "pos");
        if (!lua_isnil(L, -1)) {
            if (lua_type(L, -1) != LUA_TTABLE) luaL_error(L, "wum.audio.play: pos must be a table {x, y, z}");
            const int t = lua_gettop(L);
            o.positional = true;
            o.pos[0] = Coord(L, t, "x", 1);
            o.pos[1] = Coord(L, t, "y", 2);
            o.pos[2] = Coord(L, t, "z", 3);
        }
        lua_pop(L, 1);
    }
    Gen* g = CurrentGen();
    if (!g) return Fail(L, "needs a mod context");
    if (clip <= 0 || clip > 0xFFFFFFFFll) return Fail(L, "unknown sound");
    Hook(g);
    std::string why;
    const uint32_t voice = au::Play(g, static_cast<uint32_t>(clip), o, &why);
    if (!voice) return Fail(L, why.c_str());
    lua_pushinteger(L, voice);
    return 1;
}

int Stop(lua_State* L) {
    const lua_Integer v = luaL_checkinteger(L, 1);
    lua_pushboolean(L, v > 0 && v <= 0xFFFFFFFFll && au::Stop(Owner(), static_cast<uint32_t>(v)));
    return 1;
}

int StopAll(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(au::StopAll(Owner())));
    return 1;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kAudio[] = {{"ready", Ready}, {"load", Load}, {"play", Play}, {"stop", Stop}, {"stopAll", StopAll},
                                      {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kAudio);
    lua_setfield(L, wum, "audio");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
