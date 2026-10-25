// Offline self-test for the desync detector (src/wormsign/{exchange_wire,detector_core,bundle}.cpp): packet round
// trips and rejects, two detectors over a simulated link (clean, desync, mod contributor, skipped and faulted
// contributors, a slow or missing COMPS answer, loss, lag, vanilla and version-mismatched peers), the engine
// divergence picked for a correlation, the field diff, the log tail, the bundle, and a timed mutation run on the
// decoder.
// Usage: wormsign_detector_selftest [mutationSeconds=10]. Exit code 0 = all passed.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "miniz.h"
#include "tools/json_read.h"
#include "wormsign/bundle.h"
#include "wormsign/detector_core.h"
#include "wormsign/exchange_wire.h"
#include "wormsign/hash_engine.h"

using namespace melange::wormsign;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what);
    }
}

bool DecodeOk(const std::vector<uint8_t>& b, wire::Packet* p) {
    return wire::Decode(b.data(), b.size(), p) == wire::DecodeResult::Ok;
}

// ---------------------------------------------------------------- packets
void Packets() {
    wire::Packet p;
    {
        wire::Hello h;
        h.engineHash = 1;
        h.matchKey = 0x1122334455667788ull;
        h.firstTick = 3;
        h.contribHash = 42;
        h.seenYou = true;
        h.melange = "0.2.0";
        h.content16 = "0123456789abcdef";
        h.contribs = {{"mod.a.env", 1}, {"mod.b.env", 3}};
        const auto b = wire::Encode(h);
        Expect(DecodeOk(b, &p) && p.kind == wire::Kind::Hello, "hello decodes");
        Expect(p.hello.matchKey == h.matchKey && p.hello.firstTick == 3 && p.hello.contribHash == 42 && p.hello.seenYou &&
                   p.hello.melange == "0.2.0" && p.hello.content16 == h.content16 && p.hello.contribs.size() == 2 &&
                   p.hello.contribs[1].name == "mod.b.env" && p.hello.contribs[1].version == 3 && !p.hello.namesTruncated,
               "hello round trip");
        for (int i = 0; i < 200; ++i) h.contribs.push_back({"mod.some-long-mod-name-" + std::to_string(i) + ".env", 1});
        const auto big = wire::Encode(h);
        Expect(big.size() <= wire::kMaxPacket, "a long contributor list still fits one packet");
        Expect(DecodeOk(big, &p) && p.hello.namesTruncated && p.hello.contribs.size() < h.contribs.size(),
               "a long contributor list is cut and marked");
    }
    {
        wire::Hashes h;
        h.matchKey = 7;
        h.firstTick = 1000;
        for (uint32_t i = 0; i < wire::kMaxBatch; ++i) h.ticks.push_back({0x100 + i, 0x200 + i});
        h.present = ~0ull & ~(1ull << 5);
        const auto b = wire::Encode(h);
        Expect(b.size() <= wire::kMaxPacket, "a full HASHES batch fits");
        Expect(DecodeOk(b, &p) && p.kind == wire::Kind::Hashes && p.hashes.ticks.size() == wire::kMaxBatch &&
                   p.hashes.ticks[49].engine == 0x100 + 49 && p.hashes.ticks[49].mods == 0x200 + 49 &&
                   !(p.hashes.present >> 5 & 1) && (p.hashes.present >> 49 & 1) && !(p.hashes.present >> 50),
               "hashes round trip, present mask trimmed to n");
        std::vector<uint8_t> over = b;
        over[8 + 8 + 4] = 51;
        Expect(!DecodeOk(over, &p), "more than 50 ticks is refused");
    }
    {
        wire::Comps c;
        c.matchKey = 9;
        c.tick = 5000;
        c.have = true;
        c.engine = 1;
        c.mods = 2;
        for (int i = 0; i < 6; ++i) c.c[i] = 10 + i;
        c.contrib = {5, 6, 7};
        Expect(DecodeOk(wire::Encode(c), &p) && p.kind == wire::Kind::Comps && p.comps.tick == 5000 && p.comps.c[5] == 15 &&
                   p.comps.contrib.size() == 3 && p.comps.contrib[2] == 7,
               "comps round trip");
        c.contrib.assign(500, 1);
        const auto b = wire::Encode(c);
        Expect(b.size() <= wire::kMaxPacket && DecodeOk(b, &p) && p.comps.contrib.size() == wire::kMaxCompContribs,
               "comps caps the contributor list");
    }
    {
        wire::Flag f;
        f.matchKey = 3;
        f.tick = 77;
        f.engine = 8;
        f.c[2] = 99;
        Expect(DecodeOk(wire::Encode(f), &p) && p.kind == wire::Kind::Flag && p.flag.tick == 77 && p.flag.c[2] == 99,
               "flag round trip");
        const auto r = wire::Encode(wire::Kind::DetailReq, wire::TickReq{3, 12});
        Expect(DecodeOk(r, &p) && p.kind == wire::Kind::DetailReq && p.req.tick == 12, "detail request round trip");
    }
    {
        std::string json = "{\"worms\":[";
        for (int i = 0; i < 400; ++i) json += (i ? "," : "") + std::string("{\"slot\":") + std::to_string(i) + ",\"energy\":100}";
        json += "]}";
        auto chunks = wire::EncodeDetail(5, 60, json);
        Expect(chunks.size() > 1, "a detail record spans several chunks");
        bool fits = true;
        for (auto& c : chunks) fits = fits && c.size() <= wire::kMaxPacket;
        Expect(fits, "every detail chunk fits");
        std::mt19937 rng(5);
        std::shuffle(chunks.begin(), chunks.end(), rng);
        wire::DetailAssembly a;
        std::string out;
        int done = 0;
        for (size_t i = 0; i < chunks.size(); ++i) {
            Expect(DecodeOk(chunks[i], &p) && p.kind == wire::Kind::Detail, "detail chunk decodes");
            if (a.Add(p.detail, &out)) ++done;
            if (i == 0) Expect(!a.Add(p.detail, &out), "a duplicate chunk is ignored");
        }
        Expect(done == 1 && out == json, "detail chunks reassemble out of order");
        Expect(wire::EncodeDetail(5, 60, std::string(wire::kMaxDetailBytes + 1, 'x')).empty(), "a detail over 64 KB is not sent");
        wire::DetailChunk bad;
        bad.count = 2;
        bad.index = 0;
        bad.total = 5000;  // two chunks cannot hold 5000 bytes
        bad.bytes.assign(1024, 'x');
        Expect(!a.Add(bad, &out), "inconsistent chunk sizes are refused");
    }
    {
        const auto good = wire::Encode(wire::Kind::CompsReq, wire::TickReq{1, 2});
        auto magic = good;
        magic[0] ^= 1;
        Expect(wire::Decode(magic.data(), magic.size(), &p) == wire::DecodeResult::NotOurs, "another magic is not ours");
        auto proto = good;
        proto[5] = 2;
        Expect(wire::Decode(proto.data(), proto.size(), &p) == wire::DecodeResult::BadProtocol && p.protocol == 2,
               "protocol 2 is reported as such");
        auto len = good;
        len[6] ^= 1;
        Expect(wire::Decode(len.data(), len.size(), &p) == wire::DecodeResult::Malformed, "a wrong length is refused");
        auto kind = good;
        kind[4] = 99;
        Expect(wire::Decode(kind.data(), kind.size(), &p) == wire::DecodeResult::Malformed, "an unknown kind is refused");
        std::vector<uint8_t> huge(wire::kMaxPacket + 1, 0);
        memcpy(huge.data(), &wire::kMagic, 4);
        Expect(wire::Decode(huge.data(), huge.size(), &p) == wire::DecodeResult::NotOurs, "over 1100 bytes is refused");
        int prefixOk = 0;
        wire::Hello h;
        h.contribs = {{"x", 1}};
        const auto hb = wire::Encode(h);
        for (size_t n = 0; n < hb.size(); ++n) {
            auto cut = std::vector<uint8_t>(hb.begin(), hb.begin() + static_cast<ptrdiff_t>(n));
            if (n >= 8) {
                const uint16_t l = static_cast<uint16_t>(n);
                memcpy(cut.data() + 6, &l, 2);
            }
            if (wire::Decode(cut.data(), cut.size(), &p) == wire::DecodeResult::Ok) ++prefixOk;
        }
        Expect(prefixOk == 0, "no truncated HELLO decodes");
    }
    Expect(wire::MatchKey(1, 2) == wire::MatchKey(1, 2) && wire::MatchKey(1, 2) != wire::MatchKey(2, 1), "match key");
    Expect(wire::ContribListHash({{"a", 1}}) != wire::ContribListHash({{"a", 2}}) &&
               wire::ContribListHash({{"ab", 1}, {"c", 1}}) != wire::ContribListHash({{"a", 1}, {"bc", 1}}),
           "contributor list hash covers names, boundaries and versions");
}

// ---------------------------------------------------------------- compare
void Compare() {
    auto cmp = std::make_unique<detect::PeerCompare>();
    cmp->Reset();
    for (uint32_t t = 1; t <= 100; ++t) cmp->AddOurs(t, t, 0);
    for (uint32_t t = 1; t <= 100; ++t) cmp->AddTheirs(t, t, 0);
    Expect(cmp->Compared() == 100 && cmp->Matched() == 100 && !cmp->Diverged(), "equal ticks compare clean");
    cmp->AddTheirs(50, 999, 0);
    Expect(cmp->Compared() == 100, "a resent tick is not compared twice");
    for (uint32_t t = 101; t <= 110; ++t) cmp->AddTheirs(t, t == 105 ? 0 : t, 0);
    for (uint32_t t = 101; t <= 110; ++t) cmp->AddOurs(t, t, 0);
    Expect(cmp->Diverged() && cmp->DivergedTick() == 105 && cmp->TakeNewDivergence() && !cmp->TakeNewDivergence(),
           "theirs first: the mismatch is flagged once at its tick");
    Expect(cmp->LastCommon() == 110 && cmp->MismatchedAfter() == 0, "later ticks still count");

    cmp->Reset();
    cmp->SetCompareMods(false);
    cmp->AddOurs(1, 5, 1);
    cmp->AddTheirs(1, 5, 2);
    Expect(!cmp->Diverged(), "mods are ignored when the contributor lists differ");
    cmp->SetCompareMods(true);
    cmp->AddOurs(2, 5, 1);
    cmp->AddTheirs(2, 5, 2);
    Expect(cmp->Diverged() && cmp->DivergedTick() == 2, "mods count when the lists match");

    cmp->Reset();
    cmp->AddOurs(10000, 1, 0);
    Expect(!cmp->AddTheirs(10, 1, 0) && !cmp->AddTheirs(20000, 1, 0) && cmp->Dropped() == 2, "far ticks are dropped");
    Expect(detect::CompList(0x05) == "time+rng, worms" && detect::CompList(0) == "", "component list");
}

// ---------------------------------------------------------------- two detectors over a link
struct Link;
struct Side final : detect::Env {
    uint64_t id = 0;
    Link* link = nullptr;
    std::map<uint32_t, TickHash> ring;
    std::map<uint32_t, std::string> detail;
    std::map<uint32_t, std::vector<uint64_t>> contrib;
    std::vector<Divergence> diverged;
    std::vector<detect::Report> bundles;
    std::vector<std::string> notes;
    std::map<uint64_t, int> sentTo;
    uint64_t now = 0;
    uint64_t bytesSent = 0, packetsSent = 0;
    uint64_t NowMs() override { return now; }
    bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable) override;
    bool OurTick(uint32_t tick, TickHash* out) override {
        auto it = ring.find(tick);
        if (it == ring.end()) return false;
        *out = it->second;
        return true;
    }
    std::string Detail(uint32_t tick) override {
        auto it = detail.find(tick);
        return it == detail.end() ? std::string() : it->second;
    }
    bool ContribHashes(uint32_t tick, std::vector<uint64_t>* out) override {
        auto it = contrib.find(tick);
        if (it == contrib.end()) return false;
        *out = it->second;
        return true;
    }
    void Diverged(const Divergence& d) override { diverged.push_back(d); }
    void Bundle(const detect::Report& r) override { bundles.push_back(r); }
    void Note(bool, const std::string& text) override { notes.push_back(text); }
    bool Noted(const char* part) const {
        for (const auto& n : notes)
            if (n.find(part) != std::string::npos) return true;
        return false;
    }
};

struct Link {
    struct Msg {
        uint64_t from, to;
        std::vector<uint8_t> bytes;
        bool reliable;
        uint64_t due;
    };
    std::deque<Msg> q;
    uint64_t latencyMs = 40;
    double loss = 0;
    std::mt19937 rng{7};
    uint64_t dropped = 0;
    // COMPS answers from `compsFrom`: dropped, or delayed by compsDelayMs on top of the latency.
    uint64_t compsFrom = 0, compsDelayMs = 0, compsSeen = 0;
    bool compsDrop = false;
    void Push(uint64_t from, uint64_t to, const std::vector<uint8_t>& b, bool reliable, uint64_t now) {
        if (!reliable && loss > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < loss) {
            ++dropped;
            return;
        }
        uint64_t extra = 0;
        if (compsFrom && from == compsFrom) {
            wire::Packet p;
            if (wire::Decode(b.data(), b.size(), &p) == wire::DecodeResult::Ok && p.kind == wire::Kind::Comps) {
                ++compsSeen;
                if (compsDrop) return;
                extra = compsDelayMs;
            }
        }
        q.push_back({from, to, b, reliable, now + latencyMs + extra});
    }
};

bool Side::Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable) {
    ++sentTo[to];
    ++packetsSent;
    bytesSent += packet.size();
    link->Push(id, to, packet, reliable, now);
    return true;
}

TickHash Hash(uint32_t tick, uint64_t salt, bool wormsOff, bool modsOff) {
    TickHash h{};
    h.tick = tick;
    for (int i = 0; i < kEngineComps; ++i) h.c[i] = (tick * 1000003ull + i) ^ 0x5555;
    if (wormsOff) h.c[kWorms] ^= salt;
    h.engine = 0;
    for (int i = 0; i < kEngineComps; ++i) h.engine = h.engine * 31 + h.c[i];
    h.mods = modsOff ? 0xbad : 0x600d + tick;
    return h;
}

struct Scenario {
    uint32_t ticks = 3000;
    uint32_t desyncAt = 0;         // on B from this tick on
    bool viaMods = false;          // B's mod contributor differs instead of the worms
    // B skipped its first contributor on ticks skipAt..skipAt+9 (demoted there): it reports 0 for it and folds
    // its last hash into its mods aggregate, so the aggregate is stale for those ticks and equal again after.
    uint32_t skipAt = 0;
    bool skipBoth = false;         // B skips both contributors on those ticks
    bool viaMods1 = false;         // with viaMods: B's second contributor differs instead of the first
    uint32_t faultAt = 0;          // B's first contributor faults at this tick: 0 from then on, left out of its mods
    // The mods hash is the contributors' fold, as contrib::HashTick computes it (else a stand-in that only says
    // equal or not). Needed wherever the detector tells a faulted contributor from a skipped one.
    bool foldMods = false;
    bool bCompsDrop = false;       // B's COMPS answers never arrive
    uint64_t bCompsDelayMs = 0;    // B's COMPS answers arrive this much later than other packets
    uint32_t bLag = 0;             // B runs this many ticks behind
    uint32_t bBegin = 0;           // B's session opens at this frame (packets to it before are dropped)
    double loss = 0;
    std::string bAdvert = "1";
    uint32_t bEngineHash = kEngineHashVersion;
    std::vector<wire::ContribName> contribs = {{"mod.desync-probe.env", 1}, {"mod.sim-sampler.env", 1}};
    std::vector<wire::ContribName> bContribs;   // B's list when it differs from A's (an older Melange)
};

struct Result {
    Side a, b;
    bool aFlagged = false, aEngineFlagged = false, aEngineBefore = false;
    uint32_t aFlaggedTick = 0, aEngineFlaggedTick = 0, aEngineBeforeTick = 0;
    uint64_t compsSeen = 0;
    uint64_t aLatencyMs = 0, bLatencyMs = 0;
    std::vector<detect::PeerStatus> aPeers, bPeers;
    uint64_t dropped = 0;
};

std::unique_ptr<Result> Run(const Scenario& s) {
    auto r = std::make_unique<Result>();
    Link link;
    link.loss = s.loss;
    link.compsFrom = s.bCompsDrop || s.bCompsDelayMs ? 76561198000000002ull : 0;
    link.compsDrop = s.bCompsDrop;
    link.compsDelayMs = s.bCompsDelayMs;
    Side& A = r->a;
    Side& B = r->b;
    A.id = 76561198000000001ull;
    B.id = 76561198000000002ull;
    A.link = B.link = &link;
    detect::Detector da(A), db(B);
    da.SetIdentity("0.2.0", "0123456789abcdef");
    db.SetIdentity("0.2.0", "0123456789abcdef");
    da.SetPeers({{B.id, "B", s.bAdvert}});
    db.SetPeers({{A.id, "A", "1"}});
    const uint64_t key = wire::MatchKey(A.id, 99);
    da.Begin(1, key, s.contribs);
    const auto& bList = s.bContribs.empty() ? s.contribs : s.bContribs;
    if (!s.bBegin) db.Begin(1, key, bList);
    uint64_t desyncWallA = 0, desyncWallB = 0;
    const uint32_t frames = s.ticks + s.bLag + 400;
    for (uint32_t f = 1; f <= frames; ++f) {
        const uint64_t now = f * 20ull;
        A.now = B.now = now;
        auto tick = [&](Side& side, detect::Detector& d, uint32_t t, bool isB) {
            if (t < 1 || t > s.ticks) return;
            const bool off = isB && s.desyncAt && t >= s.desyncAt;
            const bool skipped = isB && s.skipAt && t >= s.skipAt && t < s.skipAt + 10;
            const bool faulted = isB && s.faultAt && t >= s.faultAt;
            TickHash h = Hash(t, 0x77, off && !s.viaMods, off && s.viaMods);
            if (skipped) h.mods = Hash(s.skipAt - 1, 0x77, false, false).mods;  // the stale fold-in
            // Per contributor: the value computed this tick (0: not computed), and what the aggregate folds in.
            uint64_t v[2] = {off && s.viaMods && !s.viaMods1 ? 0xbadull : 0x1ull + t, off && s.viaMods && s.viaMods1 ? 0xbad1ull : 0x2ull + t};
            uint64_t fold[2] = {v[0], v[1]};
            if (skipped) {
                v[0] = 0, fold[0] = 0x1ull + (s.skipAt - 1);
                if (s.skipBoth) v[1] = 0, fold[1] = 0x2ull + (s.skipAt - 1);
            }
            if (faulted) v[0] = fold[0] = 0;
            if (s.foldMods) {
                const auto& names = isB && !s.bContribs.empty() ? s.bContribs : s.contribs;
                uint64_t all = kFnvBasis;
                for (size_t i = 0; i < names.size() && i < 2; ++i)
                    if (fold[i]) all = FnvV(fold[i], FnvV(Fnv(names[i].name.data(), names[i].name.size()), all));
                h.mods = all;
            }
            side.ring[t] = h;
            std::string energy = off && !s.viaMods ? "1" : "100";
            side.detail[t] = "{\"tick\":" + std::to_string(t) + ",\"worms\":[{\"slot\":0,\"energy\":100},{\"slot\":3,\"energy\":" +
                             energy + ",\"pos\":{\"x\":1.5,\"y\":2}}]}";
            side.contrib[t] = {v[0], v[1]};
            if (side.ring.size() > 600) {
                side.ring.erase(side.ring.begin());
                side.detail.erase(side.detail.begin());
                side.contrib.erase(side.contrib.begin());
            }
            d.OnTick(h);
            if (isB && s.desyncAt && t == s.desyncAt) desyncWallB = now;
            if (!isB && s.desyncAt && t == s.desyncAt) desyncWallA = now;
        };
        if (s.bBegin && f == s.bBegin) db.Begin(1, key, bList);
        tick(A, da, f, false);
        tick(B, db, f > s.bLag ? f - s.bLag : 0, true);
        for (size_t i = 0; i < link.q.size();) {
            if (link.q[i].due > now) {
                ++i;
                continue;
            }
            Link::Msg m = std::move(link.q[i]);
            link.q.erase(link.q.begin() + static_cast<std::ptrdiff_t>(i));
            (m.to == A.id ? da : db).OnPacket(m.from, m.bytes.data(), m.bytes.size());
        }
        const size_t na = A.diverged.size(), nb = B.diverged.size();
        da.Pump();
        db.Pump();
        const uint64_t later = (std::max)(desyncWallA, desyncWallB);
        if (na == 0 && A.diverged.size()) r->aLatencyMs = now - later;
        if (nb == 0 && B.diverged.size()) r->bLatencyMs = now - later;
    }
    detect::PeerStatus v[4];
    r->aPeers.assign(v, v + da.Peers(v, 4));
    r->bPeers.assign(v, v + db.Peers(v, 4));
    r->aFlagged = da.Flagged(&r->aFlaggedTick);
    r->aEngineFlagged = da.EngineFlagged(&r->aEngineFlaggedTick);
    r->aEngineBefore = da.EngineFlagged(&r->aEngineBeforeTick, s.desyncAt ? s.desyncAt - 1 : 0);
    r->compsSeen = link.compsSeen;
    da.End();
    db.End();
    r->dropped = link.dropped;
    return r;
}

void Pair() {
    {
        auto r = Run(Scenario{});
        Expect(r->a.diverged.empty() && r->b.diverged.empty(), "clean match: no divergence");
        Expect(r->aPeers.size() == 1 && r->aPeers[0].state == detect::PeerState::Exchanging && r->aPeers[0].modsCompared,
               "clean match: exchanging with mods compared");
        char b[160];
        snprintf(b, sizeof b, "clean match: every common tick but the last batch compared (A %u, B %u of 3000)",
                 r->aPeers[0].compared, r->bPeers[0].compared);
        const uint32_t floor = 3000 - detect::Detector::kBatchEvery;
        Expect(r->aPeers[0].compared >= floor && r->bPeers[0].compared >= floor, b);
        const double seconds = 3400 * 0.02;
        const double bps = static_cast<double>(r->a.bytesSent) / seconds, pps = static_cast<double>(r->a.packetsSent) / seconds;
        snprintf(b, sizeof b, "exchange cost %.0f B/s and %.2f packets/s each way (budget 1536, 3)", bps, pps);
        printf("  %s\n", b);
        Expect(bps <= 1536 && pps <= 3, b);
    }
    {
        Scenario s;
        s.desyncAt = 1500;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1, "desync: both sides report once");
        if (r->a.diverged.size() == 1 && r->b.diverged.size() == 1) {
            const Divergence& da = r->a.diverged[0];
            const Divergence& db = r->b.diverged[0];
            Expect(da.tick == 1500 && db.tick == 1500, "desync: both report exactly tick T");
            Expect(da.compMask == (1u << kWorms) && db.compMask == (1u << kWorms), "desync: the worms component");
            Expect(da.peer == r->b.id && db.peer == r->a.id && da.source == Source::Peer, "desync: the peer");
            Expect(da.contrib[0] == 0 && da.oursEngine != da.theirsEngine, "desync: engine, no contributor");
            char b[120];
            snprintf(b, sizeof b, "desync: reported %llu / %llu ms after the tick (<= 1000)", r->aLatencyMs, r->bLatencyMs);
            printf("  %s\n", b);
            Expect(r->aLatencyMs <= 1000 && r->bLatencyMs <= 1000, b);
        }
        Expect(r->a.bundles.size() == 1 && r->b.bundles.size() == 1, "desync: one bundle each");
        Expect(r->aFlagged && r->aFlaggedTick == 1500 && r->aEngineFlagged && r->aEngineFlaggedTick == 1500,
               "desync: flagged as an engine divergence at 1500");
        Expect(!r->aEngineBefore, "desync: no engine divergence at or before tick 1499");
        if (r->a.bundles.size() == 1) {
            const detect::Report& rep = r->a.bundles[0];
            Expect(!rep.detailLocal.empty() && !rep.detailPeer.empty(), "desync: both detail records in the bundle");
            std::string err;
            const auto lines = detect::FieldDiff(rep.detailLocal, rep.detailPeer, &err);
            Expect(lines.size() == 1 && lines[0] == "worms[1].energy 100 -> 1", "desync: the diff names the field");
            Expect(rep.haveOurs && rep.haveTheirs && rep.ours[kWorms] != rep.theirs[kWorms], "desync: both components");
        }
        Expect(r->aPeers[0].state == detect::PeerState::Diverged && r->aPeers[0].divergedTick == 1500 &&
                   r->aPeers[0].apartTicks > 1000,
               "desync: the panel shows how long the peers stay apart");
    }
    {
        Scenario s;
        s.desyncAt = 700;
        s.viaMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1, "mod desync: both sides report");
        if (r->a.diverged.size() == 1 && r->b.diverged.size() == 1) {
            Expect(r->a.diverged[0].tick == 700 && r->b.diverged[0].tick == 700, "mod desync: exactly tick T");
            Expect(r->a.diverged[0].compMask == 0 && !strcmp(r->a.diverged[0].contrib, "mod.desync-probe.env") &&
                       !strcmp(r->b.diverged[0].contrib, "mod.desync-probe.env"),
                   "mod desync: the contributor is named on both sides");
        }
        Expect(r->aFlagged && r->aFlaggedTick == 700 && !r->aEngineFlagged, "mod desync: flagged, but not as an engine divergence");
    }
    // B skipped a contributor on ticks 900..909 (demoted there, as Melange <= 0.11.1 did to a slow mod environment
    // digest): its mods aggregate is stale for those ticks and its COMPS answer has 0 for that contributor. Neither
    // side reports a desync; both stop comparing the mods hash with the other and say so; the engine hash is still
    // compared and a real engine desync later in the match is still found.
    {
        Scenario s;
        s.skipAt = 900;
        auto r = Run(s);
        Expect(r->a.diverged.empty() && r->b.diverged.empty() && r->a.bundles.empty() && r->b.bundles.empty(),
               "skipped contributor: no desync reported on either side");
        Expect(!r->aFlagged && !r->aEngineFlagged, "skipped contributor: nothing flagged");
        Expect(r->aPeers.size() == 1 && r->aPeers[0].state == detect::PeerState::Exchanging && !r->aPeers[0].modsCompared &&
                   r->aPeers[0].modsUnreliable && r->aPeers[0].divergedTick == 0,
               "skipped contributor: A exchanging, mods no longer compared, marked unreliable");
        Expect(r->bPeers.size() == 1 && r->bPeers[0].state == detect::PeerState::Exchanging && !r->bPeers[0].modsCompared &&
                   r->bPeers[0].modsUnreliable,
               "skipped contributor: the skipping side B comes to the same verdict");
        Expect(r->aPeers[0].compared >= 3000 - detect::Detector::kBatchEvery, "skipped contributor: every tick still compared");
    }
    {
        Scenario s;
        s.skipAt = 900;
        s.desyncAt = 1500;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1 && r->a.diverged[0].tick == 1500 &&
                   r->a.diverged[0].compMask == (1u << kWorms),
               "skipped contributor, then an engine desync: the engine desync is still found at its tick");
        Expect(r->aEngineFlagged && r->aEngineFlaggedTick == 1500, "skipped contributor, then an engine desync: engine-flagged");
    }
    {
        // A mods-only mismatch where both sides computed the contributor is a desync as before.
        Scenario s;
        s.skipAt = 900;
        s.desyncAt = 2000;
        s.viaMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.empty() && r->aPeers[0].modsUnreliable,
               "skipped contributor, then a mod desync: not seen (the mods hash is no longer compared with that peer)");
    }
    {
        Scenario s;
        s.desyncAt = 900;
        s.contribs.clear();
        s.viaMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->a.diverged[0].tick == 900 && r->a.diverged[0].contrib[0] == 0,
               "mods hash without named contributors: flagged, no name");
    }
    // The same skip with the mods hash folded from the contributors, as Melange computes it: B's one zero is folded
    // in as a stale value (its mods hash is not the fold of its computed values), so it is a demotion skip.
    {
        Scenario s;
        s.skipAt = 900;
        s.foldMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.empty() && r->b.diverged.empty() && r->a.bundles.empty() && r->b.bundles.empty() &&
                   r->aPeers[0].modsUnreliable && !r->aPeers[0].modsCompared,
               "skipped contributor (folded mods): no desync, mods no longer compared");
        Expect(r->a.Noted("B did not hash mod.desync-probe.env at that tick (demoted there)") &&
                   r->b.Noted("we did not hash mod.desync-probe.env at that tick (demoted there)"),
               "skipped contributor (folded mods): both logs name the skipped contributor and who skipped it");
        Expect(strstr(r->aPeers[0].modsNote, "skipped on one side at tick 900") != nullptr &&
                   strstr(r->aPeers[0].modsNote, "cannot tell") == nullptr,
               "skipped contributor (folded mods): the Peers panel says why");
    }
    // Two differing contributors at one tick: B skipped the first (demoted there) and computed a different value of
    // the second. The second is a real desync: reported on both sides, naming it, with the skipped one logged.
    {
        Scenario s;
        s.skipAt = 900;
        s.desyncAt = 900;
        s.viaMods = s.viaMods1 = true;
        s.foldMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1 && r->a.diverged[0].tick == 900 &&
                   r->b.diverged[0].tick == 900,
               "one skipped, one real contributor: a desync on both sides at the tick");
        if (r->a.diverged.size() == 1 && r->b.diverged.size() == 1)
            Expect(!strcmp(r->a.diverged[0].contrib, "mod.sim-sampler.env") &&
                       !strcmp(r->b.diverged[0].contrib, "mod.sim-sampler.env") && r->a.diverged[0].compMask == 0,
                   "one skipped, one real contributor: the real one is named, not the skipped one");
        Expect(r->a.bundles.size() == 1 && r->b.bundles.size() == 1, "one skipped, one real contributor: one bundle each");
        Expect(r->a.Noted("mod.sim-sampler.env differs from B with both sides hashing it") &&
                   r->a.Noted("B did not hash mod.desync-probe.env at that tick"),
               "one skipped, one real contributor: the skipped one is still logged");
        Expect(!r->aPeers[0].modsUnreliable, "one skipped, one real contributor: not marked unreliable");
    }
    // B skipped both contributors on the same ticks: every differing contributor is a skip, so no desync. With two
    // zeros on B a fault among them cannot be ruled out, and the panel says so.
    {
        Scenario s;
        s.skipAt = 900;
        s.skipBoth = true;
        s.foldMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.empty() && r->b.diverged.empty() && r->a.bundles.empty() && r->b.bundles.empty() &&
                   !r->aFlagged,
               "all differing contributors skipped: no desync on either side");
        Expect(r->aPeers[0].modsUnreliable && strstr(r->aPeers[0].modsNote, "cannot tell") != nullptr &&
                   r->a.Noted("B did not hash mod.desync-probe.env") && r->a.Noted("B did not hash mod.sim-sampler.env"),
               "all differing contributors skipped: both logged, the panel says a fault cannot be told apart");
    }
    // B's first contributor faults at tick 900: B's mods hash leaves it out from then on (the fold of its computed
    // values is exactly its mods hash), which the data tells apart from a demotion skip: a desync naming it.
    {
        Scenario s;
        s.faultAt = 900;
        s.foldMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1 && r->a.diverged[0].tick == 900 &&
                   r->b.diverged[0].tick == 900,
               "faulted contributor: a desync on both sides at the fault's tick");
        if (r->a.diverged.size() == 1 && r->b.diverged.size() == 1)
            Expect(!strcmp(r->a.diverged[0].contrib, "mod.desync-probe.env") &&
                       !strcmp(r->b.diverged[0].contrib, "mod.desync-probe.env"),
                   "faulted contributor: named on both sides");
        Expect(r->a.Noted("mod.desync-probe.env is not in B's mods hash (faulted there") &&
                   r->b.Noted("mod.desync-probe.env is not in our mods hash"),
               "faulted contributor: the log says it faulted, and where");
        Expect(!r->aPeers[0].modsUnreliable && r->a.bundles.size() == 1, "faulted contributor: a bundle, not unreliable");
    }
    // A mods-only mismatch whose COMPS answer never arrives: asked twice, then unconfirmed on A (log and panel, no
    // desync, no bundle, nothing flagged). B got A's answer and reports the desync.
    {
        Scenario s;
        s.desyncAt = 700;
        s.viaMods = true;
        s.bCompsDrop = true;
        auto r = Run(s);
        Expect(r->a.diverged.empty() && r->a.bundles.empty() && !r->aFlagged,
               "mods-only mismatch without COMPS: not a desync on A, no bundle");
        Expect(r->compsSeen == 2 && r->a.Noted("asking again") && r->a.Noted("unconfirmed"),
               "mods-only mismatch without COMPS: asked twice, then logged as unconfirmed");
        Expect(r->aPeers[0].modsUnconfirmed && !r->aPeers[0].modsCompared &&
                   strstr(r->aPeers[0].modsNote, "not confirmed") != nullptr &&
                   r->aPeers[0].state == detect::PeerState::Exchanging,
               "mods-only mismatch without COMPS: the Peers panel says unconfirmed");
        Expect(r->b.diverged.size() == 1 && r->b.diverged[0].tick == 700, "mods-only mismatch without COMPS: B still reports it");
    }
    // The answer arrives late, after the first wait: the second wait catches it and the desync is confirmed.
    {
        Scenario s;
        s.desyncAt = 700;
        s.viaMods = true;
        s.bCompsDelayMs = 1000;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->a.diverged[0].tick == 700 &&
                   !strcmp(r->a.diverged[0].contrib, "mod.desync-probe.env") && r->a.bundles.size() == 1 &&
                   !r->aPeers[0].modsUnconfirmed,
               "mods-only mismatch with a COMPS answer 1 s late: confirmed and named");
    }
    // An engine mismatch without COMPS keeps the old path: confirmed after the first wait.
    {
        Scenario s;
        s.desyncAt = 1500;
        s.bCompsDrop = true;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->a.diverged[0].tick == 1500 && r->a.diverged[0].compMask == (1u << kWorms) &&
                   r->a.bundles.size() == 1 && r->compsSeen == 1,
               "engine mismatch without COMPS: confirmed after one request");
    }
    // The melange.camera contributor against a peer without it (an older Melange or HashCamera=0): its mods hash
    // differs on every tick, yet only the engine hash is compared, so there is no false desync and a real one is
    // still found.
    {
        Scenario s;
        s.contribs = {{"melange.camera", 1}, {"mod.desync-probe.env", 1}};
        s.bContribs = {{"mod.desync-probe.env", 1}};
        s.desyncAt = 1;
        s.viaMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.empty() && r->b.diverged.empty(), "camera vs older peer: no false desync");
        Expect(r->aPeers.size() == 1 && r->aPeers[0].state == detect::PeerState::Exchanging &&
                   !r->aPeers[0].modsCompared && r->bPeers.size() == 1 && !r->bPeers[0].modsCompared &&
                   r->aPeers[0].compared > 2900,
               "camera vs older peer: engine hashes still exchanged, mods not compared");
    }
    {
        Scenario s;
        s.contribs = {{"melange.camera", 1}, {"mod.desync-probe.env", 1}};
        s.bContribs = {{"mod.desync-probe.env", 1}};
        s.desyncAt = 1200;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->a.diverged[0].tick == 1200 && r->a.diverged[0].compMask == (1u << kWorms),
               "camera vs older peer: an engine desync is still found");
    }
    {
        Scenario s;
        s.contribs = {{"melange.camera", 1}, {"mod.desync-probe.env", 1}};
        s.desyncAt = 800;
        s.viaMods = true;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->a.diverged[0].tick == 800 &&
                   !strcmp(r->a.diverged[0].contrib, "melange.camera") && r->a.diverged[0].compMask == 0,
               "camera on both sides: a camera desync names melange.camera");
    }
    {
        Scenario s;
        s.desyncAt = 2000;
        s.bLag = 180;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1 && r->a.diverged[0].tick == 2000 &&
                   r->b.diverged[0].tick == 2000,
               "lagging peer (3.6 s behind): both report tick T");
        Expect(r->aPeers[0].compared >= 2000 && r->bPeers[0].compared >= 2000, "lagging peer: ticks compared");
    }
    {
        Scenario s;
        s.desyncAt = 1502;
        auto r = Run(s);
        char b[120];
        snprintf(b, sizeof b, "desync just after a batch: reported %llu / %llu ms after the tick (<= 1000)", r->aLatencyMs,
                 r->bLatencyMs);
        printf("  %s\n", b);
        Expect(r->a.diverged.size() == 1 && r->a.diverged[0].tick == 1502 && r->aLatencyMs <= 1000 && r->bLatencyMs <= 1000, b);
    }
    {
        Scenario s;
        s.desyncAt = 1200;
        s.bBegin = 90;
        s.bLag = 90;
        auto r = Run(s);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1 && r->a.diverged[0].tick == 1200 &&
                   r->b.diverged[0].tick == 1200,
               "a peer whose match opens 1.8 s later: both report tick T");
    }
    {
        Scenario s;
        s.desyncAt = 2500;
        s.loss = 0.3;
        auto r = Run(s);
        char b[120];
        snprintf(b, sizeof b, "30%% loss (%llu dropped): both report tick T", r->dropped);
        Expect(r->a.diverged.size() == 1 && r->b.diverged.size() == 1 && r->a.diverged[0].tick == 2500 &&
                   r->b.diverged[0].tick == 2500,
               b);
    }
    {
        Scenario s;
        s.bAdvert = "";
        s.desyncAt = 100;
        auto r = Run(s);
        Expect(r->a.sentTo[r->b.id] == 0 && r->a.packetsSent == 0, "vanilla peer: not one packet sent to it");
        Expect(r->aPeers[0].state == detect::PeerState::NoExchange && r->a.diverged.empty(), "vanilla peer: no exchange");
    }
    {
        Scenario s;
        s.bAdvert = "2";
        auto r = Run(s);
        Expect(r->a.packetsSent == 0 && r->aPeers[0].state == detect::PeerState::VersionMismatch,
               "mlg.ws=2: no packets, version mismatch");
    }
}

// ---------------------------------------------------------------- the engine divergence for a correlation
// A with two peers: B's engine state differs from tick 1000 on, C's from 2000 on. EngineFlagged(atOrBefore) picks
// the latest engine divergence at or before the engine check's failed tick, never a later one.
void EnginePick() {
    Link link;
    Side sides[3];
    const uint64_t ids[3] = {76561198000000011ull, 76561198000000012ull, 76561198000000013ull};
    const uint32_t desyncAt[3] = {0, 1000, 2000};
    std::vector<std::unique_ptr<detect::Detector>> dets;
    const uint64_t key = wire::MatchKey(ids[0], 77);
    const std::vector<wire::ContribName> contribs = {{"mod.desync-probe.env", 1}};
    for (int i = 0; i < 3; ++i) {
        sides[i].id = ids[i];
        sides[i].link = &link;
        dets.push_back(std::make_unique<detect::Detector>(sides[i]));
        dets[i]->SetIdentity("0.2.0", "0123456789abcdef");
    }
    dets[0]->SetPeers({{ids[1], "B", "1"}, {ids[2], "C", "1"}});
    dets[1]->SetPeers({{ids[0], "A", "1"}});
    dets[2]->SetPeers({{ids[0], "A", "1"}});
    for (auto& d : dets) d->Begin(1, key, contribs);
    for (uint32_t f = 1; f <= 2600; ++f) {
        for (int i = 0; i < 3; ++i) {
            Side& s = sides[i];
            s.now = f * 20ull;
            if (f > 2200) continue;
            const bool off = desyncAt[i] && f >= desyncAt[i];
            const TickHash h = Hash(f, 0x30 + i, off, false);
            s.ring[f] = h;
            s.contrib[f] = {0x1ull + f};
            if (s.ring.size() > 600) {
                s.ring.erase(s.ring.begin());
                s.contrib.erase(s.contrib.begin());
            }
            dets[i]->OnTick(h);
        }
        for (size_t i = 0; i < link.q.size();) {
            if (link.q[i].due > f * 20ull) {
                ++i;
                continue;
            }
            Link::Msg m = std::move(link.q[i]);
            link.q.erase(link.q.begin() + static_cast<std::ptrdiff_t>(i));
            for (int k = 0; k < 3; ++k)
                if (m.to == ids[k]) dets[k]->OnPacket(m.from, m.bytes.data(), m.bytes.size());
        }
        for (auto& d : dets) d->Pump();
    }
    uint32_t t = 0;
    Expect(sides[0].diverged.size() == 2, "engine pick: A reports both peers");
    Expect(!dets[0]->EngineFlagged(&t, 999), "engine pick: nothing at or before 999");
    t = 0;
    Expect(dets[0]->EngineFlagged(&t, 1000) && t == 1000, "engine pick: 1000 for a check failing at 1000");
    t = 0;
    Expect(dets[0]->EngineFlagged(&t, 1999) && t == 1000, "engine pick: 1000 for a check failing at 1999, not the later 2000");
    t = 0;
    Expect(dets[0]->EngineFlagged(&t, 2400) && t == 2000, "engine pick: 2000 for a check failing at 2400");
    t = 0;
    Expect(dets[0]->EngineFlagged(&t) && t == 2000, "engine pick: the latest without a bound");
    for (auto& d : dets) d->End();
}

// ---------------------------------------------------------------- diff, tail, bundle
void DiffAndTail() {
    std::string err;
    auto lines = detect::FieldDiff(R"({"a":1,"b":{"c":[1,2,3]},"s":"x","f":0.5,"gone":true})",
                                   R"({"a":1,"b":{"c":[1,5,3,4]},"s":"y","f":0.25,"new":null})", &err);
    const std::vector<std::string> want = {"b.c[1] 2 -> 5", "s \"x\" -> \"y\"", "f 0.5 -> 0.25", "gone true -> (none)",
                                           "b.c[3] (none) -> 4", "new (none) -> null"};
    Expect(lines == want, "field diff: nested values, missing and extra fields");
    lines = detect::FieldDiff("{", "{}", &err);
    Expect(lines.empty() && !err.empty(), "field diff: bad JSON is an error");
    const std::string log =
        "10:00:00.000 [+   1.000] [ 1] INFO a\r\n10:01:00.000 [+  60.000] [ 1] INFO b\r\ncontinued\r\n"
        "10:03:30.000 [+ 210.000] [ 1] INFO c\r\n";
    Expect(detect::LogTailSeconds(log, 120) == "10:03:30.000 [+ 210.000] [ 1] INFO c\r\n", "log tail: last 2 minutes");
    Expect(detect::LogTailSeconds(log, 150) == log.substr(log.find("10:01")), "log tail keeps continuation lines");
    Expect(detect::LogTailSeconds("no stamps", 120) == "no stamps", "log tail without stamps keeps everything");
}

std::string ZipEntry(const std::string& zip, const char* name) {
    mz_zip_archive z{};
    if (!mz_zip_reader_init_mem(&z, zip.data(), zip.size(), 0)) return "<no zip>";
    size_t n = 0;
    void* p = mz_zip_reader_extract_file_to_heap(&z, name, &n, 0);
    std::string s = p ? std::string(static_cast<const char*>(p), n) : "<missing>";
    if (p) mz_free(p);
    mz_zip_reader_end(&z);
    return s;
}

void Bundle() {
    bundle::Inputs in;
    in.salt = "salt";
    in.div.source = Source::Peer;
    in.div.serial = 3;
    in.div.tick = 5000;
    in.div.oursEngine = 1;
    in.div.theirsEngine = 2;
    in.div.compMask = 1u << kWorms;
    in.div.peer = 76561198012345678ull;
    in.haveOurs = in.haveTheirs = true;
    in.ours[kWorms] = 10;
    in.theirs[kWorms] = 11;
    in.contributors = {{"mod.desync-probe.env", 1, 5, 5, true, true}, {"mod.sandtide.env", 1, 0x33f3cd58fac5765bull, 0, true, true}};
    in.detailLocal = R"({"rng":"0xe01dc3c7","worms":[{"slot":3,"energy":100}]})";
    in.detailPeer = R"({"rng":"0x42ecc01e","worms":[{"slot":3,"energy":1}]})";
    in.rngTrail = {{4998, 0x11111111u}, {4999, 0x42ecc01eu}, {5000, 0xe01dc3c7u}};
    in.melangeLog = "12:00:00.000 [+   1.000] [ 1] INFO C:\\Users\\Tester\\Documents peer 76561198012345678 ip 10.1.2.3\r\n";
    in.userName = "Tester";
    in.computerName = "DESKTOP-XYZ";
    in.sysinfo = R"({"computer":"DESKTOP-XYZ"})";
    in.recording = {'W', 'S', 'R', '1'};
    in.melangeVersion = "0.2.0";
    in.exeBuild = "1077";
    in.correlation = "Wormsign flagged tick 5000; the engine's turn-end check failed at tick 7488 (reason 5, Random's dont match)";
    std::string zip;
    std::vector<std::string> entries;
    Expect(bundle::BuildZip(in, &zip, &entries), "bundle builds");
    Expect(entries.size() == 8, "bundle has report, diff, both details, log, mods, sysinfo and the recording");
    const std::string report = ZipEntry(zip, "report.json");
    melange::json::Value v;
    melange::json::Error e;
    Expect(melange::json::Parse(report, &v, &e) && v.Get("tick") && v.Get("tick")->number == 5000, "report.json parses");
    Expect(report.find("76561198012345678") == std::string::npos && v.Get("peer") &&
               v.Get("peer")->string.rfind("hash:", 0) == 0,
           "report.json hashes the peer's SteamID");
    const std::string diff = ZipEntry(zip, "diff.txt");
    Expect(diff.find("\nworms[0].energy 100 -> 1\n") != std::string::npos, "diff.txt names the changed field");
    Expect(diff.find("# component worms") != std::string::npos && diff.find("reason 5") != std::string::npos,
           "diff.txt has the component and the engine correlation");
    Expect(diff.find("# contributor mod.sandtide.env 33f3cd58fac5765b -> (not hashed on the peer at this tick: demoted or faulted there)\n") !=
               std::string::npos,
           "diff.txt says a 0 contributor value was not computed on the peer");
    Expect(report.find("\"name\":\"mod.sandtide.env\"") != std::string::npos && report.find("\"theirsComputed\":false") != std::string::npos &&
               report.find("\"oursComputed\":true") != std::string::npos,
           "report.json carries the computed flags");
    Expect(diff.find("# rng: ours 0xe01dc3c7, theirs 0x42ecc01e (= our value at the end of tick 4999): the peer is 57 logic-RNG draws behind us\n") !=
               std::string::npos,
           ("diff.txt decodes the logic-RNG distance: " + diff).c_str());
    Expect(bundle::RngSteps(0x42ecc01eu, 0xe01dc3c7u) == 57 && bundle::RngSteps(0xe01dc3c7u, 0x42ecc01eu) == -1 &&
               bundle::RngSteps(5, 5) == 0,
           "RngSteps follows the game's LCG");
    {
        bundle::Inputs ahead = in;
        ahead.detailLocal = R"({"rng":"0x42ecc01e"})";
        ahead.detailPeer = R"({"rng":"0xe01dc3c7"})";
        ahead.rngTrail.clear();
        Expect(bundle::RngLine(ahead) == "# rng: ours 0x42ecc01e, theirs 0xe01dc3c7: the peer is 57 logic-RNG draws ahead of us",
               ("rng line: ahead: " + bundle::RngLine(ahead)).c_str());
        ahead.detailPeer = R"({"rng":"0x42ecc01e"})";
        Expect(bundle::RngLine(ahead) == "# rng: both 0x42ecc01e", "rng line: equal");
        ahead.detailPeer = R"({"worms":[]})";
        Expect(bundle::RngLine(ahead).empty(), "rng line: none without both values");
    }
    const std::string log = ZipEntry(zip, "logs/Melange.log");
    Expect(log.find("Tester") == std::string::npos && log.find("76561198012345678") == std::string::npos &&
               log.find("10.1.2.3") == std::string::npos,
           "the log is redacted");
    Expect(ZipEntry(zip, "sysinfo.txt").find("DESKTOP-XYZ") == std::string::npos, "the computer name is replaced");
    Expect(ZipEntry(zip, "match.wsr") == "WSR1", "the recording is stored as is");
    Expect(bundle::FileName(3, 5000, 2026, 9, 29, 14, 5, 7, 42) == L"desync-20260929-140507-p42-m3-t5000.zip", "bundle file name");
    bundle::Inputs none;
    none.div.tick = 1;
    Expect(bundle::BuildZip(none, &zip, nullptr) && ZipEntry(zip, "diff.txt").find("no field diff") != std::string::npos,
           "a bundle without detail records says so");
}

// ---------------------------------------------------------------- mutation
struct NullEnv final : detect::Env {
    uint64_t now = 0;
    uint64_t NowMs() override { return now; }
    bool Send(uint64_t, const std::vector<uint8_t>&, bool) override { return true; }
    bool OurTick(uint32_t tick, TickHash* out) override {
        *out = Hash(tick, 0, false, false);
        return tick > 0;
    }
    std::string Detail(uint32_t) override { return "{\"a\":1}"; }
    bool ContribHashes(uint32_t, std::vector<uint64_t>* out) override {
        out->assign(2, 1);
        return true;
    }
    void Diverged(const Divergence&) override {}
    void Bundle(const detect::Report&) override {}
    void Note(bool, const std::string&) override {}
};

void Mutation(double seconds) {
    std::mt19937 rng(1077);
    std::vector<std::vector<uint8_t>> seeds;
    wire::Hello h;
    h.contribs = {{"mod.a.env", 1}};
    h.matchKey = 5;
    seeds.push_back(wire::Encode(h));
    wire::Hashes hs;
    hs.matchKey = 5;
    hs.firstTick = 10;
    hs.present = 0xff;
    for (int i = 0; i < 8; ++i) hs.ticks.push_back({1, 2});
    seeds.push_back(wire::Encode(hs));
    seeds.push_back(wire::Encode(wire::Kind::CompsReq, wire::TickReq{5, 12}));
    seeds.push_back(wire::Encode(wire::Kind::DetailReq, wire::TickReq{5, 12}));
    wire::Comps c;
    c.matchKey = 5;
    c.have = true;
    c.contrib = {1, 2};
    seeds.push_back(wire::Encode(c));
    for (auto& d : wire::EncodeDetail(5, 12, std::string(3000, 'q'))) seeds.push_back(d);
    wire::Flag f;
    f.matchKey = 5;
    seeds.push_back(wire::Encode(f));
    NullEnv env;
    detect::Detector det(env);
    det.SetPeers({{9, "p", "1"}});
    det.Begin(1, 5, {{"mod.a.env", 1}});
    uint64_t runs = 0, ok = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    wire::Packet p;
    while (std::chrono::steady_clock::now() < end) {
        for (int k = 0; k < 256; ++k, ++runs) {
            std::vector<uint8_t> b = seeds[rng() % seeds.size()];
            const int mode = static_cast<int>(rng() % 4);
            if (mode == 0) {
                for (int m = 1 + static_cast<int>(rng() % 4); m-- && !b.empty();) b[rng() % b.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
            } else if (mode == 1) {
                b.resize(rng() % (b.size() + 16));
            } else if (mode == 2) {
                b.resize(rng() % 1200);
                for (auto& x : b) x = static_cast<uint8_t>(rng());
                if (b.size() >= 8) memcpy(b.data(), &wire::kMagic, 4), b[5] = 1;
            } else {
                if (b.size() > 8) b[8 + rng() % (b.size() - 8)] = static_cast<uint8_t>(rng());
            }
            if (b.size() >= 8) {
                const uint16_t n = static_cast<uint16_t>(b.size());
                if (rng() % 2) memcpy(b.data() + 6, &n, 2);
            }
            if (wire::Decode(b.data(), b.size(), &p) == wire::DecodeResult::Ok) ++ok;
            det.OnPacket(rng() % 3 ? 9 : 10, b.data(), b.size());
            if ((runs & 63) == 0) {
                det.SetPeers({{9, "p", (runs & 4095) == 0 ? "x" : "1"}});
                env.now += 20;
                det.OnTick(Hash(static_cast<uint32_t>(runs / 64) + 1, 0, false, false));
                det.Pump();
            }
        }
    }
    printf("  mutation: %llu packets in %.0f s, %llu decoded as valid\n", runs, seconds, ok);
    Expect(runs > 0, "mutation run on the decoder and the detector");
}
}  // namespace

// A mods-only mismatch found, then the peer's HELLO names other contributors before COMPS confirms it: dropped (not a
// desync, no bundle, nothing flagged) and the mods hash is no longer compared with that peer. Both orders: the HELLO
// after Start (the compare is turned off under a pending report whose lists matched) and between the mismatch and
// Start (the report records the lists as differing).
void HelloChange() {
    for (int order = 0; order < 2; ++order) {
        const char* when = order ? "HELLO before Start" : "HELLO after Start";
        Link link;
        Side A;
        A.id = 76561198000000001ull;
        A.link = &link;
        const uint64_t bId = 76561198000000002ull;
        detect::Detector da(A);
        da.SetIdentity("0.2.0", "0123456789abcdef");
        da.SetPeers({{bId, "B", "1"}});
        const uint64_t key = wire::MatchKey(A.id, 99);
        const std::vector<wire::ContribName> same = {{"mod.desync-probe.env", 1}, {"mod.sim-sampler.env", 1}};
        const std::vector<wire::ContribName> other = {{"mod.desync-probe.env", 1}};
        da.Begin(1, key, same);
        auto hello = [&](const std::vector<wire::ContribName>& list) {
            wire::Hello h;
            h.engineHash = kEngineHashVersion;
            h.matchKey = key;
            h.firstTick = 1;
            h.contribHash = wire::ContribListHash(list);
            h.seenYou = true;
            h.melange = "0.2.0";
            h.content16 = "0123456789abcdef";
            h.contribs = list;
            const std::vector<uint8_t> p = wire::Encode(h);
            da.OnPacket(bId, p.data(), p.size());
        };
        hello(same);
        wire::Hashes hs;
        hs.matchKey = key;
        hs.firstTick = 1;
        for (uint32_t t = 1; t <= 10; ++t) {
            const TickHash h = Hash(t, 0x77, false, false);
            A.ring[t] = h;
            A.contrib[t] = {1ull + t, 2ull + t};
            da.OnTick(h);
            hs.present |= 1ull << (t - 1);
            hs.ticks.push_back({h.engine, t >= 5 ? 0xbadull : h.mods});  // from tick 5 B's mods hash differs, engine equal
        }
        A.now = 200;
        const std::vector<uint8_t> hp = wire::Encode(hs);
        da.OnPacket(bId, hp.data(), hp.size());
        if (order == 1) hello(other);
        da.Pump();  // Start: FLAG, COMPS and DETAIL requests
        Expect(A.Noted("tick 5 differs from B"), (std::string(when) + ": the mismatch is found at tick 5").c_str());
        if (order == 0) hello(other);
        wire::Comps c;
        c.matchKey = key;
        c.tick = 5;
        c.have = true;
        const TickHash h5 = Hash(5, 0x77, false, false);
        c.engine = h5.engine;
        c.mods = 0xbad;
        memcpy(c.c, h5.c, sizeof c.c);
        c.contrib = {0xbadull};  // B's one contributor
        const std::vector<uint8_t> cp = wire::Encode(c);
        A.now = 300;
        da.OnPacket(bId, cp.data(), cp.size());
        da.Pump();
        Expect(A.diverged.empty() && A.bundles.empty(), (std::string(when) + ": not a desync, no bundle").c_str());
        uint32_t flagged = 0;
        Expect(!da.Flagged(&flagged), (std::string(when) + ": nothing flagged").c_str());
        Expect(A.Noted("contributor lists changed before it was confirmed"), (std::string(when) + ": the log says why").c_str());
        detect::PeerStatus v[2];
        const int n = da.Peers(v, 2);
        Expect(n == 1 && !v[0].modsCompared && strstr(v[0].modsNote, "contributor lists changed") != nullptr &&
                   v[0].state == detect::PeerState::Exchanging,
               (std::string(when) + ": the Peers panel says mods are not compared, and why").c_str());
        // The compare goes on with the engine hash: an engine mismatch later is still a desync.
        wire::Hashes hs2;
        hs2.matchKey = key;
        hs2.firstTick = 11;
        for (uint32_t t = 11; t <= 20; ++t) {
            const TickHash h = Hash(t, 0x77, false, false);
            A.ring[t] = h;
            A.contrib[t] = {1ull + t, 2ull + t};
            da.OnTick(h);
            hs2.present |= 1ull << (t - 11);
            hs2.ticks.push_back({t >= 15 ? h.engine ^ 0x77 : h.engine, 0xbadull});
        }
        A.now = 400;
        const std::vector<uint8_t> hp2 = wire::Encode(hs2);
        da.OnPacket(bId, hp2.data(), hp2.size());
        da.Pump();
        A.now = 400 + detect::Detector::kCompsWaitMs + 20;
        da.Pump();
        Expect(A.diverged.size() == 1 && A.diverged[0].tick == 15, (std::string(when) + ": an engine desync after it is still found").c_str());
        da.End();
    }
}

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? atof(argv[1]) : 10.0;
    Packets();
    Compare();
    Pair();
    HelloChange();
    EnginePick();
    DiffAndTail();
    Bundle();
    Mutation(seconds);
    printf("wormsign_detector_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
