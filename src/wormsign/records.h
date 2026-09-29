#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "melange/wormsign.h"

// Payload layouts of the .wsr record chunks (little-endian, packed). The recorder appends, the player decodes.
//   SEED  n x {u8 kind, u32 value, u32 caller, u32 t}                                           13 bytes each
//   PDRW  n x {u8 rng, u32 ret, u32 stateAfter, u32 bits}                                        13 bytes each
//   INPT  n x {u8 type, u16 id, u32 a, u32 b, u32 time, u32 callT, u32 caller, u8 strLen, strLen bytes}
//         type: SendType (the sender's order); a, b: the int values or float bits; callT: logic time of the call
//   RMTI  n x {u32 arrivedT, u16 id, u32 time, u32 a}; DISP  n x {u32 t, u16 id}
//   CTRB  records {u32 tick, u8 n, n x {u8 index, u64 hash}}: the per-contributor hashes that changed at that tick,
//         index = position in name order (HEAD contributors); a record with every index restates the whole list
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
struct ContribChange {
    uint32_t tick;
    uint8_t index;
    uint64_t hash;
};
void AppendContribChanges(std::vector<uint8_t>& out, uint32_t tick,
                          const std::vector<std::pair<uint8_t, uint64_t>>& changes);
bool DecodeContribChanges(const uint8_t* p, size_t n, std::vector<ContribChange>* out);
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

// RMTI: {arrivedT u32, id u16, time u32, a u32} -- fixed 14 bytes.
constexpr size_t kRemoteInputBytes = 14;
inline void AppendRemoteInput(std::vector<uint8_t>& out, uint32_t arrivedT, uint16_t id, uint32_t time, uint32_t a) {
    const size_t base = out.size();
    out.resize(base + kRemoteInputBytes);
    uint8_t* p = out.data() + base;
    memcpy(p, &arrivedT, 4), p += 4;
    memcpy(p, &id, 2), p += 2;
    memcpy(p, &time, 4), p += 4;
    memcpy(p, &a, 4);
}
template <class Fn>
int64_t ForEachRemoteInput(const uint8_t* data, size_t n, Fn&& fn) {
    if (n % kRemoteInputBytes) return -1;
    const size_t count = n / kRemoteInputBytes;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = data + i * kRemoteInputBytes;
        uint32_t arrivedT, time, a;
        uint16_t id;
        memcpy(&arrivedT, p, 4);
        memcpy(&id, p + 4, 2);
        memcpy(&time, p + 6, 4);
        memcpy(&a, p + 10, 4);
        fn(arrivedT, id, time, a);
    }
    return static_cast<int64_t>(count);
}

// DISP (diagnostic): {t u32, id u16} -- fixed 6 bytes.
constexpr size_t kDispatchBytes = 6;
inline void AppendDispatch(std::vector<uint8_t>& out, uint32_t t, uint16_t id) {
    const size_t base = out.size();
    out.resize(base + kDispatchBytes);
    memcpy(out.data() + base, &t, 4);
    memcpy(out.data() + base + 4, &id, 2);
}

}  // namespace melange::wormsign::rec
