// wum.lobby and wum.peer: the Steam lobby as client mods see it, and per-mod message channels on the peer bus
// (Steam P2P channel 6). Presentation only, in the client Sandbox VM; nothing here touches the sim, channel 0 or
// Wormsign. The work is done by src/net/peerbus.cpp; this file scopes channels to the calling mod generation.
// Placeholder from Melange 0.12.0 C0: there is never a lobby and no channel opens; wum.peer.now() is the real clock.
// C3 replaces the rest of this file.
#include "lua/sandbox_core.h"
#include "net/peerbus.h"

namespace melange::sandbox {
namespace {
int LobbyState(lua_State* L) {
    lua_pushnil(L);
    return 1;
}

int LobbyIsHost(lua_State* L) {
    lua_pushboolean(L, false);
    return 1;
}

int PeerOpen(lua_State* L) {
    lua_pushnil(L);
    lua_pushstring(L, "unavailable");
    return 2;
}

int PeerNow(lua_State* L) {
    lua_pushnumber(L, peerbus::NowMs());
    return 1;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kLobby[] = {{"state", LobbyState}, {"isHost", LobbyIsHost}, {nullptr, nullptr}};
    static const luaL_Reg kPeer[] = {{"open", PeerOpen}, {"now", PeerNow}, {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kLobby);
    lua_setfield(L, wum, "lobby");
    lua_newtable(L);
    RegisterFunctions(L, -1, kPeer);
    lua_setfield(L, wum, "peer");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
