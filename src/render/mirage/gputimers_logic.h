#pragma once
// Pure state machine for one triple-buffered GPU timer query (render/mirage/gputimers.cpp drives it with real
// GL calls). No GL, no windows.h: offline self-tested in tests/trace_selftest.cpp.
#include <cstdint>

namespace melange::mirage::gputimers::logic {
constexpr int kSlots = 3;
// A region that has not been timed for this many frames (its stage had no callbacks, or timers were off) reads as
// having no value, instead of showing the last result it ever had for as long as the game runs.
constexpr uint64_t kStaleFrames = 2 * kSlots + 2;

struct Region {
    int head = 0;
    bool issued[kSlots] = {false, false, false};
    double lastMs = -1.0;  // -1 until the first result has ever landed
    bool valid = false;
    bool everIssued = false;
    uint64_t lastIssuedFrame = 0;  // the caller's frame counter at the last Advance
};

// Before issuing Begin() into `head`, the caller should poll that slot's query only if it was issued before (an
// unissued slot has nothing to read yet).
inline bool ShouldPoll(const Region& r) { return r.issued[r.head]; }

// The caller read `head`'s query result (or found it not yet available).
inline void Resolve(Region& r, bool available, uint64_t ns) {
    if (!available) return;
    r.lastMs = static_cast<double>(ns) / 1e6;
    r.valid = true;
}

// A Begin/End pair was just issued into `head` on frame `frame`; move on to the next slot.
inline void Advance(Region& r, uint64_t frame) {
    r.issued[r.head] = true;
    r.head = (r.head + 1) % kSlots;
    r.everIssued = true;
    r.lastIssuedFrame = frame;
}

// The last result, if there is one and the region was timed within kStaleFrames of `frame`.
inline bool Current(const Region& r, uint64_t frame) {
    return r.valid && r.everIssued && frame >= r.lastIssuedFrame && frame - r.lastIssuedFrame <= kStaleFrames;
}
}  // namespace melange::mirage::gputimers::logic
