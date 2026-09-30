#include "levels/gate.h"

namespace melange::levels::gate {
namespace {
std::string Name(const std::string& title, const std::string& key) { return title.empty() ? key : title; }
}  // namespace

bool AllMatch(const std::vector<Member>& members) {
    for (const auto& m : members)
        if (m.status != mods::PeerStatus::Match) return false;
    return true;
}

bool KeepInList(Source s, bool inLobby, bool online, bool allMatch) {
    if (s == Source::Vanilla || !inLobby) return true;
    if (s == Source::Test) return false;
    return online && allMatch;
}

bool KeepInPool(Source s, bool randomPool) {
    if (s == Source::Vanilla) return true;
    return s == Source::Pack && randomPool;
}

bool HasMod(const std::string& modsValue, const std::string& id, const std::string& version) {
    const std::string want = id + "@" + version;
    size_t start = 0;
    while (start <= modsValue.size()) {
        size_t end = modsValue.find(',', start);
        if (end == std::string::npos) end = modsValue.size();
        if (modsValue.compare(start, end - start, want) == 0) return true;
        start = end + 1;
    }
    return false;
}

std::string LevelValue(const std::string& mod, const std::string& version, const std::string& title) {
    if (mod.empty()) return "";
    return "1;" + mod + ";" + version + ";" + title.substr(0, 40);
}

bool ParseLevelValue(const std::string& v, std::string* mod, std::string* version, std::string* title) {
    if (v.rfind("1;", 0) != 0) return false;
    const size_t a = v.find(';', 2);
    if (a == std::string::npos) return false;
    const size_t b = v.find(';', a + 1);
    if (b == std::string::npos || a == 2) return false;
    if (mod) *mod = v.substr(2, a - 2);
    if (version) *version = v.substr(a + 1, b - a - 1);
    if (title) *title = v.substr(b + 1);
    return true;
}

Verdict Evaluate(const Input& in) {
    Verdict v;
    if (!in.inLobby) return v;
    v.status = Online::Allowed;
    if (in.key.empty()) return v;
    const std::string map = Name(in.title, in.key);
    if (!in.known) {
        v.status = Online::NotAllMatch;
        v.why = in.key + " is not a level registered in this game";
    } else if (in.source == Source::Vanilla) {
        return v;
    } else if (in.source == Source::Test) {
        v.status = Online::TestLevel;
        v.why = map + " is an Erg Test level, which never starts online";
    } else if (!in.online) {
        v.status = Online::NotAllMatch;
        v.why = map + " is a mod map, and mod maps are off online ([Levels] Online=0)";
    } else {
        for (const auto& m : in.members) {
            if (m.status == mods::PeerStatus::Match) continue;
            const std::string who = m.name.empty() ? "a player" : m.name;
            v.members.push_back(HasMod(m.mods, in.mod, in.modVersion) ? who + "'s mods differ from ours"
                                                                       : who + " doesn't have " + in.mod);
        }
        if (v.members.empty()) return v;
        v.status = Online::NotAllMatch;
        v.why = map + " is a mod map; ";
        for (size_t i = 0; i < v.members.size(); ++i) v.why += (i ? ", " : "") + v.members[i];
    }
    v.hold = in.owner;
    return v;
}
}  // namespace melange::levels::gate
