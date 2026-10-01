#pragma once
// Pure state machine for one triple-buffered GPU timer query (render/mirage/gputimers.cpp drives it with real
// GL calls). No GL, no windows.h: offline self-tested in tests/trace_selftest.cpp.
#include <cstdint>

namespace melange::mirage::gputimers::logic {
constexpr int kSlots = 3;

struct Region {
    int head = 0;
    bool issued[kSlots] = {false, false, false};
    double lastMs = -1.0;  // -1 until the first result has ever landed
    bool valid = false;
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

// A Begin/End pair was just issued into `head`; move on to the next slot.
inline void Advance(Region& r) {
    r.issued[r.head] = true;
    r.head = (r.head + 1) % kSlots;
}
}  // namespace melange::mirage::gputimers::logic
