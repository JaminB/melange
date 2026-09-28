// Sandbox: the Lua 5.4 client VM for mods (scaffold stub).
#include <lua.hpp>

#include "core/module.h"
#include "lua/sandbox_internal.h"
#include "melange/lua.h"

static_assert(LUA_VERSION_NUM == 504);

namespace melange::lua {
bool Ready() { return false; }
lua_State* ClientState() { return nullptr; }
bool AddLibrary(const char*, OpenFn, uint32_t) { return false; }
const char* CurrentMod() { return nullptr; }
bool PostEvent(const char*, const char*) { return false; }
Stats GetStats() { return {}; }
}  // namespace melange::lua

namespace melange::sandbox {
bool LoadMod(const char*) { return false; }
void UnloadMod(const char*) {}
bool ReloadMod(const char*) { return false; }
EvalOut Eval(const char*, const std::string&) { return {false, "the Sandbox is not available"}; }
void Complete(const char*, const std::string&, std::vector<std::string>*) {}
}  // namespace melange::sandbox

namespace {
class Sandbox final : public melange::Module {
public:
    const char* Name() const override { return "Sandbox"; }
    const char* Description() const override { return "Lua 5.4 scripting for client mods"; }
    int Order() const override { return 50; }
    bool Install() override { return true; }
};
}  // namespace

MELANGE_MODULE(Sandbox);
