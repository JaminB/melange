#include "wormsign/detector_core.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

#include "tools/json_read.h"
#include "wormsign/hash_engine.h"

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

void PeerCompare::Undiverge() {
    diverged_ = fresh_ = false;
    divTick_ = mismatchedAfter_ = 0;
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
            // FinishComps may have dropped the divergence (not a desync, or unconfirmed): no bundle then.
            if (old->phase == Phase::WaitDetail) FinishBundle(*old);
        }
    peers_ = std::move(next);
}

void Detector::ResetMatch(Peer& p) {
    p.helloIn = p.otherMatch = p.engineMismatch = p.modsUnreliable = p.modsUnconfirmed = p.compsRetried = false;
    p.modsNote.clear();
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
    engineTicks_.clear();
    for (auto& p : peers_) ResetMatch(*p);
}

void Detector::End() {
    for (auto& p : peers_)
        if (p->phase != Phase::None && p->phase != Phase::Done) {
            if (p->phase == Phase::WaitComps) FinishComps(*p);
            if (p->phase == Phase::WaitDetail) FinishBundle(*p);
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
    p.phase = Phase::WaitComps;
    p.phaseMs = env_.NowMs();
    p.compsRetried = false;
    p.haveComps = false;
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

namespace {
// The mods aggregate one machine computed (contrib::HashTick) from its per-contributor values, if every 0 among them
// (not computed there at that tick) was left out of it. That is what a faulted contributor (or one never computed
// yet) does; a contributor skipped by demotion folds its last computed hash in instead, so the fold differs from
// that machine's mods hash whenever one of its zeros was a demotion skip.
uint64_t FoldComputed(const std::vector<wire::ContribName>& names, const std::vector<uint64_t>& v) {
    uint64_t all = kFnvBasis;
    for (size_t i = 0; i < v.size() && i < names.size(); ++i)
        if (v[i]) all = FnvV(v[i], FnvV(Fnv(names[i].name.data(), names[i].name.size()), all));
    return names.empty() ? 0 : all;
}
size_t Zeros(const std::vector<uint64_t>& v) {
    size_t n = 0;
    for (uint64_t x : v) n += x == 0;
    return n;
}
}  // namespace

// A per-contributor value of 0 in COMPS means "not computed on that machine at that tick" (contrib::HashesAt: the
// contributor was demoted there and skipped the tick, faulted, or was added later), not a hash of its state. Each
// differing contributor is one of:
//   - real: both sides computed it and the values differ (a desync of that contributor);
//   - faulted: 0 on one side, and that side's mods hash is exactly the fold of its non-zero values, so the 0 was left
//     out of it (faulted there, or never computed): the two mods hashes cannot agree again, reported as a desync;
//   - skipped: 0 on one side, and that side's mods hash is not that fold, so a 0 stood for a stale value folded in
//     (demoted there, skipping the tick). Not evidence of different state: the skipping side folded its last
//     computed hash into its aggregate while the other side folded the fresh one. When that side has several zeros,
//     some of them may be faults; that cannot be told apart, and the note says so.
// The mismatch is a desync when any contributor is real or faulted (the first real one is named, else the first
// faulted one), and not one when every differing contributor is skipped. (Seen with Melange <= 0.11.1, whose default
// demoted a slow mod environment digest to every 10 ticks: a mod's first state change on a skipped tick flagged the
// peer at that tick on every machine that was not demoted.)
bool Detector::Analyze(Peer& p, int* named, std::string* why, std::string* note) {
    *named = -1;
    why->clear();
    note->clear();
    const Report& r = p.rep;
    const Divergence& d = r.div;
    if (!r.contribListsMatch || !p.haveComps || d.oursMods == d.theirsMods || r.oursContrib.size() != contribs_.size() ||
        r.theirsContrib.size() != r.oursContrib.size())
        return true;
    const bool oursLeftOut = FoldComputed(contribs_, r.oursContrib) == d.oursMods;
    const bool theirsLeftOut = FoldComputed(contribs_, r.theirsContrib) == d.theirsMods;
    const size_t oursZeros = Zeros(r.oursContrib), theirsZeros = Zeros(r.theirsContrib);
    const std::string& peer = p.info.name;
    int firstReal = -1, firstFault = -1;
    std::string skipped, faulted;
    bool ambiguous = false;
    for (size_t i = 0; i < r.oursContrib.size(); ++i) {
        if (r.oursContrib[i] == r.theirsContrib[i]) continue;
        const bool oz = r.oursContrib[i] == 0, tz = r.theirsContrib[i] == 0;
        if (oz == tz) {
            if (firstReal < 0) firstReal = static_cast<int>(i);
            continue;
        }
        const std::string who = oz ? std::string("we") : peer;
        const std::string& name = contribs_[i].name;
        if (oz ? oursLeftOut : theirsLeftOut) {
            if (firstFault < 0) firstFault = static_cast<int>(i);
            faulted += (faulted.empty() ? "" : "; ") + name + " is not in " + (oz ? std::string("our") : peer + "'s") +
                       " mods hash (faulted there, or never computed)";
            continue;
        }
        const bool unsure = (oz ? oursZeros : theirsZeros) > 1;
        ambiguous |= unsure;
        skipped += (skipped.empty() ? "" : "; ") + who + " did not hash " + name + " at that tick (" +
                   (unsure ? "demoted or faulted there: cannot tell which" : "demoted there") + ")";
    }
    char b[200];
    if (firstReal >= 0 || firstFault >= 0) {
        *named = firstReal >= 0 ? firstReal : firstFault;
        if (firstReal < 0) {
            snprintf(b, sizeof b, "tick %u: a desync of the mods hash with %s: ", d.tick, peer.c_str());
            *why = b + faulted;
        } else if (!faulted.empty() || !skipped.empty()) {
            snprintf(b, sizeof b, "tick %u: %s differs from %s with both sides hashing it (a desync); also differing: ",
                     d.tick, contribs_[static_cast<size_t>(firstReal)].name.c_str(), peer.c_str());
            *why = b + faulted + (faulted.empty() || skipped.empty() ? "" : "; ") + skipped;
        }
        return true;
    }
    if (skipped.empty()) return true;   // the aggregates differ while no contributor does: a desync with no name
    snprintf(b, sizeof b, "tick %u: the mods hash differs from %s only because ", d.tick, peer.c_str());
    *why = b + skipped + ": not a desync; the mods hash is no longer compared with " + peer +
           " this match (the engine hash still is)";
    snprintf(b, sizeof b, "mods not compared: a contributor was skipped on one side at tick %u%s", d.tick,
             ambiguous ? " (demoted, or faulted: cannot tell)" : "");
    *note = b;
    return false;
}

void Detector::DropMods(Peer& p, const std::string& note) {
    p.modsNote = note;
    p.cmp.SetCompareMods(false);
    p.cmp.Undiverge();
    p.phase = Phase::None;
    p.rep = Report{};
    p.haveComps = p.compsRetried = false;
}

void Detector::Unconfirmed(Peer& p) {
    const Divergence& d = p.rep.div;
    char b[400];
    snprintf(b, sizeof b, "tick %u: the mods hash differs from %s (mods %016llx vs %016llx, engine equal), but %s never "
             "sent its contributor hashes (asked twice): unconfirmed, not reported as a desync; the mods hash is no longer "
             "compared with %s this match (the engine hash still is)",
             d.tick, p.info.name.c_str(), static_cast<unsigned long long>(d.oursMods),
             static_cast<unsigned long long>(d.theirsMods), p.info.name.c_str(), p.info.name.c_str());
    env_.Note(true, b);
    char n[160];
    snprintf(n, sizeof n, "mods not compared: a mismatch at tick %u was not confirmed (no contributor hashes from the peer)",
             d.tick);
    p.modsUnconfirmed = true;
    DropMods(p, n);
}

void Detector::FinishComps(Peer& p) {
    Report& r = p.rep;
    Divergence& d = r.div;
    const bool modsOnly = d.oursEngine == d.theirsEngine && d.oursMods != d.theirsMods;
    // A mods-only mismatch is judged by the contributors: without the peer's, it is not confirmed.
    if (modsOnly && !contribs_.empty() && r.contribListsMatch && !p.haveComps) {
        Unconfirmed(p);
        return;
    }
    int named = -1;
    std::string why, note;
    const bool desync = Analyze(p, &named, &why, &note);
    if (!desync && modsOnly) {
        env_.Note(true, why);
        p.modsUnreliable = true;
        DropMods(p, note);
        return;
    }
    // An engine desync is one whatever the contributors say; skipped ones are logged and not named.
    if (!why.empty()) env_.Note(true, why);
    if (r.haveOurs && r.haveTheirs)
        for (int i = 0; i < kEngineComps; ++i)
            if (r.ours[i] != r.theirs[i]) d.compMask |= static_cast<uint8_t>(1u << i);
    if (named >= 0) snprintf(d.contrib, sizeof d.contrib, "%s", contribs_[static_cast<size_t>(named)].name.c_str());
    if (!flagged_) {
        flagged_ = true;
        flaggedTick_ = d.tick;
    }
    if (d.oursEngine != d.theirsEngine) {
        if (engineTicks_.size() >= kMaxEngineFlags) engineTicks_.erase(engineTicks_.begin());
        engineTicks_.push_back(d.tick);
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
            p.cmp.SetCompareMods(h.contribHash == contribHash_ && p.modsNote.empty());
            if (first) {
                char b[200];
                snprintf(b, sizeof b, "exchanging hashes with %s (Melange %s, content %s, %s)", p.info.name.c_str(),
                         h.melange.c_str(), h.content16.c_str(),
                         h.contribHash == contribHash_ ? "same contributors" : "mod contributors differ");
                env_.Note(false, b);
            }
            // Rate limited like the COMPS/DETAIL answers below: without this, a peer that keeps sending HELLO
            // with seenYou=0 gets a fresh reply (up to ~1.1 KB) every single time, with nothing else pacing it.
            if (!h.seenYou && (!p.helloSentMs || env_.NowMs() - p.helloSentMs >= kHelloEarlyMs)) SendHello(p);
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
            const Divergence& d = p.rep.div;
            const bool needContrib = !contribs_.empty() && p.rep.contribListsMatch && d.oursMods != d.theirsMods;
            const bool modsOnly = needContrib && d.oursEngine == d.theirsEngine;
            if (p.haveComps || (p.rep.haveTheirs && !needContrib)) {
                FinishComps(p);
            } else if (now - p.phaseMs >= (p.compsRetried ? kCompsRetryWaitMs : kCompsWaitMs)) {
                if (modsOnly && !p.compsRetried) {
                    // A mods-only mismatch is only judged with the peer's contributor hashes: ask once more.
                    p.compsRetried = true;
                    p.phaseMs = now;
                    env_.Send(p.info.id, wire::Encode(wire::Kind::CompsReq, wire::TickReq{key_, d.tick}), true);
                    char b[160];
                    snprintf(b, sizeof b, "tick %u: no contributor hashes from %s after %llu ms; asking again", d.tick,
                             p.info.name.c_str(), static_cast<unsigned long long>(kCompsWaitMs));
                    env_.Note(false, b);
                } else {
                    FinishComps(p);
                }
            }
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
        s.modsCompared = p->helloIn && p->hello.contribHash == contribHash_ && p->modsNote.empty();
        s.modsUnreliable = p->modsUnreliable;
        s.modsUnconfirmed = p->modsUnconfirmed;
        snprintf(s.modsNote, sizeof s.modsNote, "%s", p->modsNote.c_str());
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

bool Detector::EngineFlagged(uint32_t* tick, uint32_t atOrBefore) const {
    bool any = false;
    uint32_t best = 0;
    for (uint32_t t : engineTicks_)
        if (t <= atOrBefore && (!any || t > best)) {
            any = true;
            best = t;
        }
    if (any && tick) *tick = best;
    return any;
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
