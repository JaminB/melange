#include "net/peerbus_core.h"

#include <algorithm>

namespace melange::peerbus {
using wire::Kind;

void Core::Reset() {
    if (Online()) ++c_.resets;
    for (auto& [id, c] : channels_) {
        for (uint64_t m : c.reachable) changes_.push_back({id, m, false});
        c.reachable.clear();
        c.send.clear();
        c.recvMsgs.clear();
        c.recvBytes.clear();
        c.byeAdvert.clear();
    }
    members_.clear();
    reasm_.Clear();
    global_.Clear();
}

void Core::SetLobby(const LobbyInfo& l, double nowMs) {
    (void)nowMs;
    if (l.lobby != lobby_.lobby || l.key != lobby_.key || l.me != lobby_.me) Reset();
    lobby_ = l;
    if (!Online()) {
        members_.clear();
        return;
    }
    std::set<uint64_t> now;
    for (const MemberInfo& mi : l.members) {
        if (!mi.id || mi.id == l.me) continue;
        now.insert(mi.id);
        Member& m = members_[mi.id];
        m.advert = mi.advert;
    }
    for (auto it = members_.begin(); it != members_.end();) {
        if (now.count(it->first)) {
            ++it;
            continue;
        }
        reasm_.DropSender(it->first);
        global_.Forget(it->first);
        for (auto& [id, c] : channels_) {
            c.send.erase(it->first);
            c.recvMsgs.erase(it->first);
            c.recvBytes.erase(it->first);
            c.byeAdvert.erase(it->first);
        }
        it = members_.erase(it);  // Tick reports it down
    }
    for (auto& [id, c] : channels_)
        for (auto it = c.byeAdvert.begin(); it != c.byeAdvert.end();) {
            auto m = members_.find(it->first);
            if (m == members_.end() || m->second.advert != it->second) it = c.byeAdvert.erase(it);
            else ++it;
        }
}

ChannelId Core::Open(const std::string& modId, int version, std::string* why) {
    if (!wire::ValidModId(modId)) {
        if (why) *why = "bad-id";
        return 0;
    }
    if (version < 1 || version > 65535) {
        if (why) *why = "bad-version";
        return 0;
    }
    for (const auto& [id, c] : channels_)
        if (c.modId == modId) {
            if (why) *why = "already-open";
            return 0;
        }
    const ChannelId id = nextId_++;
    Channel& c = channels_[id];
    c.modId = modId;
    c.version = version;
    c.stats.modId = modId;
    c.stats.version = version;
    return id;
}

void Core::Close(ChannelId ch) {
    auto it = channels_.find(ch);
    if (it == channels_.end()) return;
    for (uint64_t m : it->second.reachable)
        if (Reachable(it->second, m)) SendControl(m, Kind::Bye, it->second.modId, {}, true);
    channels_.erase(it);
}

std::string Core::Advert() const {
    std::vector<wire::AdvertEntry> e;
    for (const auto& [id, c] : channels_) e.push_back({c.modId, c.version});
    return wire::BuildAdvert(std::move(e));
}

const std::string* Core::ModOf(ChannelId ch) const {
    auto it = channels_.find(ch);
    return it == channels_.end() ? nullptr : &it->second.modId;
}

bool Core::Advertises(const Channel& c, uint64_t id) const {
    auto it = members_.find(id);
    if (it == members_.end() || !wire::Advertises(it->second.advert, c.modId, c.version)) return false;
    auto bye = c.byeAdvert.find(id);
    return bye == c.byeAdvert.end() || bye->second != it->second.advert;
}

bool Core::Reachable(const Channel& c, uint64_t id) const {
    if (!Online() || !Advertises(c, id)) return false;
    auto it = members_.find(id);
    return it != members_.end() && it->second.session;
}

bool Core::AnyCommon(uint64_t id) const {
    for (const auto& [cid, c] : channels_)
        if (Reachable(c, id)) return true;
    return false;
}

bool Core::SendControl(uint64_t to, Kind k, const std::string& modId, const std::vector<uint8_t>& body, bool reliable) {
    wire::Header h;
    h.kind = k;
    h.reliable = reliable;
    h.lobbyKey = lobby_.key;
    h.modId = modId;
    const std::vector<uint8_t> pkt = wire::Encode(h, body.data(), body.size());
    if (pkt.empty()) return false;
    if (!io_->Send(to, pkt, reliable, !reliable)) {
        ++c_.sendFailed;
        return false;
    }
    ++c_.packetsOut;
    c_.bytesOut += pkt.size();
    return true;
}

SendStatus Core::Send(ChannelId ch, uint64_t to, const std::vector<uint8_t>& payload, bool reliable, double nowMs) {
    auto it = channels_.find(ch);
    wire::GateIn g;
    g.channelOpen = it != channels_.end();
    g.online = Online();
    if (g.channelOpen && g.online) {
        auto m = members_.find(to);
        g.member = m != members_.end() && to != lobby_.me;
        g.advertises = g.member && Advertises(it->second, to);
        g.session = g.member && m->second.session;
    }
    const SendStatus gate = wire::Gate(g);
    if (gate != SendStatus::Ok) {
        if (g.channelOpen) ++it->second.stats.refused;
        return gate;
    }
    Channel& c = it->second;
    if (payload.size() > wire::MaxMessage(c.modId.size(), reliable)) {
        ++c.stats.refused;
        return SendStatus::Size;
    }
    uint16_t& seq = seq_[{c.modId, to}];
    const std::vector<std::vector<uint8_t>> frags = wire::EncodeMessage(lobby_.key, c.modId, seq, reliable, payload);
    if (frags.empty()) {
        ++c.stats.refused;
        return SendStatus::Size;
    }
    size_t wireBytes = 0;
    for (const auto& f : frags) wireBytes += f.size();
    wire::SendLimiter& lim = c.send[to];
    if (!lim.Can(payload.size(), nowMs) || !global_.Can(to, wireBytes, nowMs)) {
        ++c.stats.refused;
        return SendStatus::Rate;
    }
    lim.Take(payload.size(), nowMs);
    global_.Take(to, wireBytes, nowMs);
    ++seq;
    for (const auto& f : frags) {
        if (!io_->Send(to, f, reliable, false)) {
            ++c_.sendFailed;
            ++c.stats.refused;
            return SendStatus::NoSession;  // Steam refused it: the connection went away under us
        }
        ++c_.packetsOut;
        c_.bytesOut += f.size();
    }
    ++c.stats.sent;
    return SendStatus::Ok;
}

int Core::Broadcast(ChannelId ch, const std::vector<uint8_t>& payload, bool reliable, double nowMs) {
    if (!IsOpen(ch)) return 0;
    int n = 0;
    std::vector<uint64_t> ids;
    for (const auto& [id, m] : members_) ids.push_back(id);
    for (uint64_t id : ids)
        if (Send(ch, id, payload, reliable, nowMs) == SendStatus::Ok) ++n;
    return n;
}

std::vector<uint64_t> Core::Peers(ChannelId ch) const {
    auto it = channels_.find(ch);
    if (it == channels_.end()) return {};
    return {it->second.reachable.begin(), it->second.reachable.end()};
}

bool Core::Clock(uint64_t id, ClockView* out) const {
    auto it = members_.find(id);
    if (it == members_.end() || !it->second.clock.Ready()) return false;
    out->offsetMs = it->second.clock.Offset();
    out->rttMs = it->second.clock.Rtt();
    out->samples = it->second.clock.Samples();
    return true;
}

void Core::OnPacket(uint64_t from, const uint8_t* p, size_t n, double at) {
    ++c_.packetsIn;
    c_.bytesIn += n;
    if (!Online()) {
        ++c_.dropOffline;
        return;
    }
    auto mit = members_.find(from);
    if (mit == members_.end()) {  // before decode: a non-member's bytes are never parsed
        ++c_.dropNonMember;
        return;
    }
    wire::Packet pkt;
    switch (wire::Decode(p, n, &pkt)) {
        case wire::DecodeResult::Ok: break;
        case wire::DecodeResult::NotOurs: ++c_.dropNotOurs; return;
        case wire::DecodeResult::BadProtocol: ++c_.dropBadProtocol; return;
        case wire::DecodeResult::Malformed: ++c_.dropMalformed; return;
    }
    if (pkt.h.lobbyKey != lobby_.key) {
        ++c_.dropStaleKey;
        return;
    }
    Member& m = mit->second;
    const Kind k = pkt.h.kind;
    if (k != Kind::Msg) {
        if (!m.control.Can(1, at)) {
            ++c_.dropRate;
            return;
        }
        m.control.Take(1, at);
    }
    if (k == Kind::Ping) {
        double t0 = 0;
        if (!wire::ParsePing(pkt.body, &t0)) {
            ++c_.dropMalformed;
            return;
        }
        // Timestamps are taken in the drain: t1 (receive) and t2 (send) are both this drain's time.
        if (AnyCommon(from)) SendControl(from, Kind::Pong, "", wire::PongBody(t0, at, at), false);
        return;
    }
    if (k == Kind::Pong) {
        double t0 = 0, t1 = 0, t2 = 0;
        if (!wire::ParsePong(pkt.body, &t0, &t1, &t2)) {
            ++c_.dropMalformed;
            return;
        }
        if (t0 > at || (stalled_ && t0 < stallAt_)) return;  // not ours, or sent before the last stall
        m.clock.Add(t0, t1, t2, at);
        return;
    }
    Channel* c = nullptr;
    ChannelId cid = 0;
    for (auto& [id, ch] : channels_)
        if (ch.modId == pkt.h.modId) {
            c = &ch;
            cid = id;
        }
    if (!c) {
        ++c_.dropNoChannel;
        return;
    }
    if (k == Kind::Hello) {
        uint16_t v = 0;
        if (!wire::ParseHello(pkt.body, &v)) {
            ++c_.dropMalformed;
            return;
        }
        if (v == c->version) c->byeAdvert.erase(from);  // it (re)opened the channel
        return;
    }
    if (k == Kind::Bye) {
        c->byeAdvert[from] = m.advert;
        return;
    }
    // MSG
    if (!Advertises(*c, from)) {  // another version of the mod, or a member that never opened the channel
        ++c_.dropNoAdvert;
        ++c->stats.dropped;
        return;
    }
    auto bytes = c->recvBytes.try_emplace(from, wire::kRecvBytesPerSec, wire::kRecvByteBurst).first;
    const double len = static_cast<double>(pkt.body.size());
    if (!bytes->second.Can(len, at)) {
        ++c_.dropRate;
        ++c->stats.dropped;
        return;
    }
    bytes->second.Take(len, at);
    std::vector<uint8_t> msg;
    const auto r = reasm_.Add(from, pkt.h, pkt.body, wire::ChunkBytes(c->modId.size()), at, &msg);
    if (r == wire::Reassembler::Result::Pending) return;
    if (r == wire::Reassembler::Result::Dropped) {
        ++c->stats.dropped;
        return;
    }
    auto msgs = c->recvMsgs.try_emplace(from, wire::kRecvMsgsPerSec, wire::kRecvMsgBurst).first;
    if (!msgs->second.Can(1, at)) {
        ++c_.dropRate;
        ++c->stats.dropped;
        return;
    }
    msgs->second.Take(1, at);
    wire::Value v;
    if (!wire::DecodeValue(msg.data(), msg.size(), &v)) {
        ++c_.dropValue;
        ++c->stats.dropped;
        return;
    }
    ++c->stats.received;
    deliveries_.push_back({cid, from, std::move(msg), pkt.h.reliable, at});
}

void Core::Tick(double nowMs) {
    if (ticked_ && nowMs - lastTick_ > wire::kStallMs) {
        // A level load (or any long frame): packets waited in Steam meanwhile, so their drain times are wrong.
        ++c_.stalls;
        stalled_ = true;
        stallAt_ = nowMs;
        for (auto& [id, m] : members_) {
            m.clock.Reset();
            if (m.up) {
                m.upSince = nowMs;
                m.nextPing = nowMs;
            }
        }
    }
    ticked_ = true;
    lastTick_ = nowMs;
    reasm_.Expire(nowMs);
    c_.dropReassembly = reasm_.Dropped();

    // Session state only for members that advertise one of our channels: nobody else is ever sent anything.
    for (auto& [id, m] : members_) {
        bool wanted = false;
        if (Online())
            for (const auto& [cid, c] : channels_)
                if (Advertises(c, id)) wanted = true;
        m.session = wanted && io_->SessionActive(id);
    }

    size_t dests = 0;
    for (auto& [cid, c] : channels_) {
        std::set<uint64_t> now;
        for (const auto& [id, m] : members_)
            if (Reachable(c, id)) now.insert(id);
        for (uint64_t id : now)
            if (!c.reachable.count(id)) {
                changes_.push_back({cid, id, true});
                SendControl(id, Kind::Hello, c.modId, wire::HelloBody(static_cast<uint16_t>(c.version)), true);
            }
        for (uint64_t id : c.reachable)
            if (!now.count(id)) changes_.push_back({cid, id, false});
        c.reachable = std::move(now);
    }
    for (auto& [id, m] : members_) {
        if (!AnyCommon(id)) {
            m.up = false;
            continue;
        }
        ++dests;
        if (!m.up) {
            m.up = true;
            m.upSince = nowMs;
            m.nextPing = nowMs;
        }
        if (nowMs >= m.nextPing) {
            SendControl(id, Kind::Ping, "", wire::PingBody(nowMs), false);
            m.nextPing = nowMs + wire::PingInterval(nowMs, m.upSince);
        }
    }
    global_.SetDestinations(dests);
}

std::vector<Delivery> Core::TakeDeliveries() {
    std::vector<Delivery> d;
    d.swap(deliveries_);
    return d;
}

std::vector<PeerChange> Core::TakePeerChanges() {
    std::vector<PeerChange> c;
    c.swap(changes_);
    return c;
}

std::vector<ChannelStats> Core::Channels() const {
    std::vector<ChannelStats> out;
    for (const auto& [id, c] : channels_) out.push_back(c.stats);
    return out;
}

std::vector<uint64_t> Core::ClockMembers() const {
    std::vector<uint64_t> out;
    for (const auto& [id, m] : members_) out.push_back(id);
    return out;
}
}  // namespace melange::peerbus
