// lua.eval and lua.complete: the overlay console's powers over the WebSocket, under the console's rules.
#include <string>
#include <vector>

#include "core/log.h"
#include "lua/console_policy.h"
#include "lua/console_repl.h"
#include "lua/sandbox_internal.h"
#include "lua/sim/bridge_internal.h"
#include "melange/mods.h"
#include "melange/sim.h"
#include "oasis/providers.h"
#include "oasis/rpc/lua_request.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

bool Allowed(const rpc::LuaRequest& q, Result& r) {
    if (q.target == rpc::LuaTarget::Match) {
        if (!sim::InMatch()) return Fail(r, rpc::kNotInMatch, "not in a match");
        std::string why;
        if (!console::MatchAllowed(&why)) return Fail(r, rpc::kRefused, why);
    } else if (q.target == rpc::LuaTarget::Mod) {
        mods::ModInfo m{};
        if (!mods::Find(q.mod.c_str(), &m)) return Fail(r, rpc::kBadParams, "no mod '" + q.mod + "'");
        if (m.state != mods::State::Enabled || !m.hasClient)
            return Fail(r, rpc::kRefused, "mod '" + q.mod + "' is not an enabled mod with client code");
    }
    return true;
}

bool Request(const Call& c, Result& r, const char* key, size_t max, rpc::LuaRequest* q) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return false;
    std::string why;
    if (!rpc::ParseLua(p, key, max, q, &why)) return Fail(r, rpc::kBadParams, why);
    return Allowed(*q, r);
}

const char* Label(const rpc::LuaRequest& q) {
    return q.target == rpc::LuaTarget::Match ? "match" : q.target == rpc::LuaTarget::Mod ? q.mod.c_str() : "client";
}

void Eval(const Call& c, Result& r, void*) {
    rpc::LuaRequest q;
    if (!Request(c, r, "code", rpc::kMaxCode, &q)) return;
    const std::string code = console::ExpandShorthand(q.text);
    LOG_INFO("[oasis] lua.eval client %d %s> %.200s%s", c.client, Label(q), code.c_str(), code.size() > 200 ? "..." : "");
    const sandbox::EvalOut out = q.target == rpc::LuaTarget::Match
                                     ? simbridge::EvalMatch(code)
                                     : sandbox::Eval(q.target == rpc::LuaTarget::Mod ? q.mod.c_str() : nullptr, code);
    r.json = jsonmini::Obj().Bool("ok", out.ok).Str("text", rpc::ClipText(out.text)).End();
}

void Complete(const Call& c, Result& r, void*) {
    rpc::LuaRequest q;
    if (!Request(c, r, "prefix", rpc::kMaxPrefix, &q)) return;
    std::vector<std::string> cands;
    if (q.target == rpc::LuaTarget::Match) simbridge::CompleteMatch(q.text, &cands);
    else sandbox::Complete(q.target == rpc::LuaTarget::Mod ? q.mod.c_str() : nullptr, q.text, &cands);
    jsonmini::Arr a;
    for (size_t i = 0; i < cands.size() && i < 500; ++i) a.Str(cands[i]);
    r.json = a.End();
}
}  // namespace

void InstallLua() {
    AddMethod("lua.eval", &Eval, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("lua.complete", &Complete, nullptr, kRpcGameOnly);
}
}  // namespace melange::oasis::providers
