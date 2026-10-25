// wum.media: watch-party playback for client mods (the media pack, players, resolve, torrents). Presentation only, in
// the client Sandbox VM. The work is done by src/media/* (media.h, pack.h); this file scopes players and torrents to the
// calling mod generation.
// Placeholder from Melange 0.12.0 C0: classify is complete (url_logic.h), everything else reports "unavailable".
// C4b replaces the rest of this file.
#include <string>

#include "lua/sandbox_core.h"
#include "media/url_logic.h"

namespace melange::sandbox {
namespace {
namespace url = melange::media::url;

int Fail(lua_State* L, const char* why) {
    lua_pushnil(L);
    lua_pushstring(L, why);
    return 2;
}

int Unavailable(lua_State* L) { return Fail(L, "unavailable"); }

int Available(lua_State* L) {
    lua_pushboolean(L, false);
    lua_pushstring(L, "unavailable");
    return 2;
}

int Pack(lua_State* L) {
    lua_createtable(L, 0, 2);
    lua_pushstring(L, "disabled");
    lua_setfield(L, -2, "state");
    lua_pushstring(L, "");
    lua_setfield(L, -2, "version");
    return 1;
}

int CancelInstall(lua_State* L) {
    lua_pushboolean(L, true);
    return 1;
}

void SetStr(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, k);
}

int Classify(lua_State* L) {
    size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    url::Classified c;
    if (!url::Classify(std::string_view(s, n), &c)) return Fail(L, "unsupported-url");
    lua_createtable(L, 0, 4);
    SetStr(L, "url", c.url);
    if (c.kind == url::Kind::YouTube) {
        SetStr(L, "kind", "youtube");
        if (!c.yt.id.empty()) SetStr(L, "id", c.yt.id);
        if (!c.yt.playlist.empty()) SetStr(L, "playlist", c.yt.playlist);
        if (c.yt.live) {
            lua_pushboolean(L, true);
            lua_setfield(L, -2, "live");
        }
    } else {
        SetStr(L, "kind", "magnet");
        SetStr(L, "infohash", c.bt.infohash);
        if (!c.bt.name.empty()) SetStr(L, "name", c.bt.name);
    }
    return 1;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kMedia[] = {{"available", Available}, {"pack", Pack},       {"install", Unavailable},
                                      {"cancelInstall", CancelInstall},                {"classify", Classify},
                                      {"resolve", Unavailable}, {"torrent", Unavailable}, {"open", Unavailable},
                                      {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kMedia);
    lua_setfield(L, wum, "media");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
