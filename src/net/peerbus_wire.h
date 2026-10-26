#pragma once
// The peer bus's packets and the pure pieces around them (Melange 0.12.0): the wire header, the tagged binary value a
// mod sends, fragmentation and bounded reassembly, the token buckets, the NTP clock estimate, the mlg.peer advert and
// the send gates. No Steam and no game here, so peerbus_selftest and sandbox_selftest link it as is.
//
// Packet (little-endian, at most kMaxPacket bytes):
//   "MPB1" | u8 kind (bit 7: sent reliably) | u8 protocol | u16 total bytes | u64 lobbyKey | u8 modIdLen | modId
//   | u16 seq | u8 frag | u8 nfrag | body
// lobbyKey = wormsign::wire::MatchKey(owner, lobby). PING, PONG carry no mod id; HELLO, MSG and BYE carry the channel's.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace melange::peerbus::wire {
constexpr uint32_t kMagic = 0x3142504D;  // "MPB1"
constexpr uint8_t kProtocol = 1;
constexpr int kChannel = 6;              // the game uses 0, Wormsign 5, LocalNet's self-test 7
constexpr size_t kMaxPacket = 1100;
constexpr size_t kFixedHeader = 21;      // everything but the mod id
constexpr size_t kMaxModId = 64;         // spice.json ids are at most 64 characters
constexpr size_t kMaxUnreliable = 900;   // one packet
constexpr size_t kMaxReliable = 8192;    // fragmented, see MaxMessage()
constexpr uint8_t kMaxFrags = 8;
constexpr size_t kMaxString = 1024;
constexpr int kMaxDepth = 6;             // tables inside tables
constexpr size_t kMaxKeys = 256;         // per table
constexpr const char* kAdvertKey = "mlg.peer";  // lobby member key: "1;<modId>@<version>,..."
constexpr const char* kPresencePrefix = "mlg.p.";  // lobby member key per mod: its presence JSON
constexpr size_t kMaxPresence = 256;
constexpr size_t kMaxAdvertEntries = 32;

enum class Kind : uint8_t { Hello = 1, Msg, Ping, Pong, Bye };
constexpr uint8_t kReliableBit = 0x80;

struct Header {
    Kind kind = Kind::Msg;
    bool reliable = false;
    uint8_t protocol = kProtocol;
    uint64_t lobbyKey = 0;
    std::string modId;  // empty for PING / PONG
    uint16_t seq = 0;
    uint8_t frag = 0, nfrag = 1;
};
struct Packet {
    Header h;
    std::vector<uint8_t> body;
};

constexpr size_t HeaderBytes(size_t modIdLen) { return kFixedHeader + modIdLen; }
constexpr size_t ChunkBytes(size_t modIdLen) { return kMaxPacket - HeaderBytes(modIdLen); }
// The largest payload one message of this mod may carry: 900 B unreliable, 8 KB reliable (a little less for mod ids
// over 41 characters, whose 8 fragments hold 8 * ChunkBytes).
size_t MaxMessage(size_t modIdLen, bool reliable);
bool ValidModId(std::string_view id);  // [a-z0-9_-], 1..64

// Empty when the packet would exceed kMaxPacket or the mod id is invalid.
std::vector<uint8_t> Encode(const Header& h, const uint8_t* body, size_t n);
// MSG fragments for one payload (nfrag 1 when it fits one packet); empty when it is over MaxMessage.
std::vector<std::vector<uint8_t>> EncodeMessage(uint64_t lobbyKey, const std::string& modId, uint16_t seq, bool reliable,
                                                const std::vector<uint8_t>& payload);

enum class DecodeResult : uint8_t { Ok, NotOurs, BadProtocol, Malformed };
// NotOurs: too short, too long, no magic or a length field that disagrees. BadProtocol: our magic, another protocol.
// Malformed: anything else that breaks the rules (kind, mod id, fragment fields, control body sizes).
DecodeResult Decode(const uint8_t* p, size_t n, Packet* out);

// Control bodies.
std::vector<uint8_t> PingBody(double t0);
std::vector<uint8_t> PongBody(double t0, double t1, double t2);
bool ParsePing(const std::vector<uint8_t>& b, double* t0);
bool ParsePong(const std::vector<uint8_t>& b, double* t0, double* t1, double* t2);
std::vector<uint8_t> HelloBody(uint16_t version);
bool ParseHello(const std::vector<uint8_t>& b, uint16_t* version);

// ---------------------------------------------------------------- the value a mod sends (a Lua value)
struct Value {
    enum class Type : uint8_t { Nil, Bool, Int, Num, Str, Table };
    Type type = Type::Nil;
    bool b = false;
    int64_t i = 0;
    double n = 0;
    std::string s;
    std::vector<std::pair<Value, Value>> t;  // key, value; keys are bool, number or string, never nil or NaN
};
// Tags: 0 nil, 1 false, 2 true, 3 integer (zigzag varint), 4 f64, 5 string (varint length), 6 table (varint count).
// False (and *why = "size") when a limit is broken: string length, depth, keys per table, or a bad key.
bool EncodeValue(const Value& v, std::vector<uint8_t>* out, std::string* why);
// Untrusted input: every limit is checked, and the whole buffer must be one value.
bool DecodeValue(const uint8_t* p, size_t n, Value* out);

// ---------------------------------------------------------------- reassembly (bounded, N2)
class Reassembler {
  public:
    static constexpr size_t kMaxPartialPerSender = 2;
    static constexpr size_t kMaxPartialBytes = 64 * 1024;
    static constexpr double kTimeoutMs = 5000;

    enum class Result : uint8_t { Pending, Complete, Dropped };
    // `chunkMax` is ChunkBytes(modId.size()): every fragment but the last must be exactly that long.
    Result Add(uint64_t from, const Header& h, const std::vector<uint8_t>& body, size_t chunkMax, double nowMs,
               std::vector<uint8_t>* message);
    void Expire(double nowMs);           // drops partial messages older than kTimeoutMs
    // True when an earlier fragment of this message started a partial (its later fragments ride on that admission).
    bool InProgress(uint64_t from, const Header& h) const;
    // A fragment of this message was refused (rate): drop the partial and ignore the rest of it, so a lost message
    // never holds one of the sender's kMaxPartialPerSender slots until kTimeoutMs.
    void Abandon(uint64_t from, const Header& h, double nowMs);
    void DropSender(uint64_t from);
    void Clear();
    size_t PartialBytes() const { return bytes_; }
    size_t Partials() const { return parts_.size(); }
    uint64_t Dropped() const { return dropped_; }

  private:
    struct Key {
        uint64_t from;
        std::string mod;
        uint16_t seq;
        bool operator<(const Key& o) const {
            if (from != o.from) return from < o.from;
            if (seq != o.seq) return seq < o.seq;
            return mod < o.mod;
        }
    };
    struct Part {
        double started = 0;
        uint8_t nfrag = 0, got = 0;
        std::vector<std::vector<uint8_t>> frags;
        std::vector<bool> have;
        size_t bytes = 0;
    };
    static constexpr size_t kMaxDone = 256;
    void Erase(std::map<Key, Part>::iterator it);
    void MarkDone(const Key& key, double nowMs);
    std::map<Key, Part> parts_;
    std::map<Key, double> done_;  // recently completed messages: a late duplicate fragment never starts a new partial
    size_t bytes_ = 0;
    uint64_t dropped_ = 0;
};

// ---------------------------------------------------------------- rate limits
class TokenBucket {
  public:
    TokenBucket() = default;
    TokenBucket(double perSec, double cap) : rate_(perSec), cap_(cap), tokens_(cap) {}
    void Configure(double perSec, double cap);
    bool Can(double n, double nowMs);
    void Take(double n, double nowMs);   // may go below zero only when the caller skipped Can
    void Credit(double upTo, double nowMs);  // tokens at least `upTo`, above the cap until spent (a stall's backlog)
    double Tokens(double nowMs);

  private:
    void Refill(double nowMs);
    double rate_ = 0, cap_ = 0, tokens_ = 0, last_ = 0;
    bool started_ = false;
};

// Send: 20 messages a second (burst 10) and 8 KB a second per mod per peer. The 32 KB/s channel budget is shared
// fair-share per destination: each of N reachable destinations gets 32/N KB/s (and the total bucket caps the sum).
constexpr double kSendMsgsPerSec = 20, kSendMsgBurst = 10;
constexpr double kSendBytesPerSec = 8192, kSendByteBurst = 8192;
constexpr double kGlobalBytesPerSec = 32768;
constexpr double kDestMinBurst = 9216;  // a full 8 KB reliable message with its headers always fits one destination
// Receive: twice the send budget per sender and mod; control packets (PING, PONG, HELLO, BYE) 10 a second per sender.
constexpr double kRecvMsgsPerSec = 40, kRecvMsgBurst = 20;
constexpr double kRecvBytesPerSec = 16384, kRecvByteBurst = 16384 + 2048;
constexpr double kRecvControlPerSec = 10, kRecvControlBurst = 10;
constexpr int kMaxPacketsPerFrame = 64;

class SendLimiter {  // one per (channel, destination)
  public:
    SendLimiter() : msgs_(kSendMsgsPerSec, kSendMsgBurst), bytes_(kSendBytesPerSec, kSendByteBurst) {}
    bool Can(size_t payload, double nowMs) { return msgs_.Can(1, nowMs) && bytes_.Can(static_cast<double>(payload), nowMs); }
    void Take(size_t payload, double nowMs) {
        msgs_.Take(1, nowMs);
        bytes_.Take(static_cast<double>(payload), nowMs);
    }

  private:
    TokenBucket msgs_, bytes_;
};

class GlobalBudget {
  public:
    GlobalBudget() : total_(kGlobalBytesPerSec, kGlobalBytesPerSec) {}
    void SetDestinations(size_t n);      // reachable destinations now (>= 1 is assumed)
    bool Can(uint64_t dest, size_t wireBytes, double nowMs);
    void Take(uint64_t dest, size_t wireBytes, double nowMs);
    void Forget(uint64_t dest) { per_.erase(dest); }
    void Clear() { per_.clear(); }

  private:
    TokenBucket& For(uint64_t dest);
    TokenBucket total_;
    std::map<uint64_t, TokenBucket> per_;
    size_t n_ = 1;
};

// ---------------------------------------------------------------- clock (NTP over PING/PONG)
struct ClockSample {
    double rtt = 0, offset = 0;
};
// rtt = (t3 - t0) - (t2 - t1); offset = ((t1 - t0) + (t2 - t3)) / 2 (their clock - mine). The offset is the one of
// the minimum-RTT sample of the last 8; the reported RTT is an EWMA (weight 0.85 on the old value) of every sample.
bool NtpSample(double t0, double t1, double t2, double t3, ClockSample* out);  // false when the times are impossible
class ClockEstimator {
  public:
    static constexpr int kWindow = 8;
    static constexpr double kEwma = 0.85;
    static constexpr double kMaxRttMs = 10000;
    bool Add(double t0, double t1, double t2, double t3);
    bool Ready() const { return count_ > 0; }
    double Offset() const;
    double Rtt() const { return rtt_; }
    int Samples() const { return total_; }
    void Reset();

  private:
    ClockSample ring_[kWindow];
    int count_ = 0, next_ = 0, total_ = 0;
    double rtt_ = 0;
};
// PING every 1 s for the first 5 s a peer is reachable (or after a stall), then every 3 s.
constexpr double kPingFastMs = 1000, kPingSlowMs = 3000, kPingFastWindowMs = 5000;
constexpr double kStallMs = 500;  // a gap between frames longer than this (a level load) discards the samples
inline double PingInterval(double nowMs, double upSinceMs) {
    return nowMs - upSinceMs < kPingFastWindowMs ? kPingFastMs : kPingSlowMs;
}

// ---------------------------------------------------------------- mlg.peer advert
struct AdvertEntry {
    std::string modId;
    int version = 0;
    bool operator<(const AdvertEntry& o) const { return modId != o.modId ? modId < o.modId : version < o.version; }
    bool operator==(const AdvertEntry& o) const { return modId == o.modId && version == o.version; }
};
std::string BuildAdvert(std::vector<AdvertEntry> entries);  // "" when empty, else "1;a@1,b@2" (sorted)
bool ParseAdvert(std::string_view value, std::vector<AdvertEntry>* out);  // tolerant: bad entries are skipped
bool Advertises(std::string_view value, std::string_view modId, int version);  // version < 0: any version

// ---------------------------------------------------------------- send gates
enum class SendStatus : uint8_t { Ok, Size, Rate, NoPeer, NoSession, Offline, Closed };
inline const char* StatusName(SendStatus s) {
    switch (s) {
        case SendStatus::Ok: return "ok";
        case SendStatus::Size: return "size";
        case SendStatus::Rate: return "rate";
        case SendStatus::NoPeer: return "no-peer";
        case SendStatus::NoSession: return "no-session";
        case SendStatus::Offline: return "offline";
        case SendStatus::Closed: return "closed";
    }
    return "?";
}
struct GateIn {
    bool channelOpen = true;
    bool online = false;       // in a lobby, with a lobbyKey
    bool member = false;       // a current lobby member other than me
    bool advertises = false;   // its mlg.peer lists <modId>@<version> (and it has not said BYE since)
    bool session = false;      // GetP2PSessionState(member).connectionActive
};
// The order of the checks is the order of the reasons: closed, offline, no-peer, no-session. A packet is sent only on
// Ok, so Melange is never the first traffic between two machines and never reaches a member that does not drain ch6.
SendStatus Gate(const GateIn& g);
}  // namespace melange::peerbus::wire
