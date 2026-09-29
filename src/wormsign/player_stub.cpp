// Placeholder until the replay player lands: arming always fails.
#include <cstdio>

#include "melange/wormsign.h"

namespace melange::wormsign {
bool Arm(const wchar_t*, char* err, size_t errLen) {
    if (err && errLen) snprintf(err, errLen, "replays are not available in this build");
    return false;
}
void Disarm() {}
bool SetPaused(bool) { return false; }
bool SetSpeed(float) { return false; }
bool RunTo(uint32_t) { return false; }
PlayStatus Status() { return PlayStatus{PlayState::Idle, 0, 0, 0, 0, 1.0f, ""}; }
}  // namespace melange::wormsign
