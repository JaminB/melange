// Offline self-test for the peer bus: wire encode and decode, fragmentation and reassembly with loss and reordering,
// the rate limiter, the NTP min-RTT offset, the gates (no mlg.peer, version mismatch, no session, lobbyKey change)
// and a malformed-packet fuzz; then two state machines (peerbus_core) wired through a fake network with skewed clocks.
// Exit code 0 = all passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "net/peerbus_core.h"
#include "net/peerbus_wire.h"

using namespace melange::peerbus;
namespace w = melange::peerbus::wire;

namespace {
int g_pass = 0, g_fail = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

w::Value Str(const std::string& s) {
    w::Value v;
    v.type = w::Value::Type::Str;
    v.s = s;
    return v;
}
w::Value Int(int64_t i) {
    w::Value v;
    v.type = w::Value::Type::Int;
    v.i = i;
    return v;
}
w::Value Num(double n) {
    w::Value v;
    v.type = w::Value::Type::Num;
    v.n = n;
    return v;
}
w::Value Bool(bool b) {
    w::Value v;
    v.type = w::Value::Type::Bool;
    v.b = b;
    return v;
}
w::Value Tab(std::vector<std::pair<w::Value, w::Value>> t = {}) {
    w::Value v;
    v.type = w::Value::Type::Table;
    v.t = std::move(t);
    return v;
}
bool Same(const w::Value& a, const w::Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case w::Value::Type::Nil: return true;
        case w::Value::Type::Bool: return a.b == b.b;
        case w::Value::Type::Int: return a.i == b.i;
        case w::Value::Type::Num: return std::memcmp(&a.n, &b.n, 8) == 0;
        case w::Value::Type::Str: return a.s == b.s;
        case w::Value::Type::Table:
            if (a.t.size() != b.t.size()) return false;
            for (size_t i = 0; i < a.t.size(); ++i)
                if (!Same(a.t[i].first, b.t[i].first) || !Same(a.t[i].second, b.t[i].second)) return false;
            return true;
    }
    return false;
}
std::vector<uint8_t> Enc(const w::Value& v) {
    std::vector<uint8_t> b;
    w::EncodeValue(v, &b, nullptr);
    return b;
}
w::Value Nest(int levels) {  // `levels` tables inside each other
    w::Value v = Tab();
    for (int i = 1; i < levels; ++i) v = Tab({{Int(1), v}});
    return v;
}

// ---------------------------------------------------------------- wire
void TestHeader() {
    w::Header h;
    h.kind = w::Kind::Msg;
    h.reliable = true;
    h.lobbyKey = 0x1122334455667788ULL;
    h.modId = "holo";
    h.seq = 513;
    h.frag = 2;
    h.nfrag = 3;
    const uint8_t body[] = {1, 2, 3};
    std::vector<uint8_t> p = w::Encode(h, body, 3);
    Expect(p.size() == w::HeaderBytes(4) + 3, "header size is 21 + modIdLen");
    Expect(p.size() >= 4 && std::memcmp(p.data(), "MPB1", 4) == 0, "magic MPB1");
    w::Packet out;
    Expect(w::Decode(p.data(), p.size(), &out) == w::DecodeResult::Ok, "decode ok");
    Expect(out.h.kind == w::Kind::Msg && out.h.reliable && out.h.lobbyKey == h.lobbyKey && out.h.modId == "holo" &&
               out.h.seq == 513 && out.h.frag == 2 && out.h.nfrag == 3 && out.body.size() == 3 && out.body[2] == 3,
           "round trip");

    auto mutated = [&](size_t at, uint8_t v) {
        std::vector<uint8_t> q = p;
        q[at] = v;
        return w::Decode(q.data(), q.size(), &out);
    };
    Expect(mutated(0, 'X') == w::DecodeResult::NotOurs, "bad magic is not ours");
    Expect(mutated(5, 2) == w::DecodeResult::BadProtocol, "protocol 2");
    Expect(mutated(6, static_cast<uint8_t>(p.size() + 1)) == w::DecodeResult::NotOurs, "length field mismatch");
    Expect(mutated(4, 9 | w::kReliableBit) == w::DecodeResult::Malformed, "unknown kind");
    Expect(mutated(4, static_cast<uint8_t>(w::Kind::Msg)) == w::DecodeResult::Malformed, "unreliable with nfrag 3");
    const size_t fragAt = w::HeaderBytes(4) - 2;
    Expect(mutated(fragAt, 3) == w::DecodeResult::Malformed, "frag >= nfrag");
    Expect(mutated(fragAt + 1, 0) == w::DecodeResult::Malformed, "nfrag 0");
    Expect(mutated(fragAt + 1, 9) == w::DecodeResult::Malformed, "nfrag 9");
    Expect(mutated(17, 'A') == w::DecodeResult::Malformed, "upper-case mod id");
    Expect(w::Decode(p.data(), 10, &out) == w::DecodeResult::NotOurs, "too short");
    std::vector<uint8_t> big(w::kMaxPacket + 1, 0);
    Expect(w::Decode(big.data(), big.size(), &out) == w::DecodeResult::NotOurs, "too long");

    w::Header ping;
    ping.kind = w::Kind::Ping;
    ping.lobbyKey = 7;
    std::vector<uint8_t> pb = w::PingBody(12.5);
    std::vector<uint8_t> pp = w::Encode(ping, pb.data(), pb.size());
    double t0 = 0;
    Expect(w::Decode(pp.data(), pp.size(), &out) == w::DecodeResult::Ok && w::ParsePing(out.body, &t0) && t0 == 12.5, "PING");
    std::vector<uint8_t> shortBody = w::Encode(ping, pb.data(), 7);
    Expect(w::Decode(shortBody.data(), shortBody.size(), &out) == w::DecodeResult::Malformed, "PING body of 7 bytes");
    ping.modId = "holo";
    std::vector<uint8_t> withId = w::Encode(ping, pb.data(), pb.size());
    Expect(w::Decode(withId.data(), withId.size(), &out) == w::DecodeResult::Malformed, "PING with a mod id");
    w::Header hello;
    hello.kind = w::Kind::Hello;
    std::vector<uint8_t> hb = w::HelloBody(3);
    std::vector<uint8_t> hp = w::Encode(hello, hb.data(), hb.size());
    Expect(w::Decode(hp.data(), hp.size(), &out) == w::DecodeResult::Malformed, "HELLO without a mod id");
    uint16_t ver = 0;
    hello.modId = "holo";
    hp = w::Encode(hello, hb.data(), hb.size());
    Expect(w::Decode(hp.data(), hp.size(), &out) == w::DecodeResult::Ok && w::ParseHello(out.body, &ver) && ver == 3, "HELLO");
    double a = 0, b = 0, c = 0;
    Expect(w::ParsePong(w::PongBody(1, 2, 3), &a, &b, &c) && a == 1 && b == 2 && c == 3, "PONG body");
    h.modId = std::string(65, 'a');
    Expect(w::Encode(h, body, 3).empty(), "a 65-character mod id is refused");
    h.modId = "holo";
    std::vector<uint8_t> huge(w::kMaxPacket, 0);
    Expect(w::Encode(h, huge.data(), huge.size()).empty(), "a packet over 1100 bytes is refused");
}

void TestValues() {
    w::Value v = Tab({{Str("s"), Str("hello")},
                      {Int(1), Int(-1)},
                      {Int(2), Int(INT64_MIN)},
                      {Int(3), Int(INT64_MAX)},
                      {Str("f"), Num(2.5)},
                      {Str("neg"), Num(-0.0)},
                      {Str("inf"), Num(INFINITY)},
                      {Num(0.5), Str("float key")},
                      {Bool(true), Str("bool key")},
                      {Str("t"), Tab({{Str("deep"), Nest(4)}})},
                      {Str("b"), Bool(true)},
                      {Str("n"), w::Value{}}});
    std::vector<uint8_t> b;
    Expect(w::EncodeValue(v, &b, nullptr), "encode a mixed table");
    w::Value back;
    Expect(w::DecodeValue(b.data(), b.size(), &back) && Same(v, back), "value round trip");
    Expect(Enc(Int(5)).size() == 2 && Enc(Int(-64)).size() == 2, "small integers are 2 bytes");

    std::string why;
    Expect(!w::EncodeValue(Str(std::string(1025, 'x')), &b, &why) && why == "size", "1025-byte string");
    Expect(w::EncodeValue(Str(std::string(1024, 'x')), &b, &why), "1024-byte string");
    Expect(w::EncodeValue(Nest(6), &b, &why), "6 levels");
    Expect(!w::EncodeValue(Nest(7), &b, &why), "7 levels");
    w::Value wide = Tab();
    for (int i = 0; i < 257; ++i) wide.t.emplace_back(Int(i), Int(i));
    Expect(!w::EncodeValue(wide, &b, &why), "257 keys");
    wide.t.pop_back();
    Expect(w::EncodeValue(wide, &b, &why), "256 keys");
    Expect(!w::EncodeValue(Tab({{w::Value{}, Int(1)}}), &b, &why), "a nil key");
    Expect(!w::EncodeValue(Tab({{Num(NAN), Int(1)}}), &b, &why), "a NaN key");
    Expect(!w::EncodeValue(Tab({{Tab(), Int(1)}}), &b, &why), "a table key");

    // hostile input
    const uint8_t nilKey[] = {6, 1, 0, 3, 2};
    Expect(!w::DecodeValue(nilKey, sizeof nilKey, &back), "decode: nil key");
    std::vector<uint8_t> nanKey = {6, 1, 4};
    const double nan = NAN;
    nanKey.insert(nanKey.end(), reinterpret_cast<const uint8_t*>(&nan), reinterpret_cast<const uint8_t*>(&nan) + 8);
    nanKey.push_back(0);
    Expect(!w::DecodeValue(nanKey.data(), nanKey.size(), &back), "decode: NaN key");
    const uint8_t tableKey[] = {6, 1, 6, 0, 0};
    Expect(!w::DecodeValue(tableKey, sizeof tableKey, &back), "decode: table key");
    const uint8_t trailing[] = {0, 0};
    Expect(!w::DecodeValue(trailing, sizeof trailing, &back), "decode: trailing bytes");
    const uint8_t hugeCount[] = {6, 0xff, 0xff, 0x03, 0, 0};
    Expect(!w::DecodeValue(hugeCount, sizeof hugeCount, &back), "decode: a key count over 256");
    const uint8_t longStr[] = {5, 0x81, 0x08};  // 1025
    Expect(!w::DecodeValue(longStr, sizeof longStr, &back), "decode: string length over 1024");
    const uint8_t shortStr[] = {5, 4, 'a', 'b'};
    Expect(!w::DecodeValue(shortStr, sizeof shortStr, &back), "decode: a string past the end");
    const uint8_t badVar[] = {3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f};
    Expect(!w::DecodeValue(badVar, sizeof badVar, &back), "decode: varint over 64 bits");
    std::vector<uint8_t> deep;
    for (int i = 0; i < 7; ++i) deep.insert(deep.end(), {6, 1, 3, 2});
    deep.push_back(0);
    Expect(!w::DecodeValue(deep.data(), deep.size(), &back), "decode: 7 levels");
    Expect(!w::DecodeValue(nullptr, 0, &back), "decode: empty");
}

void TestFragments() {
    std::vector<uint8_t> payload(8192);
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(i * 7);
    auto frags = w::EncodeMessage(42, "holo", 9, true, payload);
    Expect(frags.size() == 8, "8 KB in 8 fragments");
    bool sizes = true;
    for (const auto& f : frags) sizes = sizes && f.size() <= w::kMaxPacket;
    Expect(sizes, "every fragment fits 1100 bytes");
    Expect(w::EncodeMessage(42, "holo", 9, true, std::vector<uint8_t>(8193)).empty(), "8193 bytes are refused");
    Expect(w::EncodeMessage(42, "holo", 9, false, std::vector<uint8_t>(900)).size() == 1, "900 bytes unreliable");
    Expect(w::EncodeMessage(42, "holo", 9, false, std::vector<uint8_t>(901)).empty(), "901 bytes unreliable are refused");
    Expect(w::EncodeMessage(42, "holo", 9, true, {}).size() == 1, "an empty payload is one packet");
    const std::string longId(64, 'm');
    Expect(w::MaxMessage(64, true) == 8 * w::ChunkBytes(64) && w::MaxMessage(64, true) < 8192, "a 64-character id caps at 8120");
    Expect(w::EncodeMessage(1, longId, 0, true, std::vector<uint8_t>(w::MaxMessage(64, true))).size() == 8, "8120 bytes fit");

    const size_t chunk = w::ChunkBytes(4);
    auto feed = [&](w::Reassembler& r, uint64_t from, const std::vector<std::vector<uint8_t>>& pkts, std::vector<size_t> order,
                    double now, std::vector<uint8_t>* got) {
        int complete = 0;
        for (size_t i : order) {
            w::Packet p;
            if (w::Decode(pkts[i].data(), pkts[i].size(), &p) != w::DecodeResult::Ok) continue;
            if (r.Add(from, p.h, p.body, chunk, now, got) == w::Reassembler::Result::Complete) ++complete;
        }
        return complete;
    };
    w::Reassembler r;
    std::vector<uint8_t> got;
    Expect(feed(r, 1, frags, {0, 1, 2, 3, 4, 5, 6, 7}, 0, &got) == 1 && got == payload, "in order");
    Expect(feed(r, 1, frags, {7, 6, 5, 4, 3, 2, 1, 0}, 0, &got) == 0, "a repeat of a delivered message is ignored");
    auto rev = w::EncodeMessage(42, "holo", 10, true, payload);
    Expect(feed(r, 1, rev, {7, 6, 5, 4, 3, 2, 1, 0}, 0, &got) == 1 && got == payload, "reversed");
    std::mt19937 rng(7);
    for (int round = 0; round < 50; ++round) {
        auto m = w::EncodeMessage(42, "holo", static_cast<uint16_t>(100 + round), true, payload);
        std::vector<size_t> order = {0, 1, 2, 3, 4, 5, 6, 7, 3, 5, 0};  // with duplicates
        std::shuffle(order.begin(), order.end(), rng);
        got.clear();
        if (feed(r, 2, m, order, 0, &got) != 1 || got != payload) {
            Expect(false, "shuffled with duplicates, round " + std::to_string(round));
            break;
        }
    }
    Expect(r.Partials() == 0 && r.PartialBytes() == 0, "nothing left over");
    Expect(feed(r, 3, frags, {0, 1, 2, 4, 5, 6, 7}, 100, &got) == 0 && r.Partials() == 1, "a lost fragment leaves a partial");
    r.Expire(100 + w::Reassembler::kTimeoutMs - 1);
    Expect(r.Partials() == 1, "kept before 5 s");
    r.Expire(100 + w::Reassembler::kTimeoutMs + 1);
    Expect(r.Partials() == 0 && r.PartialBytes() == 0, "dropped after 5 s");

    // at most 2 partial messages per sender
    auto other = w::EncodeMessage(42, "holo", 10, true, payload);
    auto third = w::EncodeMessage(42, "holo", 11, true, payload);
    feed(r, 4, frags, {0}, 0, &got);
    feed(r, 4, other, {0}, 0, &got);
    const uint64_t before = r.Dropped();
    feed(r, 4, third, {0}, 0, &got);
    Expect(r.Partials() == 2 && r.Dropped() == before + 1, "a third partial from one sender is dropped");
    r.Clear();
    // at most 64 KB of partial buffers in total
    int senders = 0;
    for (uint64_t s = 10; s < 30; ++s) {
        auto m = w::EncodeMessage(42, "holo", 1, true, payload);
        feed(r, s, m, {0, 1, 2, 3, 4, 5, 6}, 0, &got);
        ++senders;
    }
    Expect(r.PartialBytes() <= w::Reassembler::kMaxPartialBytes && r.Partials() < static_cast<size_t>(senders),
           "partial buffers stay under 64 KB (" + std::to_string(r.Partials()) + " kept)");
    r.Clear();
    // a fragment that does not match the message's shape
    w::Packet p0, bad;
    w::Decode(frags[0].data(), frags[0].size(), &p0);
    Expect(r.Add(5, p0.h, p0.body, chunk, 0, &got) == w::Reassembler::Result::Pending, "first fragment");
    bad = p0;
    bad.h.nfrag = 5;
    bad.h.frag = 1;
    Expect(r.Add(5, bad.h, bad.body, chunk, 0, &got) == w::Reassembler::Result::Dropped && r.Partials() == 0,
           "the same seq with another nfrag drops the message");
    bad = p0;
    bad.h.frag = 1;
    bad.body.resize(10);
    Expect(r.Add(5, bad.h, bad.body, chunk, 0, &got) == w::Reassembler::Result::Dropped, "a short middle fragment");
}

void TestRates() {
    w::SendLimiter lim;
    int ok = 0;
    for (int i = 0; i < 15; ++i)
        if (lim.Can(10, 1000)) {
            lim.Take(10, 1000);
            ++ok;
        }
    Expect(ok == 10, "burst 10: " + std::to_string(ok));
    Expect(!lim.Can(10, 1040), "nothing after 40 ms");
    Expect(lim.Can(10, 1050), "one more after 50 ms (20 a second)");
    ok = 0;
    for (double t = 2000; t < 12000; t += 10)
        if (lim.Can(10, t)) {
            lim.Take(10, t);
            ++ok;
        }
    Expect(ok >= 199 && ok <= 211, "about 20 messages a second over 10 s: " + std::to_string(ok));
    w::SendLimiter bytes;
    Expect(bytes.Can(8192, 0), "one 8 KB message");
    bytes.Take(8192, 0);
    Expect(!bytes.Can(100, 1) && bytes.Can(4000, 500), "8 KB a second");

    w::GlobalBudget g;
    g.SetDestinations(4);
    int toA = 0;
    while (g.Can(1, 1000, 0)) {
        g.Take(1, 1000, 0);
        ++toA;
    }
    Expect(toA == 9, "one destination of 4 gets its share (9 KB burst): " + std::to_string(toA));
    Expect(g.Can(2, 1000, 0), "another destination is not starved");
    int total = toA;
    for (uint64_t d = 2; d <= 5; ++d)
        while (g.Can(d, 1000, 0)) {
            g.Take(d, 1000, 0);
            ++total;
        }
    Expect(total <= 33, "the channel total stays at 32 KB: " + std::to_string(total));
}

void TestClock() {
    w::ClockSample s;
    // their clock is 1000 ms ahead; 20 ms each way; they hold it 2 ms
    Expect(w::NtpSample(100, 1120, 1122, 142, &s) && std::fabs(s.rtt - 40) < 1e-9 && std::fabs(s.offset - 1000) < 1e-9, "NTP math");
    Expect(!w::NtpSample(100, 1120, 1122, 90, &s), "t3 before t0");
    Expect(!w::NtpSample(100, 1120, 1110, 142, &s), "t2 before t1");
    Expect(!w::NtpSample(100, 0, 1, 20000, &s), "rtt over 10 s");
    w::ClockEstimator c;
    Expect(!c.Ready(), "no sample yet");
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> jitter(0, 60);
    double t = 0;
    for (int i = 0; i < 8; ++i) {
        const double up = 15 + jitter(rng), down = 15 + jitter(rng);
        const double t0 = t, t1 = t0 + up + 500, t2 = t1 + 1, t3 = t0 + up + 1 + down;
        c.Add(t0, t1, t2, t3);
        t += 1000;
    }
    const double clean0 = t, clean1 = clean0 + 15 + 500, clean2 = clean1 + 1, clean3 = clean0 + 15 + 1 + 15;
    c.Add(clean0, clean1, clean2, clean3);  // the minimum-RTT sample: offset exactly 500
    Expect(std::fabs(c.Offset() - 500) < 1e-9, "the offset of the minimum-RTT sample: " + std::to_string(c.Offset()));
    Expect(c.Samples() == 9 && c.Rtt() > 30 && c.Rtt() < 130, "EWMA RTT: " + std::to_string(c.Rtt()));
    for (int i = 0; i < 8; ++i) {
        const double t0 = t + 10000 + i * 1000;
        c.Add(t0, t0 + 600, t0 + 601, t0 + 201);  // rtt 200, offset 500
    }
    Expect(std::fabs(c.Offset() - 500) < 1e-9 && c.Rtt() > 100, "the window keeps only the last 8 samples");
    c.Reset();
    Expect(!c.Ready() && c.Samples() == 0, "reset");
    Expect(w::PingInterval(4999, 0) == 1000 && w::PingInterval(5000, 0) == 3000, "1 s pings for 5 s, then 3 s");
}

void TestAdvertAndGates() {
    Expect(w::BuildAdvert({}) == "", "no channel, no advert");
    Expect(w::BuildAdvert({{"zed", 1}, {"holo", 1}, {"holo", 1}}) == "1;holo@1,zed@1", "sorted, unique");
    std::vector<w::AdvertEntry> e;
    Expect(w::ParseAdvert("1;holo@1,BAD@1,x@0,y@abc,ok@65535,z", &e) && e.size() == 2 && e[1].modId == "ok", "tolerant parse");
    Expect(!w::ParseAdvert("2;holo@1", &e), "another advert format");
    Expect(w::Advertises("1;holo@1,kanly@2", "holo", 1), "advertises holo@1");
    Expect(!w::Advertises("1;holo@2", "holo", 1), "version mismatch");
    Expect(w::Advertises("1;holo@2", "holo", -1), "any version");
    Expect(!w::Advertises("", "holo", 1), "no mlg.peer");
    Expect(!w::Advertises("1;holox@1", "holo", 1), "a prefix is not the mod");

    w::GateIn g;
    g.channelOpen = false;
    Expect(w::Gate(g) == SendStatus::Closed, "closed first");
    g.channelOpen = true;
    Expect(w::Gate(g) == SendStatus::Offline, "offline");
    g.online = true;
    Expect(w::Gate(g) == SendStatus::NoPeer, "not a member");
    g.member = true;
    Expect(w::Gate(g) == SendStatus::NoPeer, "no mlg.peer");
    g.advertises = true;
    Expect(w::Gate(g) == SendStatus::NoSession, "no session");
    g.session = true;
    Expect(w::Gate(g) == SendStatus::Ok, "every gate open");
    g.advertises = false;
    Expect(w::Gate(g) == SendStatus::NoPeer, "a session alone is not enough");
    for (SendStatus st : {SendStatus::Size, SendStatus::Rate, SendStatus::NoPeer, SendStatus::NoSession, SendStatus::Offline,
                          SendStatus::Closed})
        Expect(std::string(w::StatusName(st)).find('?') == std::string::npos, "a reason for every status");
}

// ---------------------------------------------------------------- two state machines on a fake network
struct Net;
struct Node;
struct NodeIo final : Io {
    Net* net = nullptr;
    Node* self = nullptr;
    bool SessionActive(uint64_t member) override;
    bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable, bool noDelay) override;
};
struct Node {
    uint64_t id = 0;
    double skew = 0;  // its clock = global + skew
    NodeIo io;
    Core core{&io};
    std::vector<Delivery> got;
    std::vector<PeerChange> changes;
    int sent = 0;
    std::map<uint64_t, int> sentTo;
    std::map<uint64_t, bool> session;
};
struct Net {
    struct Pkt {
        double due;
        uint64_t from, to;
        std::vector<uint8_t> bytes;
    };
    double t = 0, latency = 20, loss = 0;
    std::vector<Pkt> inflight;
    std::vector<Node*> nodes;
    std::mt19937 rng{11};
    void Step(double dt) {
        t += dt;
        for (Node* n : nodes) {
            const double now = t + n->skew;
            std::vector<Pkt> keep, mine;
            for (Pkt& p : inflight) (p.to == n->id && p.due <= t ? mine : keep).push_back(std::move(p));
            inflight.swap(keep);
            for (Pkt& p : mine) n->core.OnPacket(p.from, p.bytes.data(), p.bytes.size(), now);
            n->core.Tick(now);
            for (auto& d : n->core.TakeDeliveries()) n->got.push_back(std::move(d));
            for (auto& c : n->core.TakePeerChanges()) n->changes.push_back(c);
        }
    }
    void Run(double ms, double dt = 16) {
        for (double e = 0; e < ms; e += dt) Step(dt);
    }
};
bool NodeIo::SessionActive(uint64_t member) {
    auto it = self->session.find(member);
    return it != self->session.end() && it->second;
}
bool NodeIo::Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable, bool) {
    ++self->sent;
    ++self->sentTo[to];
    if (!reliable && net->loss > 0 && std::uniform_real_distribution<double>(0, 1)(net->rng) < net->loss) return true;
    net->inflight.push_back({net->t + net->latency, self->id, to, packet});
    return true;
}

LobbyInfo LobbyFor(uint64_t lobby, uint64_t owner, uint64_t me, const std::vector<std::pair<uint64_t, std::string>>& others) {
    LobbyInfo l;
    l.lobby = lobby;
    l.owner = owner;
    l.me = me;
    l.key = (lobby * 1000003ULL) ^ owner;  // stands in for wormsign::wire::MatchKey
    for (const auto& [id, adv] : others) l.members.push_back({id, adv});
    return l;
}

bool WentDown(const std::vector<PeerChange>& v, uint64_t id) {
    for (const auto& c : v)
        if (!c.up && c.id == id) return true;
    return false;
}

void TestCore() {
    Net net;
    Node a, b;
    a.id = 100;
    b.id = 200;  // 300 is a member without Melange: no advert, never anything sent
    b.skew = 1234.5;
    for (Node* n : {&a, &b}) {
        n->io.net = &net;
        n->io.self = n;
        net.nodes.push_back(n);
    }
    std::string why;
    const ChannelId ca = a.core.Open("holo", 1, &why);
    const ChannelId cb = b.core.Open("holo", 1, &why);
    Expect(ca && cb, "channels open");
    Expect(!a.core.Open("holo", 2, &why) && why == "already-open", "one channel per mod");
    Expect(!a.core.Open("Bad Id", 1, &why) && why == "bad-id", "a bad mod id");
    Expect(!a.core.Open("x", 0, &why) && why == "bad-version", "version 0");
    Expect(a.core.Advert() == "1;holo@1", "advert");
    const std::vector<uint8_t> hi = Enc(Tab({{Str("t"), Str("hi")}}));

    // Not in a lobby yet.
    Expect(a.core.Send(ca, 200, hi, true, 0) == SendStatus::Offline, "offline before the lobby");
    a.core.SetLobby(LobbyFor(1, 100, 100, {{200, "1;holo@1"}, {300, ""}}), net.t);
    b.core.SetLobby(LobbyFor(1, 100, 200, {{100, "1;holo@1"}, {300, ""}}), net.t + b.skew);

    // No session yet: never the first packet.
    net.Run(3000);
    Expect(a.sent == 0 && b.sent == 0, "no P2P session: not a single packet, not even a PING");
    Expect(a.core.Send(ca, 200, hi, true, net.t) == SendStatus::NoSession, "send before the session: no-session");
    Expect(a.core.Peers(ca).empty() && a.changes.empty(), "nobody reachable");
    Expect(a.core.Send(ca, 300, hi, true, net.t) == SendStatus::NoPeer, "a member without mlg.peer: no-peer");
    Expect(a.core.Send(ca, 999, hi, true, net.t) == SendStatus::NoPeer, "not a member: no-peer");
    Expect(a.core.Send(ca, 100, hi, true, net.t) == SendStatus::NoPeer, "myself: no-peer");
    a.session[300] = true;  // even with a session, a member that does not advertise the channel gets nothing
    net.Run(1000);
    Expect(a.sentTo[300] == 0 && a.sent == 0, "a session without mlg.peer: nothing sent");

    // The game's own connection comes up.
    a.session[200] = b.session[100] = true;
    net.Run(100);
    Expect(a.changes.size() == 1 && a.changes[0].up && a.changes[0].id == 200, "onPeer(up) when the session exists");
    Expect(a.core.Peers(ca) == std::vector<uint64_t>{200}, "peers()");
    net.Run(6000);
    ClockView cv;
    Expect(a.core.Clock(200, &cv) && std::fabs(cv.offsetMs - 1234.5) < 20 && cv.rttMs > 30 && cv.rttMs < 80,
           "A's clock of B: offset " + std::to_string(cv.offsetMs) + " rtt " + std::to_string(cv.rttMs));
    Expect(b.core.Clock(100, &cv) && std::fabs(cv.offsetMs + 1234.5) < 20, "B's clock of A: " + std::to_string(cv.offsetMs));
    const int pingsFast = cv.samples;
    net.Run(9000);
    b.core.Clock(100, &cv);
    Expect(pingsFast >= 5 && cv.samples - pingsFast >= 2 && cv.samples - pingsFast <= 4,
           "1 s pings first, then 3 s: " + std::to_string(pingsFast) + " then +" + std::to_string(cv.samples - pingsFast));

    // Messages: reliable 8 KB in fragments, and unreliable.
    w::Value bigTab = Tab();
    for (int i = 0; i < 8; ++i) bigTab.t.emplace_back(Int(i), Str(std::string(1000, static_cast<char>('a' + i))));
    const std::vector<uint8_t> big = Enc(bigTab);
    Expect(big.size() > 8000 && big.size() <= 8192, "an ~8 KB value");
    Expect(a.core.Send(ca, 200, big, true, net.t) == SendStatus::Ok, "send 8 KB");
    Expect(a.core.Send(ca, 200, hi, false, net.t) == SendStatus::Ok, "send unreliable");
    net.Run(100);
    Expect(b.got.size() == 2 && b.got[0].payload == big && b.got[0].reliable && b.got[0].from == 100 && b.got[1].payload == hi &&
               !b.got[1].reliable,
           "B receives both, in order, with meta");
    Expect(b.got.size() == 2 && b.got[0].at >= net.t - 100 + b.skew, "receive time is B's clock");
    Expect(a.core.Send(ca, 200, Enc(Str(std::string(1000, 'x'))), false, net.t) == SendStatus::Size, "unreliable over 900: size");
    Expect(a.core.Send(ca, 200, std::vector<uint8_t>(8193, 0), true, net.t) == SendStatus::Size, "over 8 KB: size");

    // Rate: burst 10 then refuse.
    net.Run(2000);
    int ok = 0, rate = 0;
    for (int i = 0; i < 15; ++i) {
        const SendStatus s = a.core.Send(ca, 200, hi, true, net.t);
        ok += s == SendStatus::Ok;
        rate += s == SendStatus::Rate;
    }
    Expect(ok == 10 && rate == 5, "rate: 10 then \"rate\" (" + std::to_string(ok) + "/" + std::to_string(rate) + ")");
    Expect(a.core.Broadcast(ca, hi, true, net.t) == 0, "broadcast under the same limit");
    net.Run(1000);
    Expect(a.core.Broadcast(ca, hi, true, net.t) == 1, "broadcast reaches B only");

    // A flood from B is cut at the receive budget.
    net.Run(2000);
    a.got.clear();
    const uint64_t dropBefore = a.core.Stats().dropRate;
    for (uint16_t i = 0; i < 100; ++i) {
        auto f = w::EncodeMessage(b.core.Key(), "holo", static_cast<uint16_t>(1000 + i), false, hi);
        a.core.OnPacket(200, f[0].data(), f[0].size(), net.t);
    }
    Expect(a.core.Stats().dropRate - dropBefore >= 70, "a flood is dropped past the receive budget");
    a.core.TakeDeliveries();

    // Dropped before delivery: a non-member (before decode), another lobby key, a mod with no channel, a bad value.
    const uint64_t nonMember = a.core.Stats().dropNonMember;
    auto f = w::EncodeMessage(a.core.Key(), "holo", 1, true, hi);
    a.core.OnPacket(777, f[0].data(), f[0].size(), net.t);
    Expect(a.core.Stats().dropNonMember == nonMember + 1, "a non-member's packet");
    auto stale = w::EncodeMessage(a.core.Key() ^ 1, "holo", 1, true, hi);
    const uint64_t staleBefore = a.core.Stats().dropStaleKey;
    a.core.OnPacket(200, stale[0].data(), stale[0].size(), net.t);
    Expect(a.core.Stats().dropStaleKey == staleBefore + 1, "another lobby key");
    auto otherMod = w::EncodeMessage(a.core.Key(), "kanly", 1, true, hi);
    const uint64_t noChan = a.core.Stats().dropNoChannel;
    a.core.OnPacket(200, otherMod[0].data(), otherMod[0].size(), net.t);
    Expect(a.core.Stats().dropNoChannel == noChan + 1, "a message for a mod with no channel here");
    auto junk = Enc(Str("x"));
    junk.push_back(9);
    auto badValue = w::EncodeMessage(a.core.Key(), "holo", 2, true, junk);
    const uint64_t valueBefore = a.core.Stats().dropValue;
    a.core.OnPacket(200, badValue[0].data(), badValue[0].size(), net.t + 2000);
    Expect(a.core.Stats().dropValue == valueBefore + 1 && a.core.TakeDeliveries().empty(), "a payload that is not one value");
    net.Run(2100);

    // Version mismatch.
    b.core.SetLobby(LobbyFor(1, 100, 200, {{100, "1;holo@2"}, {300, ""}}), net.t + b.skew);
    net.Run(100);
    Expect(b.core.Send(cb, 100, hi, true, net.t + b.skew) == SendStatus::NoPeer && b.core.Peers(cb).empty(),
           "another channel version: no-peer");
    b.core.SetLobby(LobbyFor(1, 100, 200, {{100, "1;holo@1"}, {300, ""}}), net.t + b.skew);
    net.Run(100);
    Expect(b.core.Peers(cb).size() == 1, "same version again: reachable");

    // A stall (a level load): the samples are discarded and measured again.
    net.Step(2000);
    Expect(!a.core.Clock(200, &cv) && a.core.Stats().stalls == 1, "a 2 s frame drops the clock samples");
    net.Run(1500);
    Expect(a.core.Clock(200, &cv) && std::fabs(cv.offsetMs - 1234.5) < 20, "and measures again right away");

    // The lobby changes hands: the key changes, everyone goes down, then comes back under the new key.
    a.changes.clear();
    a.core.SetLobby(LobbyFor(1, 200, 100, {{200, "1;holo@1"}, {300, ""}}), net.t);
    net.Step(16);
    Expect(WentDown(a.changes, 200) && a.core.Stats().resets >= 1, "owner change: onPeer(down) and a reset");
    const uint64_t staleNow = a.core.Stats().dropStaleKey;
    Expect(b.core.Send(cb, 100, hi, true, net.t + b.skew) == SendStatus::Ok, "B still on the old key");
    net.Run(100);
    Expect(a.core.Stats().dropStaleKey > staleNow, "A drops B's old-key traffic");
    b.core.SetLobby(LobbyFor(1, 200, 200, {{100, "1;holo@1"}, {300, ""}}), net.t + b.skew);
    net.Run(200);
    a.got.clear();
    Expect(b.core.Send(cb, 100, hi, true, net.t + b.skew) == SendStatus::Ok, "B on the new key");
    net.Run(100);
    Expect(a.got.size() == 1, "and A takes it");

    // BYE: B closes; A stops sending to it before B's mlg.peer update even arrives.
    a.changes.clear();
    b.core.Close(cb);
    net.Run(100);
    Expect(a.core.Send(ca, 200, hi, true, net.t) == SendStatus::NoPeer && a.core.Peers(ca).empty(), "after BYE: no-peer");
    Expect(WentDown(a.changes, 200), "after BYE: onPeer(down)");
    const ChannelId cb2 = b.core.Open("holo", 1, &why);
    net.Run(200);
    Expect(cb2 && a.core.Peers(ca).size() == 1, "B reopens: its HELLO makes it reachable again");
    a.got.clear();
    Expect(b.core.Send(cb2, 100, big, true, net.t + b.skew) == SendStatus::Ok, "B sends 8 KB on the new channel");
    net.Run(100);
    Expect(a.got.size() == 1 && a.got[0].payload == big, "a reopened channel's fragments are not taken for old duplicates");

    // The session drops (the game's connection went away): no-session, onPeer(down).
    a.changes.clear();
    a.session[200] = false;
    net.Run(50);
    Expect(a.core.Send(ca, 200, hi, true, net.t) == SendStatus::NoSession && a.core.Peers(ca).empty() && WentDown(a.changes, 200),
           "session lost");
    // Leaving the lobby resets everything.
    a.core.SetLobby(LobbyInfo{}, net.t);
    Expect(!a.core.Online() && a.core.Send(ca, 200, hi, true, net.t) == SendStatus::Offline, "left the lobby: offline");
    a.core.Close(ca);
    Expect(a.core.Send(ca, 200, hi, true, net.t) == SendStatus::Closed && a.core.Advert().empty(), "closed");
    Expect(a.sentTo[300] == 0 && b.sentTo[300] == 0, "the member without Melange never got a packet");
}

// Loss on unreliable traffic: the clock still converges (reliable traffic is never lost by Steam).
void TestLossyClock() {
    Net net;
    net.loss = 0.4;
    net.latency = 35;
    Node a, b;
    a.id = 1;
    b.id = 2;
    b.skew = -50000;
    a.session[2] = b.session[1] = true;
    for (Node* n : {&a, &b}) {
        n->io.net = &net;
        n->io.self = n;
        net.nodes.push_back(n);
    }
    std::string why;
    a.core.Open("holo", 1, &why);
    b.core.Open("holo", 1, &why);
    a.core.SetLobby(LobbyFor(5, 1, 1, {{2, "1;holo@1"}}), net.t);
    b.core.SetLobby(LobbyFor(5, 1, 2, {{1, "1;holo@1"}}), net.t + b.skew);
    net.Run(20000);
    ClockView cv;
    Expect(a.core.Clock(2, &cv) && std::fabs(cv.offsetMs + 50000) < 20, "40% loss: offset " + std::to_string(cv.offsetMs));
}

void TestFuzz() {
    std::mt19937 rng(1234);
    w::Header h;
    h.kind = w::Kind::Msg;
    h.reliable = true;
    h.lobbyKey = 99;
    h.modId = "holo";
    h.nfrag = 1;
    const std::vector<uint8_t> payload =
        Enc(Tab({{Str("a"), Tab({{Int(1), Num(2.5)}, {Int(2), Str("xyz")}})}, {Str("b"), Int(-7)}}));
    std::vector<std::vector<uint8_t>> seeds = {w::Encode(h, payload.data(), payload.size())};
    for (const auto& f : w::EncodeMessage(99, "holo", 4, true, std::vector<uint8_t>(3000, 0x06))) seeds.push_back(f);
    w::Header ctl;
    ctl.kind = w::Kind::Pong;
    ctl.lobbyKey = 99;
    const std::vector<uint8_t> pong = w::PongBody(1, 2, 3);
    seeds.push_back(w::Encode(ctl, pong.data(), pong.size()));

    Net net;
    Node a;
    a.id = 1;
    a.io.net = &net;
    a.io.self = &a;
    a.session[2] = true;
    std::string why;
    a.core.Open("holo", 1, &why);
    LobbyInfo l = LobbyFor(5, 1, 1, {{2, "1;holo@1"}});
    l.key = 99;
    a.core.SetLobby(l, 0);
    a.core.Tick(0);

    int decoded = 0;
    for (int i = 0; i < 20000; ++i) {
        std::vector<uint8_t> p = seeds[rng() % seeds.size()];
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int e = 0; e < edits && !p.empty(); ++e) {
            switch (rng() % 4) {
                case 0: p[rng() % p.size()] = static_cast<uint8_t>(rng()); break;
                case 1: p.resize(rng() % (p.size() + 1)); break;
                case 2: p.insert(p.begin() + static_cast<std::ptrdiff_t>(rng() % (p.size() + 1)), static_cast<uint8_t>(rng())); break;
                default: p[rng() % p.size()] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
            }
        }
        w::Packet out;
        if (!p.empty() && w::Decode(p.data(), p.size(), &out) == w::DecodeResult::Ok) {
            ++decoded;
            w::Value v;
            w::DecodeValue(out.body.data(), out.body.size(), &v);
        }
        a.core.OnPacket(2, p.data(), p.size(), static_cast<double>(i));
        if (i % 64 == 0) a.core.Tick(static_cast<double>(i));
        std::vector<uint8_t> v(rng() % 64);
        for (auto& x : v) x = static_cast<uint8_t>(rng());
        w::Value val;
        w::DecodeValue(v.data(), v.size(), &val);
    }
    bool allDecode = true;
    for (const Delivery& d : a.core.TakeDeliveries()) {
        w::Value v;
        allDecode = allDecode && w::DecodeValue(d.payload.data(), d.payload.size(), &v);
    }
    Expect(allDecode, "fuzz: every delivered payload decodes");
    Expect(decoded > 0, "fuzz: some mutations still decode (" + std::to_string(decoded) + ")");
    const Counters& c = a.core.Stats();
    Expect(c.dropMalformed + c.dropNotOurs + c.dropBadProtocol + c.dropStaleKey > 0, "fuzz: malformed packets are counted");
}
}  // namespace

int main() {
    TestHeader();
    TestValues();
    TestFragments();
    TestRates();
    TestClock();
    TestAdvertAndGates();
    TestCore();
    TestLossyClock();
    TestFuzz();
    printf("peerbus_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
