#pragma once
#include <string>
#include <vector>

#include "levels/session.h"
#include "mods/spice.h"

// Map packs enabled or disabled at the menu, offline only. The policy is pure (live_policy.cpp); live.cpp runs it.
namespace melange::levels::live {
struct Session : session::State {
    bool ini = false;          // [Levels] LivePacks
    bool enabled = false;      // the Levels module is active
};
std::string SessionRefusal(const Session& s);   // "" when a live change may run now

// Only a pack of levels can change live: sims, messages, weapons and client code need a restart.
std::string PackRefusal(const spice::Manifest& m);

// The pack's levels leave the network lists and hold a start: its content set was frozen at launch.
std::string OnlineWhy(const std::string& map);

// "enabled for this session (offline only until restart)" and its disable twin.
std::string Badge(bool on);

void Install();   // game side: runs queued changes each frame
}  // namespace melange::levels::live
