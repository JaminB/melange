// SimTurnHoldNet: keeps an online match flowing through a turn-start hold (wum.sim.holdTurn).
//
// At a turn end the active player's machine sends TimeSync(-1) (NetService 0x707bc5): the spectators then run freely
// until their own Turn.Started, where the throttle pauses them until the new active player leads by 3 s. In the
// vanilla game nothing is sent between that -1 and Turn.Started, because StartTurn follows at once. A hold opens a
// window there, and the periodic time-sync sender (NetService 0x707155, every 10 in-game ticks while the current
// player is local and ActiveWormIndex is set, called from the InGame update 0x709e5f) keeps stamping the old active
// player's clock into the spectators' lead. A spectator then pauses 2 s short of that clock (the PANIC rule), never
// reaches its own deferred StartTurn, and the old active player, already paused at its Turn.Started waiting for the
// new player's lead, waits forever: a mutual pause (seen on LocalNet 2026-10-24, C stuck at t=67960 under D's
// t=70080). This module skips the periodic sender while a hold is active, so the window looks like the vanilla gap:
// the spectators run freely on the -1 lead to their own StartTurn, and the roles are re-established at Turn.Started
// as always. The sim state is untouched (the sender only carries clocks), and nothing is skipped without a hold.
#include <safetyhook.hpp>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/sim.h"

namespace {
constexpr uintptr_t kPeriodicTimeSync = 0x707155;  // __thiscall NetService::SendPeriodicTimeSync(), no arguments
SafetyHookInline g_hook;
uint32_t g_skippedSince = 0;   // tick of the hold whose sends were skipped (for one log line per hold)
uint32_t g_skipped = 0;

void __fastcall HkPeriodicTimeSync(uintptr_t ns, void* /*edx*/) {
    const auto h = melange::sim::GetTurnHold();
    if (h.holding) {
        if (g_skippedSince != h.since) {
            g_skippedSince = h.since;
            g_skipped = 0;
            LOG_INFO("[net] turn hold: periodic time-sync sends skipped while the turn start is held (hold from tick %u)",
                     h.since);
        }
        ++g_skipped;
        return;
    }
    g_hook.thiscall<void>(ns);
}

class SimTurnHoldNet final : public melange::Module {
public:
    const char* Name() const override { return "SimTurnHoldNet"; }
    const char* Description() const override { return "no periodic time-sync sends while a turn-start hold runs"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 55; }
    bool Install() override {
        // push ebp / mov ebp,esp / sub esp,0x10 / and dword [ebp-4],0
        if (!melange::mem::Expect(kPeriodicTimeSync, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x10, 0x83, 0x65, 0xfc, 0x00})) {
            LOG_WARN("[net] turn hold: unexpected code at %08x; periodic time-sync sends are not gated",
                     static_cast<unsigned>(kPeriodicTimeSync));
            return true;
        }
        g_hook = safetyhook::create_inline(kPeriodicTimeSync, &HkPeriodicTimeSync);
        if (!g_hook) LOG_ERROR("[net] turn hold: hooking the periodic time-sync sender failed");
        return true;
    }
    void Uninstall() override { g_hook = {}; }
};
}  // namespace

MELANGE_MODULE(SimTurnHoldNet);
