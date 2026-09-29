#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "melange/wormsign.h"
#include "wormsign/exchange_wire.h"

// The desync detector without Steam or the game: the per-peer tick comparison, the exchange protocol state
// machine and the field diff. detector.cpp connects it to the lobby, P2P channel 5 and the tick clock.
namespace melange::wormsign::detect {
const char* CompName(int i);                        // engine component names as reports show them
std::string CompList(uint8_t mask);                  // "worms, time+rng"

// Compares our tick hashes with one peer's by tick number, whichever side has a tick first.
class PeerCompare {
  public:
    static constexpr uint32_t kSlots = 4096;       // ticks kept per side (about 80 s)
    static constexpr uint32_t kMaxSkew = 3000;     // their ticks further than this from ours are dropped

    void Reset();
    void SetCompareMods(bool on) { compareMods_ = on; }
    void AddOurs(uint32_t tick, uint64_t engine, uint64_t mods);
    bool AddTheirs(uint32_t tick, uint64_t engine, uint64_t mods);  // false: dropped as too far from our ticks

    bool Diverged() const { return diverged_; }
    uint32_t DivergedTick() const { return divTick_; }
    bool TakeNewDivergence();                       // true once, when the first mismatch was found
    bool Ours(uint32_t tick, uint64_t* engine, uint64_t* mods) const;
    bool Theirs(uint32_t tick, uint64_t* engine, uint64_t* mods) const;

    uint32_t Compared() const { return compared_; }
    uint32_t Matched() const { return matched_; }
    uint32_t MismatchedAfter() const { return mismatchedAfter_; }  // compared ticks after the first that differ
    uint32_t LastCommon() const { return lastCommon_; }
    uint32_t OurLatest() const { return ourLatest_; }
    uint32_t TheirLatest() const { return theirLatest_; }
    uint32_t Dropped() const { return dropped_; }

  private:
    struct Slot {
        uint32_t tick = 0;
        uint64_t engine = 0, mods = 0;
        bool have = false, done = false;
    };
    void Try(uint32_t tick);
    Slot ours_[kSlots], theirs_[kSlots];
    bool compareMods_ = true, diverged_ = false, fresh_ = false, anyOurs_ = false;
    uint32_t divTick_ = 0, compared_ = 0, matched_ = 0, mismatchedAfter_ = 0, lastCommon_ = 0;
    uint32_t ourLatest_ = 0, theirLatest_ = 0, dropped_ = 0;
};

enum class PeerState : uint8_t { NoExchange, VersionMismatch, Waiting, Exchanging, OtherMatch, Diverged };
const char* PeerStateName(PeerState s);
struct PeerStatus {
    uint64_t steamId;
    char name[64];
    PeerState state;
    bool modsCompared;           // contributor lists match
    uint32_t compared, lastCommonTick, theirTick, apartTicks, divergedTick;
    int32_t lagTicks;            // our latest tick minus theirs
    char melange[24], content[17];
};
struct PeerInfo {
    uint64_t id = 0;
    std::string name, advert;    // advert: the member's lobby key value, "" when it has none
};

// What a bundle needs from one divergence.
struct Report {
    Divergence div{};
    uint64_t ours[kEngineComps] = {}, theirs[kEngineComps] = {};
    bool haveOurs = false, haveTheirs = false, contribListsMatch = true;
    std::vector<wire::ContribName> contribNames;
    std::vector<uint64_t> oursContrib, theirsContrib;
    std::string detailLocal, detailPeer, peerName;
};

class Env {
  public:
    virtual uint64_t NowMs() = 0;
    virtual bool Send(uint64_t to, const std::vector<uint8_t>& packet, bool reliable) = 0;
    virtual bool OurTick(uint32_t tick, TickHash* out) = 0;
    virtual std::string Detail(uint32_t tick) = 0;           // "" when unknown
    virtual bool ContribHashes(uint32_t tick, std::vector<uint64_t>* out) = 0;
    virtual void Diverged(const Divergence& d) = 0;          // raise it
    virtual void Bundle(const Report& r) = 0;                // write it
    virtual void Note(bool warn, const std::string& text) = 0;

  protected:
    ~Env() = default;
};

class Detector {
  public:
    static constexpr uint32_t kBatchEvery = 25;       // ticks between HASHES
    static constexpr uint32_t kBatchTicks = 40;       // ticks per HASHES: each tick goes out in two batches
    static constexpr uint64_t kHelloEarlyMs = 1000, kHelloLateMs = 10000;
    static constexpr uint64_t kCompsWaitMs = 400, kDetailWaitMs = 3000;
    static constexpr size_t kMaxPeers = 16;
    static constexpr size_t kDetailAnswersPerMin = 4, kCompsAnswersPerMin = 8;

    explicit Detector(Env& env) : env_(env) {}
    // Our identity for HELLO; call before Begin.
    void SetIdentity(const std::string& melange, const std::string& content16);
    void SetPeers(const std::vector<PeerInfo>& peers);        // lobby members other than us; [] outside a lobby
    void Begin(uint32_t serial, uint64_t matchKey, std::vector<wire::ContribName> contribs);
    void End();                                               // reports pending divergences with what is known
    bool InSession() const { return session_; }
    void OnTick(const TickHash& h);
    void OnPacket(uint64_t from, const uint8_t* p, size_t n);
    void Pump();                                              // sends HELLO/HASHES, times out requests

    int Peers(PeerStatus* out, int max) const;
    bool Flagged(uint32_t* tick) const;                       // our first divergence this match
    uint64_t MatchKey() const { return key_; }
    uint32_t BadPackets() const { return badPackets_; }

  private:
    enum class Phase : uint8_t { None, WaitComps, WaitDetail, Done };
    struct Peer {
        PeerInfo info;
        wire::Hello hello;
        bool helloIn = false, badProtocol = false, engineMismatch = false, otherMatch = false;
        uint64_t helloSentMs = 0;
        PeerCompare cmp;
        wire::DetailAssembly assembly;
        std::deque<uint64_t> detailAnswers, compsAnswers;
        Phase phase = Phase::None;
        uint64_t phaseMs = 0;
        Report rep;
        bool haveComps = false;
    };
    static bool Exchanging(const Peer& p);
    PeerState StateOf(const Peer& p) const;
    Peer* Find(uint64_t id);
    void ResetMatch(Peer& p);
    void SendHello(Peer& p);
    void SendBatch(Peer& p);
    bool RateOk(std::deque<uint64_t>& q, size_t perMin);
    void Start(Peer& p);
    void FinishComps(Peer& p);
    void FinishBundle(Peer& p);

    Env& env_;
    std::vector<std::unique_ptr<Peer>> peers_;
    std::string melange_, content16_;
    std::vector<wire::ContribName> contribs_;
    uint64_t contribHash_ = 0, key_ = 0;
    uint32_t serial_ = 0, firstTick_ = 0, latest_ = 0, lastBatch_ = 0, flaggedTick_ = 0, badPackets_ = 0;
    bool session_ = false, haveTick_ = false, flagged_ = false;
};

// Flattens JSON into "path value" pairs: object members as a.b, array items as a[3]. Values are compact JSON.
bool Flatten(std::string_view json, std::vector<std::pair<std::string, std::string>>* out, std::string* err);
// One line per differing field, "path ours -> theirs" ("(none)" when a side lacks it): our fields in document
// order, then those only the peer has.
std::vector<std::string> FieldDiff(std::string_view oursJson, std::string_view theirsJson, std::string* err);

// The lines of Melange.log whose "[+seconds]" stamp is within `seconds` of its last stamped line.
std::string LogTailSeconds(std::string_view log, double seconds);
}  // namespace melange::wormsign::detect
