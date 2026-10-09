#pragma once
#include <string>
#include <vector>

// Game-file integrity at runtime ([Handshake] PeerIntegrity): hashes the exe and the retail data files once on a
// background thread, publishes "mlg.gid" and warns (lobby banner, Thumper/Lobby panel) when another Melange peer's
// game files differ. Warning only: it never holds a start and never touches the content hash.
namespace melange::handshake::gid {
void Install(bool enabled, bool publish);
bool Enabled();
std::string OurValue();                  // "" until the background hash finishes (or when it failed)
std::vector<std::string> Warnings();     // "<name>: <what differs>" per mismatched Melange peer, main thread
}  // namespace melange::handshake::gid
