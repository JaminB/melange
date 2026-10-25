#pragma once
// What wum_peer.cpp (wum.lobby, wum.peer) calls on the peer bus, Melange 0.12.0. Main thread unless stated. Implemented
// by net/peerbus.cpp in the game and by tests/sandbox_fakes_peer.inc in sandbox_selftest. peerbus.h keeps the two
// functions other subsystems use (NowMs, Clock); this header is the Lua binding's own.
#include <cstdint>
#include <string>
#include <vector>

#include "net/peerbus_core.h"

namespace melange::peerbus {
bool Available();  // the [PeerBus] module is installed (Enabled=1)

// ---- the lobby as the bus last polled it
struct LobbyMember {
    uint64_t id = 0;
    std::string name;
    std::string melange;  // its Melange version from mlg, "" without Melange
    std::string advert;   // its mlg.peer
    bool me = false, host = false;
};
struct LobbyView {
    uint64_t lobby = 0, me = 0, owner = 0;
    std::string phase;                  // "lobby", "loading", "match", "results"
    std::vector<LobbyMember> members;   // me included
};
bool Lobby(LobbyView* out);  // false outside a net session

// ---- channels (ids are never reused)
ChannelId Open(const std::string& modId, int version, std::string* why);
void Close(ChannelId ch);
SendStatus Send(ChannelId ch, uint64_t to, const std::vector<uint8_t>& payload, bool reliable);
int Broadcast(ChannelId ch, const std::vector<uint8_t>& payload, bool reliable);
std::vector<uint64_t> Peers(ChannelId ch);
// mlg.p.<modId>: `json` (<= wire::kMaxPresence bytes) is coalesced to one write per 2 s; "" clears it.
bool SetPresence(ChannelId ch, const std::string& json);
std::string Presence(ChannelId ch, uint64_t member);  // a member's raw value, "" when none or not a member

// ---- delivery from the bus's frame (main thread, outside any Lua call)
enum class LobbyEvent : uint8_t { Enter, Leave, Members, Owner, Phase };
struct Hooks {
    void (*message)(const Delivery& d) = nullptr;
    void (*peer)(const PeerChange& c) = nullptr;
    void (*lobby)(LobbyEvent e) = nullptr;
};
void SetHooks(const Hooks& h);
}  // namespace melange::peerbus
