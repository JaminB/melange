#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The hash exchange's packets, protocol v1 (little-endian). Every packet starts with
// {"WSX1", u8 kind, u8 protocol, u16 total bytes} and is at most kMaxPacket bytes.
namespace melange::wormsign::wire {
constexpr uint32_t kMagic = 0x31585357;  // "WSX1"
constexpr uint8_t kProtocol = 1;
constexpr size_t kHeaderBytes = 8;
constexpr size_t kMaxPacket = 1100;
constexpr int kChannel = 5;
constexpr const char* kLobbyKey = "mlg.ws";     // lobby member key: the exchange protocol a member speaks
constexpr const char* kLobbyValue = "1";
constexpr uint32_t kMaxBatch = 50;
constexpr uint32_t kMaxCompContribs = 120;
constexpr size_t kDetailChunkBytes = 1024;
constexpr uint32_t kMaxDetailBytes = 64 * 1024;
constexpr uint32_t kMaxDetailChunks = kMaxDetailBytes / kDetailChunkBytes;
constexpr size_t kMaxNameBytes = 63;

enum class Kind : uint8_t { Hello = 1, Hashes, CompsReq, Comps, DetailReq, Detail, Flag };

struct ContribName {
    std::string name;
    uint32_t version = 1;
};
struct Hello {
    uint32_t engineHash = 0;
    uint64_t matchKey = 0;                 // MatchKey(lobby owner, lobby)
    uint32_t firstTick = 0;                // the sender's first tick this match, 0 before it
    uint64_t contribHash = 0;              // over every contributor name and version, even those left out below
    bool seenYou = false;                  // the sender has our HELLO
    bool namesTruncated = false;
    std::string melange, content16;        // Melange version, content hash prefix ("v" vanilla)
    std::vector<ContribName> contribs;
};
struct TickPair {
    uint64_t engine = 0, mods = 0;
};
struct Hashes {
    uint64_t matchKey = 0;
    uint32_t firstTick = 0;
    uint64_t present = 0;                  // bit i: tick firstTick+i is known to the sender
    std::vector<TickPair> ticks;           // <= kMaxBatch
};
struct TickReq {
    uint64_t matchKey = 0;
    uint32_t tick = 0;
};
struct Comps {
    uint64_t matchKey = 0;
    uint32_t tick = 0;
    bool have = false;
    uint64_t engine = 0, mods = 0, c[6] = {};
    std::vector<uint64_t> contrib;         // per contributor, in the HELLO's name order
};
struct DetailChunk {
    uint64_t matchKey = 0;
    uint32_t tick = 0;
    uint16_t index = 0, count = 0;
    uint32_t total = 0;
    std::vector<uint8_t> bytes;
};
struct Flag {
    uint64_t matchKey = 0;
    uint32_t tick = 0;
    uint64_t engine = 0, mods = 0, c[6] = {};
};

struct Packet {
    Kind kind = Kind::Hello;
    uint8_t protocol = 0;
    Hello hello;
    Hashes hashes;
    TickReq req;
    Comps comps;
    DetailChunk detail;
    Flag flag;
};

std::vector<uint8_t> Encode(const Hello& m);
std::vector<uint8_t> Encode(const Hashes& m);
std::vector<uint8_t> Encode(Kind reqKind, const TickReq& m);  // CompsReq or DetailReq
std::vector<uint8_t> Encode(const Comps& m);
std::vector<uint8_t> Encode(const DetailChunk& m);
std::vector<uint8_t> Encode(const Flag& m);
// Splits a detail record into DETAIL chunks; empty when it is over kMaxDetailBytes.
std::vector<std::vector<uint8_t>> EncodeDetail(uint64_t matchKey, uint32_t tick, const std::string& json);

enum class DecodeResult : uint8_t { Ok, NotOurs, BadProtocol, Malformed };
// NotOurs: too short, too long or no magic. BadProtocol: our magic with another protocol (p->protocol is set).
DecodeResult Decode(const uint8_t* p, size_t n, Packet* out);

// Reassembles one detail record (<= kMaxDetailBytes) from chunks of one (matchKey, tick).
class DetailAssembly {
  public:
    void Reset();
    // True once every chunk has arrived; the record is then in *json.
    bool Add(const DetailChunk& c, std::string* json);
    uint32_t Tick() const { return tick_; }

  private:
    uint64_t key_ = 0;
    uint32_t tick_ = 0, total_ = 0;
    uint16_t count_ = 0, got_ = 0;
    std::vector<uint8_t> buf_;
    std::vector<bool> have_;
};

uint64_t ContribListHash(const std::vector<ContribName>& names);
uint64_t MatchKey(uint64_t hostSteamId, uint64_t lobby);
}  // namespace melange::wormsign::wire
