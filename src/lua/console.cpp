// LuaConsole: overlay REPL for the client VM and the match VM (scaffold stub).
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "lua/engine50.h"

namespace {
class LuaConsole final : public melange::Module {
public:
    const char* Name() const override { return "LuaConsole"; }
    const char* Description() const override { return "Lua console in the overlay"; }
    int Order() const override { return 55; }
    bool Install() override {
        if (melange::game::IsKnownBuild() && !melange::lua50::Check())
            LOG_WARN("[console] engine Lua check failed: the Match target stays off");
        return true;
    }
};
}  // namespace

MELANGE_MODULE(LuaConsole);
