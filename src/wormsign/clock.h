#pragma once
#include <cstdint>
// The tick clock: a tick b covers logic time (20b-20, 20b] and ends just before the scheduler dispatches the first
// task of b+1. Sessions open when the match VM appears and close when it goes.
namespace melange::wormsign::clock {
bool Install();                           // mid-hook 0x68d85c, inline TaskManager::Update 0x68d4a8
// Before the first task of `tick`, then before each later logic time within it (the replay player injects here).
using PreTickFn = void (*)(uint32_t tick, uint32_t timeMs);
void SetPreTick(PreTickFn fn);            // one owner: the player; null when idle
uint32_t CurrentBucket();                 // the bucket being run (0 outside a session)

bool Installed();                         // Install() succeeded
void Uninstall();
bool SetHooksEnabled(bool on);            // off: every hook disabled (original bytes back), the session closed
bool HooksEnabled();
bool ProloguesOriginal();                 // both hooked sites hold their original bytes
bool Paused();                            // TM+0x3c: the scheduler runs no task
}  // namespace melange::wormsign::clock
