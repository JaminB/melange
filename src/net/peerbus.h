#pragma once
// The peer bus: Steam P2P channel 6 for client mods (wum.peer / wum.lobby). Written as a contract by Melange 0.12.0
// C0; C3 implements it (peerbus.cpp, peerbus_wire.h, p2p.h/.cpp, wum_peer.cpp). The functions below are the ones
// OTHER subsystems call (C4b's p:follow); the rest is C3's own.
#include <cstdint>

namespace melange::peerbus {
// Monotonic milliseconds (QueryPerformanceCounter), the clock of wum.peer.now() and of every bus timestamp. Any thread.
double NowMs();

// The bus's clock estimate for one lobby member: offset = their NowMs() - mine (from the minimum-RTT sample of the
// last 8), rtt smoothed with EWMA 0.85. False when the member is unknown or has no sample yet. Main thread.
struct ClockInfo {
    double offsetMs = 0, rttMs = 0;
    int samples = 0;
};
bool Clock(uint64_t steamId, ClockInfo* out);
}  // namespace melange::peerbus
