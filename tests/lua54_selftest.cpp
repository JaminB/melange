// Offline self-test: the vendored Lua 5.4 is a C++ build (lua_error unwinds C++ frames). Exit code 0 = passed.
#include <cstdio>
#include <cstring>

#include <lua.hpp>

namespace {
int g_destroyed = 0;
struct Guard {
    ~Guard() { ++g_destroyed; }
};

int Throws(lua_State* L) {
    Guard g;
    return luaL_error(L, "boom %d", 7);
}
}  // namespace

int main() {
    int fail = 0;
    lua_State* L = luaL_newstate();
    luaL_openlibs(L);
    if (luaL_dostring(L, "return string.format('%d-%s', 6 * 7, _VERSION)") != LUA_OK ||
        std::strcmp(lua_tostring(L, -1), "42-Lua 5.4") != 0) {
        printf("FAIL: eval\n");
        ++fail;
    }
    lua_settop(L, 0);
    lua_pushcfunction(L, &Throws);
    const int rc = lua_pcall(L, 0, 0, 0);
    if (rc != LUA_ERRRUN || !std::strstr(lua_tostring(L, -1), "boom 7") || g_destroyed != 1) {
        printf("FAIL: error unwinding (rc=%d destroyed=%d)\n", rc, g_destroyed);
        ++fail;
    }
    if (luaL_dostring(L, "return math.maxinteger // 2 + 1 == 1 << 62") != LUA_OK || !lua_toboolean(L, -1)) {
        printf("FAIL: integers\n");
        ++fail;
    }
    lua_close(L);
    printf("lua54_selftest: %s\n", fail ? "FAILED" : "passed");
    return fail;
}
