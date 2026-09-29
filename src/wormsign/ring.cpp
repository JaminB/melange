#include "wormsign/ring.h"

#include <algorithm>

namespace melange::wormsign {
namespace {
constexpr uint32_t kEmpty = 0xffffffffu;
}

TickRing::TickRing(uint32_t capTicks) : cap_((std::max)(capTicks, kBlockTicks)) {}

TickRing::Block TickRing::NewBlock() {
    Block b;
    if (!spare_.empty()) {
        b = std::move(spare_.back());
        spare_.pop_back();
    } else {
        b.reset(new TickHash[kBlockTicks]);
    }
    for (uint32_t i = 0; i < kBlockTicks; ++i) b[i].tick = kEmpty;
    return b;
}

void TickRing::Reset() {
    while (!blocks_.empty()) {
        if (spare_.empty()) spare_.push_back(std::move(blocks_.front()));
        blocks_.pop_front();
    }
    base_ = count_ = 0;
    any_ = false;
}

void TickRing::Put(const TickHash& h) {
    if (any_ && (h.tick < base_ || h.tick - base_ < count_)) return;
    if (!any_ || h.tick - base_ - count_ >= cap_) {
        Reset();
        any_ = true;
        base_ = h.tick;
    }
    const uint32_t idx = h.tick - base_;
    while (blocks_.size() * kBlockTicks <= idx) blocks_.push_back(NewBlock());
    count_ = idx + 1;
    blocks_[idx / kBlockTicks][idx % kBlockTicks] = h;
    while (count_ > kBlockTicks && count_ - kBlockTicks >= cap_) {
        if (spare_.size() < 2) spare_.push_back(std::move(blocks_.front()));
        blocks_.pop_front();
        base_ += kBlockTicks;
        count_ -= kBlockTicks;
    }
}

bool TickRing::Get(uint32_t tick, TickHash* out) const {
    if (!any_ || tick < base_ || tick - base_ >= count_) return false;
    const uint32_t idx = tick - base_;
    const TickHash& h = blocks_[idx / kBlockTicks][idx % kBlockTicks];
    if (h.tick != tick) return false;
    if (out) *out = h;
    return true;
}

size_t TickRing::Bytes() const { return (blocks_.size() + spare_.size()) * kBlockTicks * sizeof(TickHash); }
}  // namespace melange::wormsign
