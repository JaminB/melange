#pragma once
#include <string>

// Whether the game is somewhere a change to the installed content may run now: shared by live map packs and the Store.
namespace melange::session {
struct State {
    bool inLobby = false, netSession = false;
    bool atFrontend = false;
    bool attract = false;      // the attract demo is running
    bool loading = false;      // a level set-up has not reached its match
    bool testBusy = false;     // an Erg Test is armed, starting or playing
};
enum class For { Packs, Plugins };
std::string ChangeRefusal(const State& s, For what);   // "" when allowed

State Now();   // game side, main thread (levels/live.cpp)
}  // namespace melange::session
