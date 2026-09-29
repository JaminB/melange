#pragma once
#include <cstdint>
#include <string>

#include "melange/wormsign.h"
#include "wormsign/replay_core.h"

// The replay player behind melange/wormsign.h Arm/Disarm/SetPaused/SetSpeed/RunTo/Status.
namespace melange::wormsign::player {
void Install();                  // the Wormsign module: session observer, lobby events, panel, test verbs
void Uninstall();

struct Info {
    std::wstring path;
    std::string note;            // setup/compare notes ("mods hashes not compared: ...")
    ReplayCore::Counters n;
    size_t inputs = 0, nextInput = 0;
    uint32_t runTo = 0, blocked = 0;
    bool restartPending = false;
};
Info GetInfo();                  // any thread
struct DivergenceDetail {
    Divergence d;
    TickHash recorded, live;
};
bool LastDivergence(DivergenceDetail* out);  // any thread; false before the first divergence of this replay
// Re-arms the same recording (now, or when the current match ends) and fast-forwards to `runTo` once it plays.
bool Restart(uint32_t runTo, char* err, size_t errLen);
std::wstring ReplaysDir();       // Documents\Melange\replays
}  // namespace melange::wormsign::player
