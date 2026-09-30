#pragma once
#include <string>
#include <vector>

#include "melange/levels.h"
#include "melange/mods.h"

// The online map gate: which mod levels a picker or random pool may offer, and whether the host's start is held.
namespace melange::levels::gate {
// Pure policy (gate_policy.cpp).
struct Member {
    std::string name;
    mods::PeerStatus status = mods::PeerStatus::Unknown;
    std::string mods;   // the member's "mlg.mods" ("id@version,...")
};
struct Input {
    bool inLobby = false, owner = false;
    bool online = true;          // [Levels] Online
    std::string key;             // the lobby's level (WXD.Level.Current)
    bool known = true;           // a WXFE_LevelDetails of that name exists on this host
    Source source = Source::Vanilla;
    bool live = false;           // the pack was enabled or disabled at the menu this session
    std::string title, mod, modVersion;
    std::vector<Member> members;  // everyone but us
};
struct Verdict {
    bool hold = false;
    Online status = Online::NotInLobby;
    std::string why;                    // one line, for the log and the start-hold reason
    std::vector<std::string> members;   // "<name> doesn't have <pack>" per offending member
};
Verdict Evaluate(const Input& in);
bool AllMatch(const std::vector<Member>& members);
bool KeepInList(Source s, bool inLobby, bool online, bool allMatch, bool live = false);   // the landscape picker
bool KeepInPool(Source s, bool randomPool);                            // Quick Game / lobby random pools
bool HasMod(const std::string& modsValue, const std::string& id, const std::string& version);
// The host's "mlg.lvl" member value naming the pack of the lobby's level ("" for a vanilla level), and its parse.
std::string LevelValue(const std::string& mod, const std::string& version, const std::string& title);
bool ParseLevelValue(const std::string& v, std::string* mod, std::string* version, std::string* title);

// Game side (gate.cpp). Main thread.
void Install(bool online);           // start-hold reason, banner, member data
void Tick();                         // from registry::OnFrame
Verdict Current();
bool InLobby();                      // any thread
bool MembersMatch();                 // every other lobby member's content matches ours (last evaluation; any thread)
std::vector<std::string> LobbyLines();   // rows for the lobby panel (host and joiner)
uint32_t HeldStarts();
}  // namespace melange::levels::gate
