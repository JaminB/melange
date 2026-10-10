#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The clone gate: may this lobby's members play the local clones? Called by the handshake's gate.
namespace melange::mods {
bool CloneLobbyOk(std::string* why);
}

// The weapon handshake around it: the host's start refusal, the lobby banner and the joiner's modal.
namespace melange::handshake::wpngate {
enum class Policy : uint8_t { Refuse, Suspend };
void Install(Policy policy, bool leaveButton);   // from the Handshake module
Policy CurrentPolicy();
bool LocalClones();                              // this peer declares live-capable clones, vanilla weapon renames (weaponText) or icon replacements (weaponIcons)

struct View {
    bool inLobby = false, owner = false;
    bool hostHeld = false;          // we host with clones and a member does not match
    bool refusing = false;          // ... and the start is being refused
    bool joinerMismatch = false;
    std::string why;
    std::vector<std::string> members;
};
View Current();                                  // the last evaluation (main thread)

// The joiner's modal (drawn by the lobby panel).
bool TakeModalRequest();                         // true once per (lobby, mlg.req) mismatch
bool LeaveAvailable();
void RequestLeave();                             // runs on the next frame, only on a joiner in the lobby
void ModalClosed();
}  // namespace melange::handshake::wpngate

namespace melange::handshake {
std::string PeerModsDiff(uint64_t steamId);      // handshake.cpp: what differs in mlg.mods
std::string PeerMsgDiff(uint64_t steamId);       // handshake.cpp: what differs in mlg.msg
}
