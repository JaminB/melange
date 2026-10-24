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
bool g_online = false, g_ok = false;

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

class SimTurnHoldNet final : public melange::Module {
public:
    const char* Name() const override { return "SimTurnHoldNet"; }
    const char* Description() const override { return "online: TimeSync(-1) before a held turn start runs"; }
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
        melange::events::Subscribe(melange::events::Event::MatchStart, [] { g_online = true; });
        melange::events::Subscribe(melange::events::Event::MatchEnd, [] { g_online = false; });
        melange::turnhold::SetBeforeStartTurn(&BeforeDeferredStartTurn, nullptr);
        return true;
    }
};
}  // namespace

MELANGE_MODULE(SimTurnHoldNet);
