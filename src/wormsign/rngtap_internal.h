#pragma once
#include <cstdint>
#include <vector>

// Additive to rngtap.h: the recorder's read side of what rngtap.cpp already taps -- the seed log and the
// pre-match draw buffer that the recording's SEED and PDRW chunks are built from.
namespace melange::wormsign::rngtap {
struct SeedEvent {
    int kind;  // 0 logic, 1 second
    uint32_t value, caller, t;
};
struct DrawEvent {
    int rng;  // 0 logic, 1 second
    uint32_t ret, bits, stateAfter;
};
bool Install();
bool Installed();
// Session begin: the seed calls since the menu-entry seed (the one before the match-start seed), then cleared.
std::vector<SeedEvent> TakeSessionSeeds();
// Session begin: the draws since the menu-entry seed, then cleared. `overflow` is set once the
// 200000-entry cap was hit and later draws were dropped -- the recording is then marked pre-match incomplete.
std::vector<DrawEvent> PreMatchDraws(bool* overflow);
}  // namespace melange::wormsign::rngtap
