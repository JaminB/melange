#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "melange/wormsign.h"

namespace melange::wormsign {
// In-match tick ring: 4096-tick blocks, grown to the session's length; past the cap the oldest block is reused.
class TickRing {
  public:
    static constexpr uint32_t kBlockTicks = 4096;
    static constexpr uint32_t kCapTicks = 360000;
    explicit TickRing(uint32_t capTicks = kCapTicks);
    void Reset();                               // keeps one block for the next session
    void Put(const TickHash& h);                // ticks ascend; a gap leaves empty slots
    bool Get(uint32_t tick, TickHash* out) const;
    uint32_t FirstTick() const { return base_; }
    uint32_t Count() const { return count_; }   // slots from FirstTick(), gaps included
    size_t Bytes() const;

  private:
    using Block = std::unique_ptr<TickHash[]>;
    Block NewBlock();
    std::deque<Block> blocks_;
    std::vector<Block> spare_;
    uint32_t cap_, base_ = 0, count_ = 0;
    bool any_ = false;
};
}  // namespace melange::wormsign
