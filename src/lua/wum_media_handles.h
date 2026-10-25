#pragma once
// The Lua-side shape of a wum.media player, shared by wum_media.cpp (which creates players) and wum_render.cpp
// (wum.draw.media, which draws them). Frozen for Melange 0.12.0: a player is a full userdata with this metatable and
// this layout, so any binding can recognise one without knowing the rest of wum.media.
#include <lua.hpp>

#include <cstdint>

namespace melange::sandbox::mediaud {
inline constexpr const char kPlayerMeta[] = "melange.media.player";
inline constexpr const char kTorrentMeta[] = "melange.media.torrent";

struct PlayerUd {
    uint32_t id;      // media::* player id; 0 once closed
    uint32_t serial;  // the creating generation's Gen::serial (a reload revokes it)
};
struct TorrentUd {
    uint32_t id;
    uint32_t serial;
};

// The player at `idx`, or nullptr if the value is not a player (closed players are returned with id 0).
inline PlayerUd* TestPlayer(lua_State* L, int idx) { return static_cast<PlayerUd*>(luaL_testudata(L, idx, kPlayerMeta)); }
inline TorrentUd* TestTorrent(lua_State* L, int idx) { return static_cast<TorrentUd*>(luaL_testudata(L, idx, kTorrentMeta)); }
}  // namespace melange::sandbox::mediaud
