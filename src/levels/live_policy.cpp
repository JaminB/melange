#include "levels/live.h"

namespace melange::levels::live {
std::string SessionRefusal(const Session& s) {
    if (!s.ini) return "live pack changes are off ([Levels] LivePacks=0); restart the game";
    if (!s.enabled) return "[Levels] is disabled";
    if (s.inLobby || s.netSession) return "packs cannot change in a lobby or a network game";
    if (s.attract || s.loading) return "the game is loading a level; try again in a moment";
    if (!s.atFrontend) return "packs can change only at the main menu";
    if (s.testBusy) return "a Test is under way; try again when it ends";
    return "";
}

std::string PackRefusal(const spice::Manifest& m) {
    if (m.levels.empty()) return "the mod has no levels";
    const char* need = nullptr;
    if (!m.entrySim.empty()) need = "entry.sim";
    else if (!m.entryClient.empty() || m.unsafe) need = "client code";
    else if (!m.messages.empty()) need = "messages";
    else if (!m.weapons.empty()) need = "weapons";
    else if (m.filesystem != "none") need = "file overrides";
    else
        for (const auto& l : m.levels)
            if (!l.sim.empty()) need = "level scripts";
    return need ? std::string("the mod has ") + need + "; restart required" : "";
}

std::string OnlineWhy(const std::string& map) {
    return map + " was changed at the menu this session and plays offline only until a restart";
}

std::string Badge(bool on) {
    return on ? "enabled for this session (offline only until restart)" : "disabled for this session until restart";
}
}  // namespace melange::levels::live
