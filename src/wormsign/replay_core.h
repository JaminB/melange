#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "melange/wormsign.h"
#include "wormsign/recording.h"

// The replay player's engine-free logic: forced seeds and pre-match draws, the input schedule, the per-tick compare
// and the virtual scheduler clock. player.cpp wires it to the game.
namespace melange::wormsign {
class ReplayCore {
  public:
    struct Counters {
        uint32_t seedsForced = 0, drawsForced = 0, drawsUnforced = 0;
        uint32_t injected = 0, skipped = 0, late = 0, failed = 0;
        uint32_t compared = 0, matched = 0, firstDivergence = 0;
    };
    enum class Cmp : uint8_t { NotRecorded, Match, Mismatch };

    // compareEngine: the recording's engine hash version is ours; compareMods: its contributor set is ours.
    void Start(std::shared_ptr<const Recording> rec, bool compareEngine, bool compareMods);
    void SetSkip(uint16_t id, uint32_t time) { skipId_ = id, skipTime_ = time; }  // time 0: every input of id
    const Recording* Rec() const { return rec_.get(); }
    bool ComparesEngine() const { return compareEngine_; }
    bool ComparesMods() const { return compareMods_; }

    // The latest recorded seed from the same (kind, caller): the match-start seed whatever the menu route did.
    bool ForceSeed(int kind, uint32_t caller, uint32_t* value);
    // The next recorded draw of the same (rng, return address); false once that site's draws are used up.
    bool ForceDraw(int rng, uint32_t ret, uint32_t* stateAfter, uint32_t* bits);

    // Sends every input whose call time is <= timeMs; send(const rec::Input&) returns false when injection failed.
    template <class Send>
    void Due(uint32_t timeMs, Send&& send);
    size_t NextInput() const { return next_; }

    Cmp Compare(const TickHash& live, TickHash* recorded, uint8_t* compMask);
    const Counters& Count() const { return n_; }

  private:
    struct Site {
        std::vector<uint32_t> idx;
        size_t next = 0;
    };
    static uint64_t Key(int rng, uint32_t ret) { return static_cast<uint64_t>(rng & 1) << 32 | ret; }
    std::shared_ptr<const Recording> rec_;
    std::unordered_map<uint64_t, Site> sites_;
    size_t next_ = 0;
    uint16_t skipId_ = 0;
    uint32_t skipTime_ = 0;
    bool compareEngine_ = true, compareMods_ = false, diverged_ = false;
    Counters n_;
};

template <class Send>
void ReplayCore::Due(uint32_t timeMs, Send&& send) {
    if (!rec_) return;
    const auto& in = rec_->inputs;
    while (next_ < in.size() && in[next_].callT <= timeMs) {
        const rec::Input& i = in[next_++];
        if (skipId_ && i.id == skipId_ && (!skipTime_ || i.time == skipTime_)) {
            ++n_.skipped;
            continue;
        }
        if (i.time <= timeMs) {
            ++n_.late;
            continue;
        }
        if (send(i)) ++n_.injected;
        else ++n_.failed;
    }
}

// The scheduler time handed to the game while a replay plays: real time scaled by the speed, frozen while paused,
// a fixed step per frame while fast-forwarding, and never past `cap`. Integer arithmetic only.
class VirtualClock {
  public:
    static constexpr int64_t kMaxFrameMs = 320;   // at most 16 ticks of scaled or fast-forward time per frame
    void Reset() { have_ = false, rem_ = 0; }
    void SetSpeedMilli(uint32_t s) { speed_ = s; }
    uint32_t SpeedMilli() const { return speed_; }
    void SetPaused(bool p) { paused_ = p; }
    bool Paused() const { return paused_; }
    void SetFast(bool f) { fast_ = f; }
    bool Fast() const { return fast_; }
    void SetCap(bool on, int64_t cap = 0) { capOn_ = on, cap_ = cap; }
    // enginePaused: the game's own pause, during which the time base moves with real time.
    int Filter(int realNow, bool enginePaused);

  private:
    bool have_ = false, paused_ = false, fast_ = false, capOn_ = false;
    int64_t v_ = 0, last_ = 0, rem_ = 0, cap_ = 0;
    uint32_t speed_ = 1000;
};
}  // namespace melange::wormsign
