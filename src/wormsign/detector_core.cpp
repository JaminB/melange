#include "wormsign/detector_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "tools/json_read.h"

namespace melange::wormsign::detect {
const char* CompName(int i) {
    static const char* const kNames[] = {"time+rng", "turn", "worms", "tasks", "projectiles", "teams", "rng2"};
    return i >= 0 && i < 7 ? kNames[i] : "?";
}

std::string CompList(uint8_t mask) {
    std::string s;
    for (int i = 0; i < 6; ++i)
        if (mask & (1u << i)) s += (s.empty() ? "" : ", ") + std::string(CompName(i));
    return s;
}

void PeerCompare::Reset() {
    for (Slot& s : ours_) s = Slot{};
    for (Slot& s : theirs_) s = Slot{};
    const bool mods = compareMods_;
    diverged_ = fresh_ = anyOurs_ = false;
    divTick_ = compared_ = matched_ = mismatchedAfter_ = lastCommon_ = ourLatest_ = theirLatest_ = dropped_ = 0;
    compareMods_ = mods;
}

void PeerCompare::AddOurs(uint32_t tick, uint64_t engine, uint64_t mods) {
    Slot& s = ours_[tick % kSlots];
    s = Slot{tick, engine, mods, true, false};
    if (!anyOurs_ || tick > ourLatest_) ourLatest_ = tick;
    anyOurs_ = true;
    Try(tick);
}

bool PeerCompare::AddTheirs(uint32_t tick, uint64_t engine, uint64_t mods) {
    if (anyOurs_ && (static_cast<uint64_t>(tick) + kMaxSkew < ourLatest_ || tick > static_cast<uint64_t>(ourLatest_) + kMaxSkew)) {
        ++dropped_;
        return false;
    }
    Slot& s = theirs_[tick % kSlots];
    if (s.have && s.tick == tick) return true;
    s = Slot{tick, engine, mods, true, false};
    if (tick > theirLatest_) theirLatest_ = tick;
    Try(tick);
    return true;
}

void PeerCompare::Try(uint32_t tick) {
    Slot& a = ours_[tick % kSlots];
    Slot& b = theirs_[tick % kSlots];
    if (!a.have || !b.have || a.tick != tick || b.tick != tick || a.done || b.done) return;
    a.done = b.done = true;
    ++compared_;
    if (tick > lastCommon_) lastCommon_ = tick;
    const bool same = a.engine == b.engine && (!compareMods_ || a.mods == b.mods);
    if (same) {
        ++matched_;
        return;
    }
    if (!diverged_) {
        diverged_ = fresh_ = true;
        divTick_ = tick;
    } else {
        ++mismatchedAfter_;
    }
}

bool PeerCompare::TakeNewDivergence() {
    const bool f = fresh_;
    fresh_ = false;
    return f;
}

bool PeerCompare::Ours(uint32_t tick, uint64_t* engine, uint64_t* mods) const {
    const Slot& s = ours_[tick % kSlots];
    if (!s.have || s.tick != tick) return false;
    *engine = s.engine;
    *mods = s.mods;
    return true;
}

bool PeerCompare::Theirs(uint32_t tick, uint64_t* engine, uint64_t* mods) const {
    const Slot& s = theirs_[tick % kSlots];
    if (!s.have || s.tick != tick) return false;
    *engine = s.engine;
    *mods = s.mods;
    return true;
}

const char* PeerStateName(PeerState s) {
    switch (s) {
        case PeerState::NoExchange: return "no exchange";
        case PeerState::VersionMismatch: return "version mismatch";
        case PeerState::Waiting: return "waiting";
        case PeerState::Exchanging: return "exchanging";
        case PeerState::OtherMatch: return "other match";
        case PeerState::Diverged: return "diverged";
    }
    return "?";
}

bool Detector::Exchanging(const Peer& p) {
    return p.info.advert == wire::kLobbyValue && !p.badProtocol && !p.engineMismatch;
}

PeerState Detector::StateOf(const Peer& p) const {
    if (p.info.advert.empty()) return PeerState::NoExchange;
    if (!Exchanging(p)) return PeerState::VersionMismatch;
    if (p.cmp.Diverged()) return PeerState::Diverged;
    if (p.otherMatch) return PeerState::OtherMatch;
    return p.helloIn ? PeerState::Exchanging : PeerState::Waiting;
}

Detector::Peer* Detector::Find(uint64_t id) {
    for (auto& p : peers_)
        if (p->info.id == id) return p.get();
    return nullptr;
}

void Detector::SetIdentity(const std::string& melange, const std::string& content16) {
    melange_ = melange;
    content16_ = content16;
}

void Detector::SetPeers(const std::vector<PeerInfo>& peers) {
    std::vector<std::unique_ptr<Peer>> next;
    for (const PeerInfo& in : peers) {
        if (!in.id || next.size() >= kMaxPeers) continue;
        std::unique_ptr<Peer> p;
        for (auto& old : peers_)
            if (old && old->info.id == in.id) p = std::move(old);
        if (!p) {
            p = std::make_unique<Peer>();
            p->cmp.Reset();
        }
        if (p->info.advert != in.advert) {
            p->badProtocol = false;
            p->engineMismatch = false;
        }
        p->info = in;
        next.push_back(std::move(p));
    }
    for (auto& old : peers_)
        if (old && old->phase != Phase::None && old->phase != Phase::Done) {
            if (old->phase == Phase::WaitComps) FinishComps(*old);
            FinishBundle(*old);
        }
    peers_ = std::move(next);
}

void Detector::ResetMatch(Peer& p) {
    p.helloIn = p.otherMatch = p.engineMismatch = false;
    p.hello = wire::Hello{};
    p.helloSentMs = 0;
    p.cmp.Reset();
    p.assembly.Reset();
    p.phase = Phase::None;
    p.rep = Report{};
    p.haveComps = false;
}

void Detector::Begin(uint32_t serial, uint64_t matchKey, std::vector<wire::ContribName> contribs) {
    session_ = true;
    serial_ = serial;
    key_ = matchKey;
    contribs_ = std::move(contribs);
    contribHash_ = wire::ContribListHash(contribs_);
    haveTick_ = flagged_ = false;
    firstTick_ = latest_ = lastBatch_ = flaggedTick_ = 0;
    for (auto& p : peers_) ResetMatch(*p);
}

void Detector::End() {
    for (auto& p : peers_)
        if (p->phase != Phase::None && p->phase != Phase::Done) {
            if (p->phase == Phase::WaitComps) FinishComps(*p);
            FinishBundle(*p);
        }
    session_ = false;
    for (auto& p : peers_) ResetMatch(*p);
}

void Detector::OnTick(const TickHash& h) {
    if (!session_) return;
    if (!haveTick_) {
        haveTick_ = true;
        firstTick_ = lastBatch_ = h.tick;
    }
    latest_ = h.tick;
    for (auto& p : peers_)
        if (Exchanging(*p)) p->cmp.AddOurs(h.tick, h.engine, h.mods);
}

void Detector::SendHello(Peer& p) {
    wire::Hello h;
    h.engineHash = kEngineHashVersion;
    h.matchKey = key_;
    h.firstTick = haveTick_ ? firstTick_ : 0;
    h.contribHash = contribHash_;
    h.seenYou = p.helloIn;
    h.melange = melange_;
    h.content16 = content16_;
    h.contribs = contribs_;
    env_.Send(p.info.id, wire::Encode(h), false);
    p.helloSentMs = env_.NowMs();
}

void Detector::SendBatch(Peer& p) {
    wire::Hashes b;
    b.matchKey = key_;
    b.firstTick = (std::max)(latest_ >= kBatchTicks ? latest_ - kBatchTicks + 1 : 1, firstTick_);
    for (uint32_t t = b.firstTick, i = 0; t <= latest_ && i < wire::kMaxBatch; ++t, ++i) {
        TickHash h{};
        if (env_.OurTick(t, &h)) b.present |= 1ull << i;
        b.ticks.push_back({h.engine, h.mods});
    }
    if (b.present) env_.Send(p.info.id, wire::Encode(b), false);
}

bool Detector::RateOk(std::deque<uint64_t>& q, size_t perMin) {
    const uint64_t now = env_.NowMs();
    while (!q.empty() && now - q.front() > 60000) q.pop_front();
    if (q.size() >= perMin) return false;
    q.push_back(now);
    return true;
}

void Detector::Start(Peer& p) {
    const uint32_t t = p.cmp.DivergedTick();
    if (!flagged_) {
        flagged_ = true;
        flaggedTick_ = t;
    }
    p.phase = Phase::WaitComps;
    p.phaseMs = env_.NowMs();
    Report& r = p.rep;
    r = Report{};
    Divergence& d = r.div;
    d.source = Source::Peer;
    d.serial = serial_;
    d.tick = t;
    d.peer = p.info.id;
    p.cmp.Ours(t, &d.oursEngine, &d.oursMods);
    p.cmp.Theirs(t, &d.theirsEngine, &d.theirsMods);
    TickHash h{};
    if (env_.OurTick(t, &h)) {
        memcpy(r.ours, h.c, sizeof r.ours);
        r.haveOurs = true;
    }
    env_.ContribHashes(t, &r.oursContrib);
    r.contribNames = contribs_;
    r.contribListsMatch = p.hello.contribHash == contribHash_;
    r.detailLocal = env_.Detail(t);
    r.peerName = p.info.name;
    wire::Flag f;
    f.matchKey = key_;
    f.tick = t;
    f.engine = d.oursEngine;
    f.mods = d.oursMods;
    memcpy(f.c, r.ours, sizeof f.c);
    env_.Send(p.info.id, wire::Encode(f), true);
    const wire::TickReq req{key_, t};
    env_.Send(p.info.id, wire::Encode(wire::Kind::CompsReq, req), true);
    env_.Send(p.info.id, wire::Encode(wire::Kind::DetailReq, req), true);
    char b[200];
    snprintf(b, sizeof b, "tick %u differs from %s (engine %016llx vs %016llx, mods %016llx vs %016llx)", t,
             p.info.name.c_str(), static_cast<unsigned long long>(d.oursEngine),
             static_cast<unsigned long long>(d.theirsEngine), static_cast<unsigned long long>(d.oursMods),
             static_cast<unsigned long long>(d.theirsMods));
    env_.Note(true, b);
}

void Detector::FinishComps(Peer& p) {
    Report& r = p.rep;
    Divergence& d = r.div;
    if (r.haveOurs && r.haveTheirs)
        for (int i = 0; i < kEngineComps; ++i)
            if (r.ours[i] != r.theirs[i]) d.compMask |= static_cast<uint8_t>(1u << i);
    if (r.contribListsMatch && d.oursMods != d.theirsMods && r.oursContrib.size() == contribs_.size() &&
        r.theirsContrib.size() == r.oursContrib.size())
        for (size_t i = 0; i < r.oursContrib.size(); ++i)
            if (r.oursContrib[i] != r.theirsContrib[i]) {
                snprintf(d.contrib, sizeof d.contrib, "%s", contribs_[i].name.c_str());
                break;
            }
    env_.Diverged(d);
    p.phase = Phase::WaitDetail;
    p.phaseMs = env_.NowMs();
}

void Detector::FinishBundle(Peer& p) {
    p.phase = Phase::Done;
    env_.Bundle(p.rep);
}

void Detector::OnPacket(uint64_t from, const uint8_t* data, size_t n) {
    Peer* pp = Find(from);
    if (!pp || !session_ || !Exchanging(*pp)) return;
    Peer& p = *pp;
    wire::Packet pk;
    const wire::DecodeResult res = wire::Decode(data, n, &pk);
    if (res == wire::DecodeResult::BadProtocol) {
        if (!p.badProtocol) {
            char b[160];
            snprintf(b, sizeof b, "%s speaks exchange protocol %u, we speak %u: no exchange", p.info.name.c_str(),
                     pk.protocol, wire::kProtocol);
            env_.Note(false, b);
        }
        p.badProtocol = true;
        return;
    }
    if (res != wire::DecodeResult::Ok) {
        ++badPackets_;
        return;
    }
    if (pk.kind != wire::Kind::Hello && !p.helloIn) return;
    switch (pk.kind) {
        case wire::Kind::Hello: {
            const wire::Hello& h = pk.hello;
            if (h.engineHash != kEngineHashVersion) {
                if (!p.engineMismatch) {
                    char b[160];
                    snprintf(b, sizeof b, "%s uses engine hash v%u, we use v%u: no exchange", p.info.name.c_str(),
                             h.engineHash, kEngineHashVersion);
                    env_.Note(false, b);
                }
                p.engineMismatch = true;
                return;
            }
            if (h.matchKey != key_) {
                p.otherMatch = true;
                return;
            }
            const bool first = !p.helloIn;
            p.hello = h;
            p.otherMatch = false;
            p.helloIn = true;
            p.cmp.SetCompareMods(h.contribHash == contribHash_);
            if (first) {
                char b[200];
                snprintf(b, sizeof b, "exchanging hashes with %s (Melange %s, content %s, %s)", p.info.name.c_str(),
                         h.melange.c_str(), h.content16.c_str(),
                         h.contribHash == contribHash_ ? "same contributors" : "mod contributors differ");
                env_.Note(false, b);
            }
            if (!h.seenYou) SendHello(p);
            break;
        }
        case wire::Kind::Hashes:
            if (pk.hashes.matchKey != key_) {
                p.otherMatch = true;
                return;
            }
            for (size_t i = 0; i < pk.hashes.ticks.size(); ++i)
                if (pk.hashes.present >> i & 1)
                    p.cmp.AddTheirs(pk.hashes.firstTick + static_cast<uint32_t>(i), pk.hashes.ticks[i].engine,
                                    pk.hashes.ticks[i].mods);
            break;
        case wire::Kind::CompsReq: {
            if (pk.req.matchKey != key_ || !RateOk(p.compsAnswers, kCompsAnswersPerMin)) return;
            wire::Comps c;
            c.matchKey = key_;
            c.tick = pk.req.tick;
            TickHash h{};
            c.have = env_.OurTick(pk.req.tick, &h);
            c.engine = h.engine;
            c.mods = h.mods;
            memcpy(c.c, h.c, sizeof c.c);
            if (c.have) env_.ContribHashes(pk.req.tick, &c.contrib);
            env_.Send(p.info.id, wire::Encode(c), true);
            break;
        }
        case wire::Kind::Comps:
            if (pk.comps.matchKey != key_ || !pk.comps.have || p.phase != Phase::WaitComps || pk.comps.tick != p.rep.div.tick)
                return;
            memcpy(p.rep.theirs, pk.comps.c, sizeof p.rep.theirs);
            p.rep.haveTheirs = p.haveComps = true;
            p.rep.theirsContrib = pk.comps.contrib;
            FinishComps(p);
            break;
        case wire::Kind::DetailReq:
            if (pk.req.matchKey != key_ || !RateOk(p.detailAnswers, kDetailAnswersPerMin)) return;
            for (auto& chunk : wire::EncodeDetail(key_, pk.req.tick, env_.Detail(pk.req.tick)))
                env_.Send(p.info.id, chunk, true);
            break;
        case wire::Kind::Detail: {
            if (pk.detail.matchKey != key_ || p.phase == Phase::None || p.phase == Phase::Done ||
                pk.detail.tick != p.rep.div.tick)
                return;
            std::string json;
            if (p.assembly.Add(pk.detail, &json)) p.rep.detailPeer = std::move(json);
            break;
        }
        case wire::Kind::Flag:
            if (pk.flag.matchKey != key_) return;
            p.cmp.AddTheirs(pk.flag.tick, pk.flag.engine, pk.flag.mods);
            if (p.phase == Phase::WaitComps && pk.flag.tick == p.rep.div.tick && !p.rep.haveTheirs) {
                memcpy(p.rep.theirs, pk.flag.c, sizeof p.rep.theirs);
                p.rep.haveTheirs = true;
            }
            break;
    }
}

void Detector::Pump() {
    if (!session_) return;
    const uint64_t now = env_.NowMs();
    const bool batchDue = haveTick_ && latest_ >= lastBatch_ + kBatchEvery;
    for (auto& up : peers_) {
        Peer& p = *up;
        if (!Exchanging(p)) continue;
        const uint64_t every = p.helloIn && p.hello.seenYou ? kHelloLateMs : kHelloEarlyMs;
        if (!p.helloSentMs || now - p.helloSentMs >= every) SendHello(p);
        if (batchDue && p.helloIn && !p.otherMatch) SendBatch(p);
        if (p.cmp.TakeNewDivergence()) Start(p);
        if (p.phase == Phase::WaitComps) {
            const bool needContrib = !contribs_.empty() && p.rep.contribListsMatch && p.rep.div.oursMods != p.rep.div.theirsMods;
            if (p.haveComps || (p.rep.haveTheirs && !needContrib) || now - p.phaseMs >= kCompsWaitMs) FinishComps(p);
        }
        if (p.phase == Phase::WaitDetail && (!p.rep.detailPeer.empty() || now - p.phaseMs >= kDetailWaitMs)) FinishBundle(p);
    }
    if (batchDue) lastBatch_ = latest_;
}

int Detector::Peers(PeerStatus* out, int max) const {
    int n = 0;
    for (auto& p : peers_) {
        if (n >= max) break;
        PeerStatus& s = out[n++];
        s = PeerStatus{};
        s.steamId = p->info.id;
        snprintf(s.name, sizeof s.name, "%s", p->info.name.c_str());
        snprintf(s.melange, sizeof s.melange, "%s", p->hello.melange.c_str());
        snprintf(s.content, sizeof s.content, "%s", p->hello.content16.c_str());
        s.state = StateOf(*p);
        s.modsCompared = p->helloIn && p->hello.contribHash == contribHash_;
        s.compared = p->cmp.Compared();
        s.lastCommonTick = p->cmp.LastCommon();
        s.theirTick = p->cmp.TheirLatest();
        s.lagTicks = static_cast<int32_t>(latest_) - static_cast<int32_t>(p->cmp.TheirLatest());
        s.divergedTick = p->cmp.Diverged() ? p->cmp.DivergedTick() : 0;
        s.apartTicks = p->cmp.Diverged() && p->cmp.LastCommon() >= p->cmp.DivergedTick()
                           ? p->cmp.LastCommon() - p->cmp.DivergedTick()
                           : 0;
    }
    return n;
}

bool Detector::Flagged(uint32_t* tick) const {
    if (flagged_ && tick) *tick = flaggedTick_;
    return flagged_;
}

namespace {
std::string Quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') o += '\\', o += static_cast<char>(c);
        else if (c < 0x20) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o + "\"";
}

void Walk(const json::Value& v, const std::string& path, std::vector<std::pair<std::string, std::string>>* out) {
    switch (v.type) {
        case json::Type::Object:
            if (v.members.empty()) out->emplace_back(path, "{}");
            for (const auto& [k, m] : v.members) Walk(m, path.empty() ? k : path + "." + k, out);
            break;
        case json::Type::Array:
            if (v.items.empty()) out->emplace_back(path, "[]");
            for (size_t i = 0; i < v.items.size(); ++i) Walk(v.items[i], path + "[" + std::to_string(i) + "]", out);
            break;
        case json::Type::Number: {
            char b[40];
            if (v.IsInteger()) snprintf(b, sizeof b, "%lld", static_cast<long long>(v.number));
            else snprintf(b, sizeof b, "%.9g", v.number);
            out->emplace_back(path, b);
            break;
        }
        case json::Type::String: out->emplace_back(path, Quote(v.string)); break;
        case json::Type::Bool: out->emplace_back(path, v.boolean ? "true" : "false"); break;
        case json::Type::Null: out->emplace_back(path, "null"); break;
    }
}
}  // namespace

bool Flatten(std::string_view text, std::vector<std::pair<std::string, std::string>>* out, std::string* err) {
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e)) {
        if (err) *err = "line " + std::to_string(e.line) + ": " + e.text;
        return false;
    }
    out->clear();
    Walk(v, "", out);
    return true;
}

std::vector<std::string> FieldDiff(std::string_view oursJson, std::string_view theirsJson, std::string* err) {
    std::vector<std::string> lines;
    std::vector<std::pair<std::string, std::string>> a, b;
    if (!Flatten(oursJson, &a, err) || !Flatten(theirsJson, &b, err)) return lines;
    std::unordered_map<std::string, const std::string*> theirs;
    for (const auto& [k, v] : b) theirs.emplace(k, &v);
    std::unordered_map<std::string, bool> seen;
    for (const auto& [k, v] : a) {
        seen[k] = true;
        auto it = theirs.find(k);
        if (it == theirs.end()) lines.push_back(k + " " + v + " -> (none)");
        else if (*it->second != v) lines.push_back(k + " " + v + " -> " + *it->second);
    }
    for (const auto& [k, v] : b)
        if (!seen.count(k)) lines.push_back(k + " (none) -> " + v);
    return lines;
}

std::string LogTailSeconds(std::string_view log, double seconds) {
    struct Line {
        size_t at, len;
        double t;
        bool stamped;
    };
    std::vector<Line> lines;
    double last = -1;
    size_t p = 0;
    while (p < log.size()) {
        size_t e = log.find('\n', p);
        if (e == std::string_view::npos) e = log.size();
        else ++e;
        Line l{p, e - p, 0, false};
        const std::string_view s = log.substr(p, e - p);
        const size_t b = s.find("[+");
        if (b != std::string_view::npos && b < 24) {
            const std::string num(s.substr(b + 2, (std::min<size_t>)(16, s.size() - b - 2)));
            char* end = nullptr;
            const double t = strtod(num.c_str(), &end);
            if (end && end != num.c_str() && *end == ']') {
                l.t = t;
                l.stamped = true;
                last = t;
            }
        }
        lines.push_back(l);
        p = e;
    }
    if (last < 0) return std::string(log);
    size_t from = lines.size();
    for (size_t i = 0; i < lines.size(); ++i)
        if (lines[i].stamped && lines[i].t >= last - seconds) {
            from = i;
            break;
        }
    return from < lines.size() ? std::string(log.substr(lines[from].at)) : std::string();
}
}  // namespace melange::wormsign::detect
