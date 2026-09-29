#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "melange/wormsign.h"

// Payload layouts of the .wsr record chunks (little-endian, packed). The recorder appends, the player decodes.
//   SEED  n x {u8 kind, u32 value, u32 caller, u32 t}                                           13 bytes each
//   PDRW  n x {u8 rng, u32 ret, u32 stateAfter, u32 bits}                                        13 bytes each
//   INPT  n x {u8 type, u16 id, u32 a, u32 b, u32 time, u32 callT, u32 caller, u8 strLen, strLen bytes}
//         type: SendType (the sender's order); a, b: the int values or float bits; callT: logic time of the call
//   TICK  u32 firstTick, then records: u8 0 + {u64 engine, u64 mods, u64 c[6], u32 rngLogic, u32 rng2, u16 fpucw,
//         u16 inputs} (one tick, then the next tick number), or u8 1 + u32 n (TGAP: n ticks missing)
// A decoder stops at the first record that does not fit and reports it.
namespace melange::wormsign::rec {
constexpr size_t kSeedBytes = 13, kDrawBytes = 13, kInputFixedBytes = 24, kTickBytes = 76;
constexpr uint8_t kTickRec = 0, kTickGap = 1;
enum SendType : uint8_t { kSendMsg, kSendInt, kSendInt2, kSendFloat, kSendFloat2, kSendString, kSendTypes };

struct Seed {
    uint8_t kind;           // 0 logic, 1 second
    uint32_t value, caller, t;
};
struct Draw {
    uint8_t rng;            // 0 logic, 1 second
    uint32_t ret, stateAfter, bits;
};
struct Input {
    uint8_t type;           // SendType
    uint16_t id;
    uint32_t a, b, time, callT, caller;
    std::string str;        // kSendString only; at most 255 bytes
};

void AppendSeed(std::vector<uint8_t>& out, const Seed& s);
void AppendDraw(std::vector<uint8_t>& out, const Draw& d);
void AppendInput(std::vector<uint8_t>& out, const Input& i);

// Builds one TICK payload from consecutive tick hashes; a jump in tick numbers is written as a gap.
class TickChunk {
  public:
    void Add(const TickHash& h);
    bool Empty() const { return bytes_.empty(); }
    uint32_t Count() const { return count_; }
    uint32_t FirstTick() const { return first_; }
    uint32_t LastTick() const { return next_ ? next_ - 1 : 0; }
    std::vector<uint8_t> Take();

  private:
    std::vector<uint8_t> bytes_;
    uint32_t first_ = 0, next_ = 0, count_ = 0;
};

bool DecodeSeeds(const uint8_t* p, size_t n, std::vector<Seed>* out);
bool DecodeDraws(const uint8_t* p, size_t n, std::vector<Draw>* out);
bool DecodeInputs(const uint8_t* p, size_t n, std::vector<Input>* out);
// Calls fn(const TickHash&) for each tick record, with tick numbers filled in.
template <class Fn>
bool DecodeTicks(const uint8_t* p, size_t n, Fn&& fn);

namespace detail {
uint32_t Rd32(const uint8_t* p);
uint64_t Rd64(const uint8_t* p);
void ReadTick(const uint8_t* p, TickHash* h);
}  // namespace detail

template <class Fn>
bool DecodeTicks(const uint8_t* p, size_t n, Fn&& fn) {
    if (n < 4) return n == 0;
    uint32_t tick = detail::Rd32(p);
    size_t at = 4;
    while (at < n) {
        const uint8_t kind = p[at++];
        if (kind == kTickRec) {
            if (n - at < kTickBytes) return false;
            TickHash h{};
            detail::ReadTick(p + at, &h);
            h.tick = tick++;
            fn(static_cast<const TickHash&>(h));
            at += kTickBytes;
        } else if (kind == kTickGap) {
            if (n - at < 4) return false;
            const uint32_t skip = detail::Rd32(p + at);
            if (skip > 0xffffffffu - tick) return false;
            tick += skip;
            at += 4;
        } else {
            return false;
        }
    }
    return true;
}
}  // namespace melange::wormsign::rec
