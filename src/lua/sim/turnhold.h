#pragma once
#include <cstdint>

#include "lua/engine50.h"

// The deterministic turn-start hold (turnhold_core.cpp): wum.sim.holdTurn / releaseTurn / turnHold, the StartTurn
// wrapper in the match VM and the tick hook that releases it. Shared between the module (turnhold.cpp) and the
// offline sim self-test, which links the core only.
//
// How it works. The game's own match script (stdlib.lub) starts every turn after the first from one Lua global,
// `StartTurn()`: GameLogic.Turn.Ended -> TurnEnded() -> CheckOneTeamVictory() -> StartTurn() -> ActivateNextWorm,
// StartHotSeatTimer, SetWind, GameLogic.Turn.Started, RunAILogic. A hold wraps that global the first time a sim mod
// arms one: the wrapper swallows the call, sends GameLogic.RoundTime.Pause (TimerLogicEntity 0x50f100 then skips
// both RoundTimeRemaining and ElapsedRoundTime; the turn clock is already stopped between turns), and the tick hook
// calls the original StartTurn N ticks later after GameLogic.RoundTime.Resume. Everything runs from the sim's own
// clock on every machine, so the next turn starts at the same tick everywhere and nothing waits on a camera, an
// ActiveObject or any other per-machine state.
namespace melange::turnhold {
constexpr uint32_t kMaxTicks = 3000;  // 60 s at 50 ticks/s: the hard cap on one hold, armed or extended

// wum.sim.* trampolines (upvalue 1 = mod index, the AddSimFunction contract).
int __cdecl LHoldTurn(lua50::State* L);     // holdTurn(ticks) -> true | nil, reason; raises on bad arguments
int __cdecl LReleaseTurn(lua50::State* L);  // releaseTurn() -> true when a hold was armed or active
int __cdecl LTurnHold(lua50::State* L);     // turnHold() -> remaining, armed (0 when none)

// Notifications for the client side (turnhold.cpp posts them as client events and JSONL records).
struct Event {
    bool begin;          // true: the next turn start was just deferred; false: it is about to run
    uint32_t ticks;      // begin: the hold's length; end: ticks actually held
    uint32_t tick;       // sim tick of the event
    int32_t roundMsLeft; // RoundTimeRemaining at the event, -1 when unreadable
};
using NotifyFn = void (*)(const Event& e, void* user);
void SetNotify(NotifyFn fn, void* user);

// Installs the OnMatch observer that drops the wrapper's ref at match end. Idempotent; the trampolines call it too.
void Init();
}  // namespace melange::turnhold
