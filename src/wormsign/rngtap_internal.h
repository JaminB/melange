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
// Every seed call recorded since the session began (cleared at Begin()).
std::vector<SeedEvent> TakeSessionSeeds();
// Pre-match draws since the last logic-RNG seed (menu entry); cleared there. `overflow` is set once the
// 200000-entry cap was hit and later draws were dropped -- the recording is then marked pre-match incomplete.
std::vector<DrawEvent> PreMatchDraws(bool* overflow);
}  // namespace melange::wormsign::rngtap
