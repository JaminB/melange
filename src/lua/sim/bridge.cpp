// SimBridge: sim mods in the engine's Lua 5.0.1 match VM (scaffold stub).
#include "core/log.h"
#include "core/module.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "melange/sim.h"

namespace melange::sim {
bool InMatch() { return false; }
uint32_t MatchSerial() { return 0; }
uint32_t Tick() { return 0; }
bool ModsActive() { return false; }
int AddTickHook(TickFn, void*, int) { return 0; }
void RemoveTickHook(int) {}
uint32_t Random(uint32_t) { return 0; }
SendResult Send(const char*) { return SendResult::NotInMatch; }
SendResult SendInt(const char*, int32_t) { return SendResult::NotInMatch; }
SendResult SendFloat(const char*, float) { return SendResult::NotInMatch; }
SendResult SendString(const char*, const char*) { return SendResult::NotInMatch; }
bool RegisterModMessage(const char*, uint16_t*) { return false; }
void FreezeModMessages() {}
Stats GetStats() { return {}; }
}  // namespace melange::sim

namespace melange::simbridge {
void SetModList(const std::vector<std::string>&) {}
void SetGate(Gate) {}
sandbox::EvalOut EvalMatch(const std::string&) { return {false, "the sim bridge is not available"}; }
void CompleteMatch(const std::string&, std::vector<std::string>*) {}
int OnMatch(MatchFn, void*) { return 0; }
}  // namespace melange::simbridge

namespace {
class SimBridge final : public melange::Module {
public:
    const char* Name() const override { return "SimBridge"; }
    const char* Description() const override { return "sim mods in the match's Lua VM"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 52; }
    bool Install() override {
        if (!melange::lua50::Check()) LOG_WARN("[sim] engine Lua check failed: SimBridge stays inert");
        return true;
    }
};
}  // namespace

MELANGE_MODULE(SimBridge);
