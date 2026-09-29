#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// The hash exchange's transport: Steam P2P channel 5 through the game's own steam_api.dll, main thread. The game
// only uses and drains channel 0; nothing here touches it.
namespace melange::wormsign::exchange {
bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable);
using RecvFn = void (*)(uint64_t from, const uint8_t* p, size_t n, void* user);
int Drain(RecvFn fn, void* user, int max);      // packets read (oversized ones are read and dropped)

struct Counters {
    uint64_t sent, sendFailed, bytesSent, received, bytesReceived, oversized;
};
Counters Stats();
}  // namespace melange::wormsign::exchange
