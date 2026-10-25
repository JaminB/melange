#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The lobby as Handshake tracks it, for other Melange features that talk to lobby members. Calls the Steam
// matchmaking interface directly; installs nothing. Any thread.
namespace melange::handshake::lobby {
uint64_t Current();                                   // 0 outside a lobby
uint64_t Me();
uint64_t Owner();                                     // 0 outside a lobby
std::vector<uint64_t> Members();                      // every member other than us
std::string MemberData(uint64_t member, const char* key);
void SetMyData(const char* key, const char* value);   // no-op outside a lobby
std::string Name(uint64_t member);                    // persona name, or the id as text
std::string Data(const char* key);                    // lobby-level data, "" outside a lobby

// Melange 0.12 (the peer bus, net/peerbus.cpp). One observer, told what Handshake's Steam listeners see, from Steam's
// callback dispatch (the game's main thread): Enter (LobbyEnter), Left (kicked, or the net session closed), Members
// (someone joined or left; also how an owner change shows), MemberData (a member's data changed; `member` says who).
enum class Change : uint8_t { Enter, Left, Members, MemberData };
using ChangeFn = void (*)(Change c, uint64_t member);
void OnChange(ChangeFn fn);
// This machine's mlg.peer member value ("1;<modId>@<version>,..." or ""): written now when in a lobby and again on
// every LobbyEnter. Not part of mlg.mods or the content hash, and written even with [Handshake] Publish=0 (it names
// only client mods that opened a peer channel). Any thread.
void SetPeerAdvert(const std::string& value);
}  // namespace melange::handshake::lobby
