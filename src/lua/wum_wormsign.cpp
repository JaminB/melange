// wum.wormsign.onDivergence(fn): fn(payload, name) for every "wormsign.divergence" event.
#include "lua/sandbox_core.h"

namespace melange::sandbox {
int WormsignOnDivergence(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    Callback* cb = NewCallback(L, 1, CbKind::Event, "wormsign.divergence");
    AddListener("wormsign.divergence", cb->id);
    Activate(cb);
    lua_pushinteger(L, cb->id);
    return 1;
}
}  // namespace melange::sandbox
