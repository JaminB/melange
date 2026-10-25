// The peer bus on Steam P2P channel 6 (peerbus.h, peerbus_api.h): module [PeerBus], Melange 0.12.0.
// Every frame (main thread) it polls the lobby, drains channel 6 (always, even with no channel open, so Steam never
// buffers it), runs the state machine in peerbus_core.cpp and hands messages, peer changes and lobby events to
// wum_peer.cpp. It sends only to members whose mlg.peer lists the same mod and version and with whom Steam already
// has a live P2P session, so a Melange packet is never the first traffic between two machines (the game's listener
// at 0x785290 latches onto the first session request it sees). Never channel 0, never the sim, never a Wormsign
// contributor. SteamIDs stay out of the log.
#include "net/peerbus.h"

#include <windows.h>
#include <imgui.h>

#include <atomic>
#include <map>
#include <set>

#include "core/events.h"
#include "core/module.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "mods/handshake_internal.h"
#include "mods/lobby.h"
#include "net/net.h"
#include "net/p2p.h"
#include "net/peerbus_api.h"
#include "version.h"
#include "wormsign/exchange_wire.h"

namespace melange::peerbus {
double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

namespace {
namespace lobby = handshake::lobby;
using net::p2p::SendType;
using ull = unsigned long long;

constexpr double kLobbyPollMs = 500;
constexpr double kPresenceEveryMs = 2000;

class SteamIo final : public Io {
  public:
    bool SessionActive(uint64_t member) override { return net::p2p::SessionActive(member); }
    bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable, bool noDelay) override {
        const SendType t = reliable ? SendType::Reliable : noDelay ? SendType::UnreliableNoDelay : SendType::Unreliable;
        return net::p2p::Send(to, packet, t, wire::kChannel);
    }
};

SteamIo g_io;
Core g_core(&g_io);
bool g_installed = false;
Hooks g_hooks;
std::atomic<bool> g_dirty{true};
uint64_t g_oversized = 0;

LobbyView g_view;  // as of the last poll
bool g_inSession = false;
wum::SessionPhase g_phase = wum::SessionPhase::None;
double g_lastPoll = -1e18;
std::string g_advert;

struct PresenceRec {
    std::string value;
    bool pending = false;
    double lastWrite = -1e18;
};
std::map<std::string, PresenceRec> g_presence;  // by mod id
std::map<ChannelId, std::string> g_channels;    // channel -> mod id

// A Steam lobby before the net session starts is still "the lobby" to a mod.
const char* PhaseText(wum::SessionPhase p) { return p == wum::SessionPhase::None ? "lobby" : wum::PhaseName(p); }

void OnLobbyChange(lobby::Change, uint64_t) { g_dirty = true; }

void Fire(LobbyEvent e) {
    if (g_hooks.lobby) g_hooks.lobby(e);
}

void PublishAdvert() {
    const std::string a = g_core.Advert();
    if (a == g_advert) return;
    g_advert = a;
    lobby::SetPeerAdvert(a);
}

// Reads the lobby (local Steam calls) and feeds the state machine; fires the melange.lobby.* events.
void PollLobby(double now) {
    const uint64_t lobbyId = lobby::Current();
    const wum::SessionPhase phase = wum::Phase();
    const bool inSession = lobbyId != 0 || phase != wum::SessionPhase::None;
    const bool due = g_dirty.exchange(false) || now - g_lastPoll >= kLobbyPollMs || lobbyId != g_view.lobby;
    if (due) {
        g_lastPoll = now;
        LobbyView v;
        LobbyInfo info;
        if (lobbyId) {
            v.lobby = info.lobby = lobbyId;
            v.me = info.me = lobby::Me();
            v.owner = info.owner = lobby::Owner();
            info.key = v.owner ? wormsign::wire::MatchKey(v.owner, lobbyId) : 0;
            LobbyMember self;
            self.id = v.me;
            self.name = lobby::Name(v.me);
            self.melange = MELANGE_VERSION;
            self.advert = g_advert;
            self.me = true;
            self.host = v.me == v.owner;
            v.members.push_back(std::move(self));
            for (uint64_t id : lobby::Members()) {
                LobbyMember m;
                m.id = id;
                m.name = lobby::Name(id);
                std::string version, hash16;
                uint32_t n = 0;
                if (handshake::ParseMlgValue(lobby::MemberData(id, "mlg"), &version, &hash16, &n)) m.melange = version;
                m.advert = lobby::MemberData(id, wire::kAdvertKey);
                m.host = id == v.owner;
                info.members.push_back({id, m.advert});
                v.members.push_back(std::move(m));
            }
        }
        const uint64_t oldLobby = g_view.lobby, oldOwner = g_view.owner, oldKey = g_core.Key();
        std::set<uint64_t> oldIds, newIds;
        for (const auto& m : g_view.members) oldIds.insert(m.id);
        for (const auto& m : v.members) newIds.insert(m.id);
        g_core.SetLobby(info, now);
        if (oldKey && oldKey != g_core.Key())
            jlog::Rec("peerbus", jlog::Level::Info, "reset").Bool("online", g_core.Online()).Emit();
        g_view = std::move(v);
        if (lobbyId != oldLobby) {
            if (oldLobby) Fire(LobbyEvent::Leave);
            if (lobbyId) {
                for (auto& [mod, p] : g_presence) p.pending = !p.value.empty();  // member data does not carry over
                Fire(LobbyEvent::Enter);
            }
        } else if (lobbyId) {
            if (g_view.owner != oldOwner) Fire(LobbyEvent::Owner);
            if (newIds != oldIds) Fire(LobbyEvent::Members);
        }
    }
    g_view.phase = PhaseText(phase);
    if (inSession && g_inSession && phase != g_phase) Fire(LobbyEvent::Phase);
    g_inSession = inSession;
    g_phase = phase;
}

bool ModOpen(const std::string& mod) {
    for (const auto& [id, m] : g_channels)
        if (m == mod) return true;
    return false;
}

void WritePresence(double now) {
    if (!lobby::Current()) return;
    for (auto it = g_presence.begin(); it != g_presence.end();) {
        PresenceRec& p = it->second;
        if (p.pending && now - p.lastWrite >= kPresenceEveryMs) {
            lobby::SetMyData((std::string(wire::kPresencePrefix) + it->first).c_str(), p.value.c_str());
            p.pending = false;
            p.lastWrite = now;
        }
        if (!ModOpen(it->first) && !p.pending && p.value.empty()) it = g_presence.erase(it);
        else ++it;
    }
}

void OnPacket(uint64_t from, const uint8_t* p, size_t n, void* user) { g_core.OnPacket(from, p, n, *static_cast<double*>(user)); }

void OnFrame() {
    double now = NowMs();
    PollLobby(now);
    net::p2p::Drain(wire::kChannel, wire::kMaxPacket, &OnPacket, &now, wire::kMaxPacketsPerFrame, &g_oversized);
    g_core.Tick(now);
    WritePresence(now);
    for (const PeerChange& c : g_core.TakePeerChanges())
        if (g_hooks.peer && g_channels.count(c.ch)) g_hooks.peer(c);
    for (const Delivery& d : g_core.TakeDeliveries())
        if (g_hooks.message && g_channels.count(d.ch)) g_hooks.message(d);
}

// ---------------------------------------------------------------- Melange/Peers panel
void DrawPanel(void*) {
    const Counters& c = g_core.Stats();
    ImGui::Text("Channel %d: %s", wire::kChannel, g_core.Online() ? "in a lobby" : "not in a lobby");
    ImGui::Text("Packets in %llu (%llu B), out %llu (%llu B), send failed %llu", (ull)c.packetsIn, (ull)c.bytesIn,
                (ull)c.packetsOut, (ull)c.bytesOut, (ull)c.sendFailed);
    ImGui::TextDisabled("Dropped: non-member %llu, not ours %llu, protocol %llu, malformed %llu, stale lobby %llu, "
                        "no channel %llu, no advert %llu, rate %llu, reassembly %llu, value %llu, oversized %llu",
                        (ull)c.dropNonMember, (ull)c.dropNotOurs, (ull)c.dropBadProtocol, (ull)c.dropMalformed,
                        (ull)c.dropStaleKey, (ull)c.dropNoChannel, (ull)c.dropNoAdvert, (ull)c.dropRate,
                        (ull)c.dropReassembly, (ull)c.dropValue, (ull)g_oversized);
    ImGui::TextDisabled("Resets %llu, stalls %llu", (ull)c.resets, (ull)c.stalls);
    const std::vector<ChannelStats> chans = g_core.Channels();
    if (chans.empty()) {
        ImGui::TextDisabled("No mod has a channel open.");
    } else if (ImGui::BeginTable("peerbus.mods", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Mod");
        ImGui::TableSetupColumn("Sent");
        ImGui::TableSetupColumn("Received");
        ImGui::TableSetupColumn("Refused");
        ImGui::TableSetupColumn("Dropped");
        ImGui::TableHeadersRow();
        for (const ChannelStats& s : chans) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s@%d", s.modId.c_str(), s.version);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (ull)s.sent);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (ull)s.received);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (ull)s.refused);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (ull)s.dropped);
        }
        ImGui::EndTable();
    }
    if (g_view.members.size() > 1 &&
        ImGui::BeginTable("peerbus.members", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Member");
        ImGui::TableSetupColumn("Advertises");
        ImGui::TableSetupColumn("RTT ms");
        ImGui::TableSetupColumn("Offset ms");
        ImGui::TableHeadersRow();
        for (const LobbyMember& m : g_view.members) {
            if (m.me) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", m.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%s", m.advert.empty() ? "-" : m.advert.c_str());
            ClockView cv;
            const bool have = g_core.Clock(m.id, &cv);
            ImGui::TableNextColumn();
            if (have) ImGui::Text("%.1f", cv.rttMs);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (have) ImGui::Text("%+.1f (%d)", cv.offsetMs, cv.samples);
            else ImGui::TextDisabled("-");
        }
        ImGui::EndTable();
    }
}

class PeerBus final : public Module {
public:
    const char* Name() const override { return "PeerBus"; }
    const char* Description() const override { return "Steam P2P channel 6 for client mods (wum.peer)"; }
    int Order() const override { return 58; }  // after Handshake (57), which publishes mlg.peer
    bool Install() override {
        g_installed = true;
        lobby::OnChange(&OnLobbyChange);
        events::Subscribe(events::Event::Frame, [] { OnFrame(); });
        overlay::AddPanel("melange.peers", "Melange/Peers", &DrawPanel, nullptr);
        return true;
    }
};
}  // namespace
MELANGE_MODULE(PeerBus);

// ---------------------------------------------------------------- peerbus.h / peerbus_api.h
bool Clock(uint64_t steamId, ClockInfo* out) {
    ClockView v;
    if (!out || !g_core.Clock(steamId, &v)) return false;
    out->offsetMs = v.offsetMs;
    out->rttMs = v.rttMs;
    out->samples = v.samples;
    return true;
}

bool Available() { return g_installed; }

bool Lobby(LobbyView* out) {
    if (!g_installed || !g_inSession) return false;
    *out = g_view;
    return true;
}

ChannelId Open(const std::string& modId, int version, std::string* why) {
    if (!g_installed) {
        if (why) *why = "unavailable";
        return 0;
    }
    const ChannelId id = g_core.Open(modId, version, why);
    if (!id) return 0;
    g_channels[id] = modId;
    PublishAdvert();
    jlog::Rec("peerbus", jlog::Level::Info, "open").Str("mod", modId).Int("version", version).Emit();
    return id;
}

void Close(ChannelId ch) {
    auto it = g_channels.find(ch);
    if (it == g_channels.end()) return;
    const std::string mod = it->second;
    g_core.Close(ch);
    g_channels.erase(it);
    PublishAdvert();
    auto p = g_presence.find(mod);
    if (p != g_presence.end() && !p->second.value.empty()) {
        p->second.value.clear();
        p->second.pending = true;
    }
    jlog::Rec("peerbus", jlog::Level::Info, "close").Str("mod", mod).Emit();
}

SendStatus Send(ChannelId ch, uint64_t to, const std::vector<uint8_t>& payload, bool reliable) {
    return g_core.Send(ch, to, payload, reliable, NowMs());
}

int Broadcast(ChannelId ch, const std::vector<uint8_t>& payload, bool reliable) {
    return g_core.Broadcast(ch, payload, reliable, NowMs());
}

std::vector<uint64_t> Peers(ChannelId ch) { return g_core.Peers(ch); }

bool SetPresence(ChannelId ch, const std::string& json) {
    auto it = g_channels.find(ch);
    if (it == g_channels.end() || json.size() > wire::kMaxPresence) return false;
    PresenceRec& p = g_presence[it->second];
    if (p.value != json) {
        p.value = json;
        p.pending = true;
    }
    return true;
}

std::string Presence(ChannelId ch, uint64_t member) {
    auto it = g_channels.find(ch);
    if (it == g_channels.end() || !lobby::Current()) return "";
    bool known = false;
    for (const auto& m : g_view.members)
        if (m.id == member && !m.me) known = true;
    if (!known) return "";
    return lobby::MemberData(member, (std::string(wire::kPresencePrefix) + it->second).c_str());
}

void SetHooks(const Hooks& h) { g_hooks = h; }
}  // namespace melange::peerbus
