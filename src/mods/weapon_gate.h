#pragma once
#include <string>

// The clone gate: may this lobby's members play the local clones? Called by the handshake's gate.
namespace melange::mods {
bool CloneLobbyOk(std::string* why);
}
