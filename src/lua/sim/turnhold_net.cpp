// SimTurnHoldNet: keeps an online match flowing through a turn-start hold (wum.sim.holdTurn).
//
// The net throttle at a turn boundary (NetService): at GameLogic.Turn.Ended every machine sets its lead to 0
// (0x707bc5), and the active player's machine sends TimeSync(-1) ("others run till end of turn", 0x707277), on which
// a spectator runs unthrottled. At GameLogic.Turn.Started (0x709827) every machine pauses; the new player's machine
// sends TimeSync(T) and unpauses, the others wait until that lead passes T+3000. In the vanilla game Turn.Started
// follows Turn.Ended at once, so the -1 and the pause meet. A hold opens a window between them, and two things go
// wrong in it (both seen on LocalNet, 2026-10-24): the old active player keeps sending its periodic TimeSync(T)
// (0x707155, every 10 in-game ticks while its player is current), so the spectator's lead is finite again and the
// throttle pauses it 2 s short of that clock (the PANIC rule), before its own deferred StartTurn; and once the old
// active player reaches its Turn.Started it pauses for the new player's lead, which the spectator can only produce
// after a StartTurn it never reaches. This module sends TimeSync(-1) again on the old active player's machine right
// before its deferred StartTurn runs: the spectator runs free to its own StartTurn, and Turn.Started re-establishes
// the roles as always. The periodic sends during the hold stay as they are (they keep the spectator moving after its
// own Turn.Ended zeroed its lead, as they do through the vanilla post-activity window). Sim state is untouched: a
// time-sync message carries only a clock.
//
// A short hold breaks that (LocalNet, 2026-11-18, the Kindjal Grave Spade's 117-tick hold): a spectator runs about 5 s
// behind, so every periodic TimeSync and the final -1 of a hold shorter than that lag have already arrived when the
// spectator reaches its own Turn.Ended, and that Turn.Ended sets its lead to 0 (0x707bc5 -> 0x7065d7(0)). Lead 0 is a
// PANIC pause in the throttle (0x7082ca), nothing else is coming (the old active machine now waits at its Turn.Started
// for the new player's TimeSync), and the spectator's sim never ticks again: a deadlock with no desync. So on every
// frame during a hold on a machine whose current player is not local, a lead of exactly 0 (only the turn-boundary resets
// write 0) is set to -1 through the engine's own setter 0x7065d7: the value the old active machine's Turn.Ended -1 gave
// it, and the one the engine itself sets when the current player surrenders ("end of game or turn, full speed ahead"
// in 0x7082ca). The spectator then runs on to its own deferred StartTurn, whose Turn.Started resets the lead to 0 and
// re-establishes the roles. A late periodic TimeSync still overwrites the -1 with a finite lead as before; the last
// message of a hold is always the -1 sent below. No sim state is touched: the throttle only paces the clock.
#include <cstdint>

#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "lua/sim/turnhold.h"
#include "melange/sim.h"
#include "net/net.h"

namespace {
constexpr uintptr_t kSendTimeSync = 0x707277;  // __thiscall NetService::SendTimeSync(int32 stamp)
constexpr uintptr_t kSetLead = 0x7065d7;       // __thiscall NetService::SetThrottleLead(int32 lead), ret 4
constexpr uintptr_t kThrottle = 0x4d8;         // NetService -> NetThrottle*
constexpr uintptr_t kThrottleLead = 0x24;      // NetThrottle lead time (ms; -1 end of turn, -2 end of game)
bool g_online = false, g_ok = false, g_okLead = false;
uint32_t g_freedHold = 0;                      // holds+1 of the hold last freed (one log line per hold)

void BeforeDeferredStartTurn(void*) {
    if (!g_online || !g_ok) return;
    const uintptr_t ns = melange::wum::NetService();
    const uintptr_t cur = ns ? melange::wum::CurrentPlayer(ns) : 0;
    uint8_t local = 0;
    if (!cur || !melange::mem::SafeRead(cur + melange::wum::off::PlayerIsLocal, &local, 1) || !local) {
        LOG_INFO("[net] turn hold: release with %s current player; no time-sync sent", cur ? "a remote" : "no");
        return;
    }
    reinterpret_cast<void(__thiscall*)(uintptr_t, int32_t)>(kSendTimeSync)(ns, -1);
    LOG_INFO("[net] turn hold: local player's held turn start is about to run, sent -1 as time sync (others run to "
             "their own turn start)");
}

// Every frame: a spectator whose lead the hold's Turn.Ended zeroed runs on to its own deferred StartTurn.
void FreeSpectatorDuringHold() {
    if (!g_online || !g_okLead) return;
    const melange::sim::TurnHold t = melange::sim::GetTurnHold();
    if (!t.holding) return;
    const uintptr_t ns = melange::wum::NetService();
    if (!ns) return;
    const uintptr_t cur = melange::wum::CurrentPlayer(ns);
    uint8_t local = 0;
    if (cur && melange::mem::SafeRead(cur + melange::wum::off::PlayerIsLocal, &local, 1) && local) return;  // it leads
    uintptr_t thr = 0;
    int32_t lead = -1;
    if (!melange::mem::SafeRead(ns + kThrottle, &thr, 4) || !thr) return;
    if (!melange::mem::SafeRead(thr + kThrottleLead, &lead, 4) || lead != 0) return;
    reinterpret_cast<void(__thiscall*)(uintptr_t, int32_t)>(kSetLead)(ns, -1);
    if (g_freedHold != t.holds + 1) {
        g_freedHold = t.holds + 1;
        LOG_INFO("[net] turn hold: spectator lead was 0 during the hold (deferred at tick %u, %u ticks left); set to -1 "
                 "so it runs on to its own turn start",
                 t.since, t.remaining);
    }
}

class SimTurnHoldNet final : public melange::Module {
public:
    const char* Name() const override { return "SimTurnHoldNet"; }
    const char* Description() const override { return "online: TimeSync(-1) before a held turn start runs, spectators run through a hold"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 55; }
    bool Install() override {
        // push ebp / mov ebp,esp / push ecx / and dword [ebp-4],0 / push edi / mov edi,ecx / call
        g_ok = melange::mem::Expect(kSendTimeSync, {0x55, 0x8b, 0xec, 0x51, 0x83, 0x65, 0xfc, 0x00, 0x57, 0x8b, 0xf9, 0xe8});
        if (!g_ok) {
            LOG_WARN("[net] turn hold: unexpected code at %08x; no time-sync at a held turn start",
                     static_cast<unsigned>(kSendTimeSync));
            return true;
        }
        // mov ecx,[ecx+0x4d8] / test ecx,ecx / je +5 / jmp NetThrottle::SetLead 0x70594b
        g_okLead = melange::mem::Expect(kSetLead, {0x8b, 0x89, 0xd8, 0x04, 0x00, 0x00, 0x85, 0xc9, 0x74, 0x05, 0xe9});
        if (!g_okLead)
            LOG_WARN("[net] turn hold: unexpected code at %08x; a hold shorter than a spectator's lag can stall it",
                     static_cast<unsigned>(kSetLead));
        melange::events::Subscribe(melange::events::Event::MatchStart, [] {
            g_online = true;
            g_freedHold = 0;
        });
        melange::events::Subscribe(melange::events::Event::MatchEnd, [] { g_online = false; });
        melange::events::Subscribe(melange::events::Event::Frame, [] { FreeSpectatorDuringHold(); });
        melange::turnhold::SetBeforeStartTurn(&BeforeDeferredStartTurn, nullptr);
        return true;
    }
};
}  // namespace

MELANGE_MODULE(SimTurnHoldNet);
