// wum.web: channels and methods a client mod exposes to Oasis (mod.<id>.<x>), and a sandboxed web panel.
#include <algorithm>
#include <map>
#include <memory>
#include <string>

#include "lua/sandbox_core.h"
#include "melange/oasis.h"
#include "oasis/webpanels.h"

namespace melange::sandbox {
namespace {
namespace oasis = melange::oasis;

ModRec* NeedMod(lua_State* L) {
    ModRec* m = Current();
    if (!m) luaL_error(L, "this function needs a mod context (not available in the console environment)");
    return m;
}

// ---------------------------------------------------------------- wum.web.channel
int ChannelPublish(lua_State* L) {
    const auto ch = static_cast<oasis::ChannelId>(lua_tointeger(L, lua_upvalueindex(1)));
    luaL_checktype(L, 2, LUA_TTABLE);
    if (!oasis::HasSubscribers(ch)) return 0;
    json::Value v;
    std::string err;
    if (!ToJson(L, 2, &v, &err)) return luaL_error(L, "wum.web.channel:publish: %s", err.c_str());
    oasis::Publish(ch, WriteJson(v));
    return 0;
}

int ChannelSubscribers(lua_State* L) {
    const auto ch = static_cast<oasis::ChannelId>(lua_tointeger(L, lua_upvalueindex(1)));
    lua_pushinteger(L, static_cast<lua_Integer>(oasis::SubscriberCount(ch)));
    return 1;
}

int WebChannel(lua_State* L) {
    ModRec* m = NeedMod(L);
    Gen* g = CurrentGen();
    if (!g) return luaL_error(L, "wum.web.channel needs a mod context");
    const std::string name = luaL_checkstring(L, 1);
    const std::string full = "mod." + m->id + "." + name;
    const oasis::ChannelId ch = oasis::AddChannel(full.c_str());
    if (!ch) return luaL_error(L, "wum.web.channel: '%s' is invalid or already exists", full.c_str());
    g->cleanups.push_back([ch] { oasis::RemoveChannel(ch); });
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, static_cast<lua_Integer>(ch));
    lua_pushcclosure(L, &ChannelPublish, 1);
    lua_setfield(L, -2, "publish");
    lua_pushinteger(L, static_cast<lua_Integer>(ch));
    lua_pushcclosure(L, &ChannelSubscribers, 1);
    lua_setfield(L, -2, "subscribers");
    return 1;
}

// ---------------------------------------------------------------- wum.web.method
struct MethodBox { uint32_t cbId = 0; };

void MethodTramp(const oasis::Call& c, oasis::Result& r, void* user) {
    auto* box = static_cast<MethodBox*>(user);
    Callback* cb = FindCallback(box->cbId);
    if (!CallbackLive(cb)) {
        r.ok = false;
        r.code = -32004;
        r.message = "mod method no longer available";
        return;
    }
    json::Value params;
    json::Error e;
    if (!json::Parse(c.paramsJson.empty() ? std::string_view("{}") : c.paramsJson, &params, &e)) {
        r.ok = false;
        r.code = -32602;
        r.message = "bad params";
        return;
    }
    json::Value result;
    bool got = false;
    std::string convErr;
    const bool ran = Invoke(
        cb, [&](lua_State* L) { PushJson(L, params); return 1; }, 1,
        [&](lua_State* L) { got = ToJson(L, -1, &result, &convErr); });
    if (!ran) {
        r.ok = false;
        r.code = -32004;
        r.message = "mod method faulted";
        return;
    }
    if (!got) {
        r.ok = false;
        r.code = -32602;
        r.message = "bad result: " + convErr;
        return;
    }
    r.json = WriteJson(result);
}

int WebMethod(lua_State* L) {
    ModRec* m = NeedMod(L);
    const std::string name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    const std::string full = "mod." + m->id + "." + name;
    Callback* cb = NewCallback(L, 2, CbKind::WebMethod, name);
    const uint32_t cid = cb->id;
    auto handle = std::make_shared<int>(0);
    auto box = std::make_shared<MethodBox>();
    box->cbId = cid;
    cb->attach = [full, handle, box] {
        *handle = oasis::AddMethod(full.c_str(), &MethodTramp, box.get(), oasis::kRpcNone);
        return *handle != 0;
    };
    cb->revoke = [handle] { oasis::RemoveMethod(*handle); };
    if (!Activate(cb)) return luaL_error(L, "wum.web.method: '%s' is invalid or already exists", full.c_str());
    lua_pushinteger(L, cid);
    return 1;
}

// ---------------------------------------------------------------- wum.web.panel
std::map<Gen*, int> g_panelOf;  // one panel per mod generation; erased by the cleanup below

int WebPanel(lua_State* L) {
    ModRec* m = NeedMod(L);
    Gen* g = CurrentGen();
    if (!g) return luaL_error(L, "wum.web.panel needs a mod context");
    luaL_checktype(L, 1, LUA_TTABLE);
    if (g_panelOf.count(g)) return luaL_error(L, "wum.web.panel: this mod already has a panel");
    lua_getfield(L, 1, "title");
    const std::string title = lua_isstring(L, -1) ? lua_tostring(L, -1) : m->name;
    lua_pop(L, 1);
    lua_getfield(L, 1, "entry");
    std::string entry = lua_isstring(L, -1) ? lua_tostring(L, -1) : "web/index.html";
    lua_pop(L, 1);
    if (!SafeRelPath(entry)) return luaL_error(L, "wum.web.panel: entry must be a relative path inside the mod folder");
    std::replace(entry.begin(), entry.end(), '/', '\\');
    const size_t slash = entry.find_last_of('\\');
    const std::wstring dir = slash == std::string::npos ? m->dir : m->dir + L"\\" + Widen(entry.substr(0, slash));
    const std::string file = slash == std::string::npos ? entry : entry.substr(slash + 1);
    const int handle = oasis::providers::AddPanel(m->id.c_str(), title.c_str(), dir.c_str(), file.c_str());
    if (!handle) return luaL_error(L, "wum.web.panel: could not register (id too long, or already registered)");
    g_panelOf[g] = handle;
    g->cleanups.push_back([g, handle] {
        oasis::providers::RemovePanel(handle);
        g_panelOf.erase(g);
    });
    return 0;
}

void Shared(lua_State* L, int wum) {
    static const luaL_Reg kWeb[] = {{"channel", WebChannel}, {"method", WebMethod}, {"panel", WebPanel}, {nullptr, nullptr}};
    lua_newtable(L);
    RegisterFunctions(L, -1, kWeb);
    lua_setfield(L, wum, "web");
}

const LibRegistrar g_reg(&Shared, nullptr);
}  // namespace
}  // namespace melange::sandbox
