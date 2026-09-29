#include "wormsign/replay_core.h"

#include <algorithm>

namespace melange::wormsign {
void ReplayCore::Start(std::shared_ptr<const Recording> rec, bool compareEngine, bool compareMods) {
    rec_ = std::move(rec);
    sites_.clear();
    next_ = 0;
    compareEngine_ = compareEngine;
    compareMods_ = compareMods;
    diverged_ = false;
    n_ = Counters{};
    if (!rec_) return;
    for (uint32_t i = 0; i < rec_->draws.size(); ++i) sites_[Key(rec_->draws[i].rng, rec_->draws[i].ret)].idx.push_back(i);
}

bool ReplayCore::ForceSeed(int kind, uint32_t caller, uint32_t* value) {
    if (!rec_) return false;
    for (size_t i = rec_->seeds.size(); i-- > 0;) {
        const rec::Seed& s = rec_->seeds[i];
        if (s.kind == kind && s.caller == caller) {
            *value = s.value;
            ++n_.seedsForced;
            return true;
        }
    }
    return false;
}

bool ReplayCore::ForceDraw(int rng, uint32_t ret, uint32_t* stateAfter, uint32_t* bits) {
    auto it = sites_.find(Key(rng, ret));
    if (it == sites_.end() || it->second.next >= it->second.idx.size()) {
        ++n_.drawsUnforced;
        return false;
    }
    const rec::Draw& d = rec_->draws[it->second.idx[it->second.next++]];
    *stateAfter = d.stateAfter;
    *bits = d.bits;
    ++n_.drawsForced;
    return true;
}

ReplayCore::Cmp ReplayCore::Compare(const TickHash& live, TickHash* recorded, uint8_t* compMask) {
    *compMask = 0;
    if (!rec_ || !compareEngine_ || !rec_->Tick(live.tick, recorded)) return Cmp::NotRecorded;
    ++n_.compared;
    for (int i = 0; i < kEngineComps; ++i)
        if (live.c[i] != recorded->c[i]) *compMask |= static_cast<uint8_t>(1u << i);
    const bool same = live.engine == recorded->engine && (!compareMods_ || live.mods == recorded->mods);
    if (same) {
        ++n_.matched;
        return Cmp::Match;
    }
    if (!diverged_) {
        diverged_ = true;
        n_.firstDivergence = live.tick;
    }
    return Cmp::Mismatch;
}

int VirtualClock::Filter(int realNow, bool enginePaused) {
    if (!have_) {
        have_ = true;
        v_ = last_ = realNow;
        rem_ = 0;
        return realNow;
    }
    const int64_t dt = static_cast<int64_t>(realNow) - last_;
    last_ = realNow;
    int64_t adv;
    if (dt <= 0 || enginePaused) {
        adv = dt;
    } else if (paused_) {
        adv = 0;
    } else if (fast_) {
        adv = kMaxFrameMs;
    } else if (speed_ == 1000) {
        adv = dt;
    } else {
        const int64_t num = dt * speed_ + rem_;
        adv = num / 1000;
        rem_ = num % 1000;
        if (speed_ > 1000) adv = (std::min)(adv, (std::max)(dt, kMaxFrameMs));
    }
    v_ += adv;
    if (capOn_ && v_ > cap_) v_ = (std::max)(cap_, v_ - adv);
    return static_cast<int>(v_);
}
}  // namespace melange::wormsign
