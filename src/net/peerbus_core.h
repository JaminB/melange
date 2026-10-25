#pragma once
// The peer bus's state machine without Steam (Melange 0.12.0): channels, members, gates, rate limits, reassembly,
// HELLO/BYE and the PING/PONG clock. peerbus.cpp feeds it the lobby, the drained packets and the frame clock, and gives
// it an Io that sends and asks Steam for session state; peerbus_selftest wires two of them together.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "net/peerbus_wire.h"

namespace melange::peerbus {
using ChannelId = uint32_t;
using wire::SendStatus;

struct MemberInfo {
    uint64_t id = 0;
    std::string advert;  // its raw mlg.peer value
};
struct LobbyInfo {
    uint64_t lobby = 0, me = 0, owner = 0;
    uint64_t key = 0;                  // wormsign::wire::MatchKey(owner, lobby); 0 outside a lobby
    std::vector<MemberInfo> members;   // every member other than me
};

struct Delivery {
    ChannelId ch = 0;
    uint64_t from = 0;
    std::vector<uint8_t> payload;  // a wire::Value that DecodeValue accepted
    bool reliable = false;
    double at = 0;                 // drain time, NowMs()
};
struct PeerChange {
    ChannelId ch = 0;
    uint64_t id = 0;
    bool up = false;
};

class Io {
  public:
    virtual ~Io() = default;
    virtual bool SessionActive(uint64_t member) = 0;  // GetP2PSessionState(member).connectionActive
    // reliable: Steam k_EP2PSendReliable; else noDelay ? UnreliableNoDelay : Unreliable. Channel 6.
    virtual bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable, bool noDelay) = 0;
};

struct ChannelStats {
    std::string modId;
    int version = 0;
    uint64_t sent = 0, received = 0, refused = 0, dropped = 0;
};
struct Counters {
    uint64_t packetsIn = 0, bytesIn = 0, packetsOut = 0, bytesOut = 0, sendFailed = 0;
    uint64_t dropOffline = 0, dropNonMember = 0, dropNotOurs = 0, dropBadProtocol = 0, dropMalformed = 0;
    uint64_t dropStaleKey = 0, dropNoChannel = 0, dropNoAdvert = 0, dropRate = 0, dropReassembly = 0, dropValue = 0;
    uint64_t resets = 0, stalls = 0;
};
struct ClockView {
    double offsetMs = 0, rttMs = 0;
    int samples = 0;
};

class Core {
  public:
    explicit Core(Io* io) : io_(io) {}

    // The lobby as polled (main thread, every few hundred ms and on lobby callbacks). A new key (another lobby, or
    // the same lobby under a new owner) or leaving resets every member's state first.
    void SetLobby(const LobbyInfo& l, double nowMs);
    bool Online() const { return lobby_.lobby != 0 && lobby_.key != 0; }
    uint64_t Key() const { return lobby_.key; }

    // "already-open" (one channel per mod), "bad-id", "bad-version" (1..65535).
    ChannelId Open(const std::string& modId, int version, std::string* why);
    void Close(ChannelId ch);  // says BYE to the members it reached
    bool IsOpen(ChannelId ch) const { return channels_.count(ch) != 0; }
    std::string Advert() const;  // our mlg.peer value; "" with no channel open
    const std::string* ModOf(ChannelId ch) const;

    SendStatus Send(ChannelId ch, uint64_t to, const std::vector<uint8_t>& payload, bool reliable, double nowMs);
    int Broadcast(ChannelId ch, const std::vector<uint8_t>& payload, bool reliable, double nowMs);
    std::vector<uint64_t> Peers(ChannelId ch) const;  // reachable at the last Tick
    bool Clock(uint64_t id, ClockView* out) const;

    // One drained packet; `at` is the drain time.
    void OnPacket(uint64_t from, const uint8_t* p, size_t n, double at);
    // Once per frame, after the drain: stall check, session state, reachability (HELLO, onPeer), PINGs, expiry.
    void Tick(double nowMs);

    std::vector<Delivery> TakeDeliveries();
    std::vector<PeerChange> TakePeerChanges();
    const Counters& Stats() const { return c_; }
    std::vector<ChannelStats> Channels() const;
    std::vector<uint64_t> ClockMembers() const;  // members with a clock (any sample), for the panel

  private:
    struct Channel {
        std::string modId;
        int version = 0;
        ChannelStats stats;
        std::set<uint64_t> reachable;           // as of the last Tick
        std::map<uint64_t, wire::SendLimiter> send;
        std::map<uint64_t, wire::TokenBucket> recvMsgs, recvBytes;
        std::map<uint64_t, std::string> byeAdvert;  // a member said BYE while advertising this value
    };
    struct Member {
        std::string advert;
        bool session = false;
        bool up = false;            // reachable on at least one channel
        double upSince = 0;         // since (or since the last stall)
        double nextPing = 0;
        wire::ClockEstimator clock;
        wire::TokenBucket control{wire::kRecvControlPerSec, wire::kRecvControlBurst};
    };
    bool IsMember(uint64_t id) const { return members_.count(id) != 0; }
    bool Advertises(const Channel& c, uint64_t id) const;
    bool Reachable(const Channel& c, uint64_t id) const;
    bool AnyCommon(uint64_t id) const;  // reachable on at least one channel now
    bool SendControl(uint64_t to, wire::Kind k, const std::string& modId, const std::vector<uint8_t>& body, bool reliable);
    void Reset();  // lobby left or key changed: every member down

    Io* io_;
    LobbyInfo lobby_;
    std::map<uint64_t, Member> members_;
    std::map<ChannelId, Channel> channels_;
    // Next seq per (mod, destination). Kept across close, reopen and resets, so a receiver that still remembers a
    // completed (sender, mod, seq) never mistakes a new message for its duplicate.
    std::map<std::pair<std::string, uint64_t>, uint16_t> seq_;
    ChannelId nextId_ = 1;
    wire::Reassembler reasm_;
    wire::GlobalBudget global_;
    bool ticked_ = false, stalled_ = false;
    double lastTick_ = 0, stallAt_ = 0, creditedFor_ = -1;
    static constexpr double kStallCreditMs = 15000;
    void CreditStall(double at);
    std::vector<Delivery> deliveries_;
    std::vector<PeerChange> changes_;
    Counters c_;
};
}  // namespace melange::peerbus
