// The deterministic turn-start hold: see turnhold.h for the design. This file follows the sim bridge's Lua 5.0 rules:
// the trampolines keep no object with a destructor alive across a call that can raise, and every raise goes through
// Fail() after the C++ helpers have returned.
#include "lua/sim/turnhold.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"
#include "melange/sim.h"

namespace melange::turnhold {
namespace {
namespace l5 = lua50;
using simcore::g;

struct State {
    uint32_t serial = 0;      // the match VM this state belongs to (simcore::Match::serial)
    uint32_t armed = 0;       // ticks for the next StartTurn, 0 = none
    bool holding = false;     // StartTurn was swallowed and is pending
    uint32_t remaining = 0;   // ticks until the deferred StartTurn runs
    uint32_t total = 0;       // ticks of the current hold as armed or extended
    uint32_t since = 0;       // tick at which StartTurn was deferred
    uint32_t swallowed = 0;   // StartTurn calls swallowed during the hold (a second turn end during it)
    uint32_t holds = 0;       // holds completed this match
    bool paused = false;      // GameLogic.RoundTime.Pause was accepted
    int origRef = -1;         // registry ref of the game's StartTurn; -1: wrapper not installed
};
State s;
NotifyFn g_notify = nullptr;
void* g_notifyUser = nullptr;
int g_tickHook = 0;
int g_matchObserver = 0;
char g_err[256];

int Fail(l5::State* L, const char* msg) {
    snprintf(g_err, sizeof g_err, "%s", msg);
    l5::A().pushstring(L, g_err);
    l5::A().error(L);
    return 0;
}

int NilReason(l5::State* L, const char* reason) {
    l5::A().pushnil(L);
    l5::A().pushstring(L, reason);
    return 2;
}

// The state belongs to one match VM: a new serial (or no VM) resets it. The ref died with the old VM.
void Sync() {
    if (!g.L) {
        s = State{};
        return;
    }
    if (s.serial != g.serial) {
        s = State{};
        s.serial = g.serial;
    }
}

// Reads an integer data value through the engine's GetData (the ref the bridge captured at Init). -1 when unreadable.
int32_t ReadInt(const char* name) {
    if (!g.L || g.engineRef[simcore::kGetData] < 0 || l5::RunState() != 1) return -1;
    const auto& a = l5::A();
    const int top = a.gettop(g.L);
    if (!a.checkstack(g.L, 4)) return -1;
    simcore::SuspendBudget();
    a.rawgeti(g.L, l5::kRegistry, g.engineRef[simcore::kGetData]);
    a.pushstring(g.L, name);
    const int rc = a.pcall(g.L, 1, 1, 0);
    simcore::ResumeBudget();
    int32_t v = -1;
    if (!rc && a.isnumber(g.L, -1)) v = static_cast<int32_t>(a.tonumber(g.L, -1));
    a.settop(g.L, top);
    return v;
}

void Notify(bool begin, uint32_t ticks) {
    if (!g_notify) return;
    Event e{begin, ticks, g.tick, ReadInt("RoundTimeRemaining")};
    try {
        g_notify(e, g_notifyUser);
    } catch (...) {
        LOG_ERROR("[sim] turn hold: the notifier threw");
    }
}

// Calls the game's StartTurn under pcall. Returns the pcall code and leaves the error message on the stack on
// failure (the caller decides whether to re-raise it).
int CallOriginal(l5::State* L) {
    const auto& a = l5::A();
    a.rawgeti(L, l5::kRegistry, s.origRef);
    return a.pcall(L, 0, 0, 0);
}

// The replacement for the global `StartTurn`. Outside a hold it is the original; with a hold armed it defers.
int __cdecl LStartTurnWrapper(l5::State* L) {
    const auto& a = l5::A();
    Sync();
    if (!g.L || L != g.L || s.origRef < 0) return 0;  // another VM: nothing we can call
    if (s.holding) {
        ++s.swallowed;
        LOG_WARN("[sim] turn hold: StartTurn called again during the hold at tick %u (swallowed, %u so far)", g.tick,
                 s.swallowed);
        return 0;
    }
    if (s.armed) {
        s.holding = true;
        s.remaining = s.total = s.armed;
        s.armed = 0;
        s.since = g.tick;
        s.swallowed = 0;
        s.paused = sim::Send("GameLogic.RoundTime.Pause") == sim::SendResult::Ok;
        const int32_t left = ReadInt("RoundTimeRemaining");
        LOG_INFO("[sim] turn hold: next turn start deferred %u ticks at tick %u (round clock %s, %d ms left)", s.total,
                 s.since, s.paused ? "paused" : "NOT paused", left);
        const std::vector<float> args{static_cast<float>(s.total)};
        simbridge::Dispatch("sim.turnHold", args);
        Notify(true, s.total);
        return 0;
    }
    const int top = a.gettop(L);
    if (CallOriginal(L) == 0) {
        a.settop(L, top);
        return 0;
    }
    // Re-raise the original's error (now the top of the stack) so the engine's CallGlobal sees the same failure it
    // would without the wrapper.
    a.error(L);
    return 0;
}

// Runs the deferred StartTurn (or drops it when the round is over).
void Release() {
    l5::State* L = g.L;
    const uint32_t held = g.tick - s.since;
    s.holding = false;
    s.remaining = 0;
    ++s.holds;
    if (s.paused) {
        const auto r = sim::Send("GameLogic.RoundTime.Resume");
        if (r != sim::SendResult::Ok) LOG_WARN("[sim] turn hold: RoundTime.Resume refused (%s)", simcore::SendResultText(r));
        s.paused = false;
    }
    const int32_t left = ReadInt("RoundTimeRemaining");
    const int32_t over = ReadInt("FCS.GameOver");
    LOG_INFO("[sim] turn hold: released at tick %u after %u ticks (%u swallowed, %d ms left%s)", g.tick, held,
             s.swallowed, left, over > 0 ? ", game over: StartTurn skipped" : "");
    simbridge::Dispatch("sim.turnHoldEnd", std::vector<float>{static_cast<float>(held)});
    Notify(false, held);
    if (over > 0 || s.origRef < 0 || l5::RunState() != 1 || !g.L) return;
    const auto& a = l5::A();
    const int top = a.gettop(L);
    if (!a.checkstack(L, 4)) return;
    simcore::SuspendBudget();
    const int rc = CallOriginal(L);
    simcore::ResumeBudget();
    if (rc) {
        const char* msg = a.type(L, -1) == l5::kTString ? a.tostring(L, -1) : "?";
        LOG_ERROR("[sim] turn hold: the deferred StartTurn raised: %s", msg);
    }
    a.settop(L, top);
}

void OnTick(uint32_t, void*) {
    Sync();
    if (!s.holding || !g.L) return;
    if (s.remaining) --s.remaining;
    if (s.remaining == 0) Release();
}

// Match end: the VM is about to close, so the wrapper's ref goes back before the bridge counts what is left. Match
// begin fires after the top-level chunks ran (a chunk may already have armed a hold), so it only syncs.
void OnMatch(bool begin, void*) {
    if (begin) {
        Sync();
        return;
    }
    if (s.holding) LOG_WARN("[sim] turn hold: the match closed with a hold active (%u ticks left)", s.remaining);
    simcore::DropRef(s.origRef);
    s = State{};
}

// Replaces the global StartTurn with the wrapper, once per match. nullptr on success, else the reason.
const char* InstallWrapper(l5::State* L) {
    if (s.origRef >= 0) return nullptr;
    const auto& a = l5::A();
    if (!a.checkstack(L, 4)) return "no stack";
    const int top = a.gettop(L);
    a.pushstring(L, "StartTurn");
    a.rawget(L, l5::kGlobals);
    if (a.type(L, -1) != l5::kTFunction) {
        a.settop(L, top);
        return "the match script has no StartTurn";
    }
    s.origRef = simcore::TakeRef();  // pops the original
    if (s.origRef < 0) {
        a.settop(L, top);
        return "no registry ref";
    }
    a.pushstring(L, "StartTurn");
    a.pushcclosure(L, &LStartTurnWrapper, 0);
    a.rawset(L, l5::kGlobals);
    a.settop(L, top);
    LOG_INFO("[sim] turn hold: StartTurn wrapped in match %u (tick %u)", g.serial, g.tick);
    return nullptr;
}
}  // namespace

void SetNotify(NotifyFn fn, void* user) {
    g_notify = fn;
    g_notifyUser = user;
}

void Init() {
    if (!g_matchObserver) g_matchObserver = simbridge::OnMatch(&OnMatch, nullptr);
    if (!g_tickHook) g_tickHook = sim::AddTickHook(&OnTick, nullptr, 100);  // after other hooks: they see the hold's state
}

int __cdecl LHoldTurn(l5::State* L) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1)) return Fail(L, "wum.sim.holdTurn: expected a number of ticks");
    const float f = a.tonumber(L, 1);
    if (!(f >= 1 && f <= static_cast<float>(kMaxTicks)) || f != static_cast<float>(static_cast<uint32_t>(f)))
        return Fail(L, "wum.sim.holdTurn: ticks must be a whole number from 1 to 3000");
    const uint32_t ticks = static_cast<uint32_t>(f);
    Init();
    Sync();
    if (!g.L || L != g.L) return NilReason(L, "not in a match");
    if (l5::RunState() != 1) return NilReason(L, "halted");
    if (const char* why = InstallWrapper(L)) return NilReason(L, why);
    const char* mod = simbridge::CurrentMod();
    if (s.holding) {
        // Extend or shorten the running hold; the cap counts from the deferral.
        const uint32_t elapsed = g.tick - s.since;
        const uint32_t room = elapsed >= kMaxTicks ? 0 : kMaxTicks - elapsed;
        s.remaining = std::min(ticks, room);
        s.total = elapsed + s.remaining;
        LOG_INFO("[sim] turn hold: %s set the active hold to %u more ticks at tick %u", mod ? mod : "?", s.remaining, g.tick);
    } else {
        s.armed = ticks;
        LOG_INFO("[sim] turn hold: %s armed %u ticks for the next turn start (tick %u)", mod ? mod : "?", ticks, g.tick);
    }
    a.pushboolean(L, 1);
    return 1;
}

int __cdecl LReleaseTurn(l5::State* L) {
    Init();
    Sync();
    bool changed = false;
    if (g.L && L == g.L) {
        if (s.holding && s.remaining) {
            s.remaining = 0;  // the tick hook releases at the end of this tick
            changed = true;
        }
        if (s.armed) {
            s.armed = 0;
            changed = true;
        }
    }
    if (changed) {
        const char* mod = simbridge::CurrentMod();
        LOG_INFO("[sim] turn hold: %s released at tick %u", mod ? mod : "?", g.tick);
    }
    l5::A().pushboolean(L, changed);
    return 1;
}

int __cdecl LTurnHold(l5::State* L) {
    Sync();
    const auto& a = l5::A();
    a.pushnumber(L, static_cast<float>(s.holding ? s.remaining : 0));
    a.pushnumber(L, static_cast<float>(s.armed));
    return 2;
}
}  // namespace melange::turnhold

namespace melange::sim {
TurnHold GetTurnHold() {
    using namespace turnhold;
    TurnHold t{};
    if (!simcore::g.L || s.serial != simcore::g.serial) return t;
    t.holding = s.holding;
    t.remaining = s.holding ? s.remaining : 0;
    t.total = s.holding ? s.total : 0;
    t.since = s.holding ? s.since : 0;
    t.armed = s.armed;
    t.holds = s.holds;
    return t;
}
}  // namespace melange::sim
