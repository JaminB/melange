#pragma once
// Test-only access to the libtorrent engine (bt_libtorrent.cpp). Nothing in the helper's pipe path calls this; the
// offline engine test (tests/bt_test.cpp) uses it to join a seeder on 127.0.0.1, since the engine itself never adds
// a loopback tracker or peer.
#include <string>

#include "bt/bt.h"

namespace holo::bt::testing {
bool ConnectPeer(Engine* e, const std::string& ih, const std::string& ip, int port);
// Sessions started after this listen on 127.0.0.1 only, with DHT and LSD off (no firewall prompt, no traffic).
void LoopbackOnly(bool on);
// The tracker vetting Add applies to every magnet tracker (blocking: may resolve a name).
bool TrackerAllowed(const std::string& tracker);
// Trackers and url seeds a torrent currently has (-1 for an unknown torrent).
int TrackerCount(Engine* e, const std::string& ih);
int WebSeedCount(Engine* e, const std::string& ih);
}  // namespace holo::bt::testing
