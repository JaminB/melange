// sandbox_selftest fakes for wum.lobby / wum.peer (owner: C3): link-time stand-ins for peerbus:: (peerbus_api.h, force-
// included by cmake/holo/sandbox_peer.cmake), so wum_peer.cpp runs without Steam. Included inside namespace melange,
// after the other fakes. The bus's own logic is tested by peerbus_selftest; here only the binding.
namespace fake_peer {
inline double g_nowMs = 5000.0;
inline bool g_available = true;
inline bool g_inSession = false;
inline peerbus::LobbyView g_view;
struct Chan {
    std::string mod;
    int version = 0;
};
inline std::map<peerbus::ChannelId, Chan> g_chans;
inline peerbus::ChannelId g_next = 1;
inline int g_opens = 0, g_closes = 0;
struct Sent {
    peerbus::ChannelId ch = 0;
    uint64_t to = 0;
    std::vector<uint8_t> payload;
    bool reliable = false;
};
inline std::vector<Sent> g_sent;
inline peerbus::SendStatus g_status = peerbus::SendStatus::Ok;
inline std::vector<uint64_t> g_peers;
inline std::map<peerbus::ChannelId, std::string> g_presenceSet;
inline std::map<uint64_t, std::string> g_presenceOf;
inline std::map<uint64_t, peerbus::ClockInfo> g_clocks;
inline peerbus::Hooks g_hooks;

inline void Reset() {
    g_available = true;
    g_inSession = false;
    g_view = {};
    g_sent.clear();
    g_status = peerbus::SendStatus::Ok;
    g_peers.clear();
    g_presenceSet.clear();
    g_presenceOf.clear();
    g_clocks.clear();
}
inline peerbus::ChannelId ChannelOf(const std::string& mod) {
    for (const auto& [id, c] : g_chans)
        if (c.mod == mod) return id;
    return 0;
}
// What the bus would hand wum_peer.cpp at the end of its frame.
inline void Deliver(peerbus::ChannelId ch, uint64_t from, const std::vector<uint8_t>& payload, bool reliable, double at) {
    peerbus::Delivery d;
    d.ch = ch;
    d.from = from;
    d.payload = payload;
    d.reliable = reliable;
    d.at = at;
    if (g_hooks.message) g_hooks.message(d);
}
inline void PeerUp(peerbus::ChannelId ch, uint64_t id, bool up) {
    if (g_hooks.peer) g_hooks.peer({ch, id, up});
}
inline void Lobby(peerbus::LobbyEvent e) {
    if (g_hooks.lobby) g_hooks.lobby(e);
}
}  // namespace fake_peer
namespace peerbus {
double NowMs() { return fake_peer::g_nowMs += 0.25; }
bool Clock(uint64_t id, ClockInfo* out) {
    auto it = fake_peer::g_clocks.find(id);
    if (it == fake_peer::g_clocks.end()) return false;
    *out = it->second;
    return true;
}
bool Available() { return fake_peer::g_available; }
bool Lobby(LobbyView* out) {
    if (!fake_peer::g_inSession) return false;
    *out = fake_peer::g_view;
    return true;
}
ChannelId Open(const std::string& modId, int version, std::string* why) {
    if (fake_peer::ChannelOf(modId)) {
        *why = "already-open";
        return 0;
    }
    ++fake_peer::g_opens;
    const ChannelId id = fake_peer::g_next++;
    fake_peer::g_chans[id] = {modId, version};
    return id;
}
void Close(ChannelId ch) {
    if (fake_peer::g_chans.erase(ch)) ++fake_peer::g_closes;
}
SendStatus Send(ChannelId ch, uint64_t to, const std::vector<uint8_t>& payload, bool reliable) {
    if (!fake_peer::g_chans.count(ch)) return SendStatus::Closed;
    if (payload.size() > wire::MaxMessage(fake_peer::g_chans[ch].mod.size(), reliable)) return SendStatus::Size;
    if (fake_peer::g_status == SendStatus::Ok) fake_peer::g_sent.push_back({ch, to, payload, reliable});
    return fake_peer::g_status;
}
int Broadcast(ChannelId ch, const std::vector<uint8_t>& payload, bool reliable) {
    int n = 0;
    for (uint64_t id : fake_peer::g_peers)
        if (Send(ch, id, payload, reliable) == SendStatus::Ok) ++n;
    return n;
}
std::vector<uint64_t> Peers(ChannelId ch) { return fake_peer::g_chans.count(ch) ? fake_peer::g_peers : std::vector<uint64_t>{}; }
bool SetPresence(ChannelId ch, const std::string& json) {
    if (!fake_peer::g_chans.count(ch)) return false;
    fake_peer::g_presenceSet[ch] = json;
    return true;
}
std::string Presence(ChannelId ch, uint64_t member) {
    if (!fake_peer::g_chans.count(ch)) return "";
    auto it = fake_peer::g_presenceOf.find(member);
    return it == fake_peer::g_presenceOf.end() ? "" : it->second;
}
void SetHooks(const Hooks& h) { fake_peer::g_hooks = h; }
}  // namespace peerbus
