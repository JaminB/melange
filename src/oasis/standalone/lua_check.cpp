#include "oasis/standalone/lua_check.h"

#include <cstdlib>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace melange::oasis::standalone {
bool CompileLua50(const std::string& text, int* line, std::string* message) {
    lua_State* L = lua_open();
    if (!L) return true;
    const int rc = luaL_loadbuffer(L, text.data(), text.size(), "=script.lua");
    bool ok = rc != LUA_ERRSYNTAX;
    if (!ok) {
        const char* m = lua_tostring(L, -1);
        std::string msg = m ? m : "syntax error";
        int at = 0;
        // "script.lua:12: '=' expected near 'x'"
        const std::string prefix = "script.lua:";
        if (msg.rfind(prefix, 0) == 0) {
            const size_t colon = msg.find(':', prefix.size());
            if (colon != std::string::npos) {
                at = std::atoi(msg.substr(prefix.size(), colon - prefix.size()).c_str());
                msg = msg.substr(colon + 1);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            }
        }
        if (line) *line = at;
        if (message) *message = msg;
    }
    lua_close(L);
    return ok;
}
}  // namespace melange::oasis::standalone
