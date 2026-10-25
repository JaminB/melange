// sandbox_selftest fakes for wum.lobby / wum.peer (owner: C3): link-time stand-ins for peerbus:: and the lobby, so
// wum_peer.cpp runs without Steam. Included inside namespace melange, after the other fakes.
namespace fake_peer {
inline double g_nowMs = 5000.0;
}
namespace peerbus {
double NowMs() { return fake_peer::g_nowMs += 0.25; }
bool Clock(uint64_t, ClockInfo*) { return false; }
}  // namespace peerbus
