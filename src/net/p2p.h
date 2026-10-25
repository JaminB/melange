#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Steam P2P packets on any channel but 0, through the game's own steam_api.dll (SteamNetworking005), main thread.
// The channel-generic twin of wormsign/exchange.cpp (which stays as it is, on channel 5); the peer bus uses it on 6.
// The game only uses and drains channel 0; nothing here touches it.
namespace melange::net::p2p {
enum class SendType : int { Unreliable = 0, UnreliableNoDelay = 1, Reliable = 2 };
bool Send(uint64_t to, const std::vector<uint8_t>& packet, SendType type, int channel);
using RecvFn = void (*)(uint64_t from, const uint8_t* p, size_t n, void* user);
// Reads at most `max` packets from `channel`; a packet over `maxBytes` is read (so it never piles up) and dropped,
// counted in *oversized. Returns the packets read.
int Drain(int channel, size_t maxBytes, RecvFn fn, void* user, int max, uint64_t* oversized);
// GetP2PSessionState(member).connectionActive: the game (or anyone) already has a live P2P session with the member.
bool SessionActive(uint64_t member);
bool Available();  // steam_api.dll is loaded and SteamNetworking() answers
}  // namespace melange::net::p2p
