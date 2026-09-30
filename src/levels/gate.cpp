// The online map gate in the game: the host's start is held through starthold while the lobby's level is a mod level
// some member cannot play, the host names its pack level in "mlg.lvl", and both sides get a banner and panel rows.
#include "levels/gate.h"

#include <atomic>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "levels/engine.h"
#include "levels/registry.h"
#include "melange/jlog.h"
#include "mods/lobby.h"
#include "mods/lobbybanner.h"
#include "mods/starthold.h"
#include "mods/thumper_internal.h"
#include "net/net.h"

namespace melange::levels::gate {
namespace {
namespace eng = levels::engine;
namespace lobby = handshake::lobby;
namespace lobbybanner = mods::lobbybanner;

constexpr int kEvalEvery = 15;

bool g_online = true, g_installed = false;
Verdict g_v;
std::vector<std::string> g_joiner;
std::atomic<bool> g_match{true};
std::atomic<bool> g_inLobby{false};
bool g_wasHeld = false;
uint32_t g_held = 0;
std::string g_published;
int g_banner = 0;

bool Waiting() { return game::IsKnownBuild() && wum::CurrentState() == wum::state::WaitingGameStart; }

std::string ModVersion(const std::string& mod, bool* active) {
    thumper::Entry e;
    const bool found = !mod.empty() && thumper::FindEntry(mod, &e);
    if (active) *active = found && e.sessionActive;
    return found ? e.manifest.version : "";
}

Input Gather(bool owner) {
    Input in;
    in.inLobby = true;
    in.owner = owner;
    in.online = g_online;
    mods::Peer peers[16];
    const int n = mods::Peers(peers, 16);
    for (int i = 0; i < n && i < 16; ++i)
        in.members.push_back({peers[i].name, peers[i].status, lobby::MemberData(peers[i].steamId, "mlg.mods")});
    if (!owner) return in;
    in.key = eng::CurrentLevelKey();
    if (in.key.empty()) return in;
    in.known = eng::LevelDetails(in.key.c_str(), nullptr);
    LevelInfo info{};
    if (registry::Find(in.key.c_str(), &info)) {
        in.source = info.source;
        in.title = info.title;
        in.mod = info.mod;
        in.modVersion = ModVersion(in.mod, nullptr);
    }
    return in;
}

std::vector<std::string> JoinerLines() {
    std::vector<std::string> out;
    std::string mod, version, title;
    if (!ParseLevelValue(lobby::MemberData(lobby::Owner(), "mlg.lvl"), &mod, &version, &title)) return out;
    bool active = false;
    const std::string ours = ModVersion(mod, &active);
    if (active && ours == version) return out;
    const std::string map = title.empty() ? mod : title;
    out.push_back(active ? "The host picked " + map + " from " + mod + " " + version + "; you have " + mod + " " + ours + "."
                         : "The host picked " + map + ", a map from " + mod + " " + version + ", which you don't have.");
    return out;
}

void Publish(const Input& in) {
    const std::string v = in.source == Source::Pack ? LevelValue(in.mod, in.modVersion, in.title) : "";
    if (v == g_published) return;
    g_published = v;
    lobby::SetMyData("mlg.lvl", v.c_str());
}

void Evaluate(bool owner) {
    const Input in = Gather(owner);
    g_v = gate::Evaluate(in);
    g_match = AllMatch(in.members);
    g_joiner = owner ? std::vector<std::string>{} : JoinerLines();
    if (owner) Publish(in);
    if (g_v.hold != g_wasHeld) {
        g_wasHeld = g_v.hold;
        if (g_v.hold) {
            ++g_held;
            LOG_WARN("[levels] the match start is held: %s", g_v.why.c_str());
        } else {
            LOG_INFO("[levels] the match start is no longer held");
        }
        jlog::Rec("levels", g_v.hold ? jlog::Level::Warn : jlog::Level::Info, "start_hold")
            .Bool("held", g_v.hold).Str("key", in.key).Str("why", g_v.why);
    }
}

bool HoldReason(std::string* why, void*) {
    if (!g_v.hold) return false;
    if (why) *why = g_v.why;
    return true;
}

void UpdateBanner() {
    constexpr uint32_t kAmber = 0xff30a0ffu;
    if (!g_banner) return;
    if (!g_inLobby.load()) lobbybanner::Set(g_banner, kAmber, "", {});
    else if (g_v.hold) lobbybanner::Set(g_banner, kAmber, "The match cannot start on this map:", {g_v.why});
    else if (!g_joiner.empty())
        lobbybanner::Set(g_banner, kAmber, g_joiner.front(), std::vector<std::string>(g_joiner.begin() + 1, g_joiner.end()));
    else lobbybanner::Set(g_banner, kAmber, "", {});
}
}  // namespace

void Install(bool online) {
    if (g_installed) return;
    g_installed = true;
    g_online = online;
    mods::starthold::Add("levels", &HoldReason, nullptr);
    g_banner = lobbybanner::Add("levels", 20);
}

void Tick() {
    UpdateBanner();
    const uint64_t l = lobby::Current();
    g_inLobby = l != 0;
    if (!l) {
        if (g_v.status != Online::NotInLobby || g_v.hold || !g_joiner.empty()) {
            g_v = Verdict{};
            g_joiner.clear();
            g_wasHeld = false;
        }
        g_match = true;
        g_published.clear();
        return;
    }
    const bool owner = lobby::Owner() != 0 && lobby::Owner() == lobby::Me();
    if (!(owner && Waiting()) && events::FrameCount() % kEvalEvery) return;
    Evaluate(owner);
}

Verdict Current() { return g_v; }
bool InLobby() { return lobby::Current() != 0; }
bool MembersMatch() { return g_match.load(); }
uint32_t HeldStarts() { return g_held; }

std::vector<std::string> LobbyLines() {
    std::vector<std::string> out;
    if (g_v.hold) out.push_back("Mod map held: " + g_v.why);
    for (const auto& j : g_joiner) out.push_back(j);
    return out;
}
}  // namespace melange::levels::gate
