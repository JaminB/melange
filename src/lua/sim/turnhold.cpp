// SimTurnHold: wum.sim.holdTurn / releaseTurn / turnHold for sim mods, and the client-side notifications of a hold.
// The logic is in turnhold_core.cpp (shared with the offline sim self-test).
#include "lua/sim/turnhold.h"

#include <cstdio>

#include "core/log.h"
#include "core/module.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "melange/jlog.h"
#include "melange/lua.h"
#include "melange/wormsign.h"

namespace {
void Notify(const melange::turnhold::Event& e, void*) {
    char json[128];
    snprintf(json, sizeof json, "{\"ticks\":%u,\"tick\":%u,\"roundMsLeft\":%d}", e.ticks, e.tick, e.roundMsLeft);
    melange::lua::PostEvent(e.begin ? "melange.turnHold" : "melange.turnHoldEnd", json);
    melange::jlog::Rec("sim", melange::jlog::Level::Info, e.begin ? "turn hold" : "turn hold end")
        .Uint("ticks", e.ticks).Uint("tick", e.tick).Int("roundMsLeft", e.roundMsLeft);
}

int g_contrib = 0;

void Contrib(melange::wormsign::Hasher& h, uint32_t, void*) {
    uint8_t b[melange::turnhold::kHashBytes];
    h.Bytes(b, melange::turnhold::HashBytes(b, sizeof b));
}

class SimTurnHold final : public melange::Module {
public:
    const char* Name() const override { return "SimTurnHold"; }
    const char* Description() const override { return "deterministic turn-start hold for sim mods (wum.sim.holdTurn)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 54; }
    bool Install() override {
        if (!melange::lua50::Check()) {
            LOG_WARN("[sim] engine Lua check failed: SimTurnHold stays inert");
            return true;
        }
        using namespace melange::turnhold;
        // No Init() here: holdTurn adds the tick hook in the match that needs it, so vanilla matches (and online
        // matches against peers without sim mods) keep the sim bridge's hooks as they were before 0.11.
        SetNotify(&Notify, nullptr);
        bool ok = melange::simbridge::AddSimFunction("holdTurn", &LHoldTurn);
        ok = melange::simbridge::AddSimFunction("releaseTurn", &LReleaseTurn) && ok;
        ok = melange::simbridge::AddSimFunction("turnHold", &LTurnHold) && ok;
        if (!ok) LOG_WARN("[sim] could not add wum.sim.holdTurn/releaseTurn/turnHold");
        // The hold decides the tick the deferred StartTurn runs, so a diverging hold is named by its own
        // contributor instead of only by its effects at the release tick.
        g_contrib = melange::wormsign::AddContributor("melange.turnhold", &Contrib, nullptr);
        return true;
    }
    void Uninstall() override {
        using namespace melange::turnhold;
        if (g_contrib) melange::wormsign::RemoveContributor(g_contrib);
        g_contrib = 0;
        SetNotify(nullptr, nullptr);
        Shutdown();
    }
};
}  // namespace

MELANGE_MODULE(SimTurnHold);
