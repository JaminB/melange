// Wormsign: the tick clock and the per-tick engine hash; recordings, replays and the desync detector build on it.
//   wormsign.stats         session, tick and hash cost
#include <lua.hpp>
#include <windows.h>

#include <cstdio>

#include "core/config.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/lua.h"
#include "melange/testcmd.h"
#include "melange/wormsign.h"
#include "wormsign/clock.h"
#include "wormsign/library.h"
#include "wormsign/recorder.h"
#include "wormsign/session.h"

namespace ws = melange::wormsign;

namespace {
int LTick(lua_State* L) {
    ws::TickHash h;
    if (!ws::LastTick(&h)) {
        lua_pushnil(L);
        return 1;
    }
    char b[17];
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, h.tick);
    lua_setfield(L, -2, "tick");
    snprintf(b, sizeof b, "%016llx", h.engine);
    lua_pushstring(L, b);
    lua_setfield(L, -2, "engine");
    snprintf(b, sizeof b, "%016llx", h.mods);
    lua_pushstring(L, b);
    lua_setfield(L, -2, "mods");
    return 1;
}

int LLibrary(lua_State* L) {
    ws::ReplayInfo infos[64];
    const int total = ws::Library(infos, 64);
    lua_createtable(L, total > 64 ? 64 : total, 0);
    for (int i = 0; i < total && i < 64; ++i) {
        lua_createtable(L, 0, 8);
        char narrow[520];
        WideCharToMultiByte(CP_UTF8, 0, infos[i].path, -1, narrow, sizeof narrow, nullptr, nullptr);
        lua_pushstring(L, narrow);
        lua_setfield(L, -2, "path");
        lua_pushinteger(L, static_cast<lua_Integer>(infos[i].bytes));
        lua_setfield(L, -2, "bytes");
        lua_pushinteger(L, infos[i].ticks);
        lua_setfield(L, -2, "ticks");
        lua_pushstring(L, infos[i].land);
        lua_setfield(L, -2, "land");
        lua_pushboolean(L, infos[i].online);
        lua_setfield(L, -2, "online");
        lua_pushboolean(L, infos[i].complete);
        lua_setfield(L, -2, "complete");
        lua_pushboolean(L, infos[i].pinned);
        lua_setfield(L, -2, "pinned");
        lua_pushboolean(L, infos[i].flagged);
        lua_setfield(L, -2, "flagged");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

int OpenLib(lua_State* L) {
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, &LTick);
    lua_setfield(L, -2, "tick");
    lua_pushcfunction(L, &LLibrary);
    lua_setfield(L, -2, "library");
    return 1;
}

bool VerbStats(std::string_view, void*) {
    const auto c = ws::session::GetCost();
    ws::TickHash h{};
    const bool have = ws::LastTick(&h);
    LOG_INFO("[wormsign] stats: enabled=%d hooks=%d inMatch=%d serial=%u tick=%u t=%u paused=%d engine=%016llx "
             "fpucw=%04x | hash n=%llu mean=%.2f us p95=%.1f max=%.1f, tick end mean=%.2f us",
             ws::Enabled(), ws::clock::HooksEnabled(), ws::InMatch(), ws::MatchSerial(), ws::Tick(), ws::LogicTimeMs(),
             ws::clock::Paused(), have ? h.engine : 0ULL, have ? h.fpucw : 0, c.ticks, c.hashUsMean, c.hashUsP95,
             c.hashUsMax, c.tickUsMean);
    return true;
}

class Wormsign final : public melange::Module {
  public:
    const char* Name() const override { return "Wormsign"; }
    const char* Description() const override { return "tick clock and per-tick state hashes"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 58; }
    bool Install() override {
        const bool record = Bool("Record", true);
        const bool recordDetail = Bool("RecordDetail", true);
        const int keepMatches = Int("KeepMatches", 20);
        const int maxMB = Int("MaxMB", 200);
        Bool("Exchange", true);
        melange::config::EnsureKey(Name(), "OnDesync", "report");
        Bool("ReplayAnyContent", false);
        melange::testcmd::Register("wormsign.stats", &VerbStats);
        if (!ws::clock::Install()) return true;
        ws::session::SetEnabled(true);
        melange::lua::AddLibrary("wormsign", &OpenLib);
        ws::library::Configure(keepMatches, static_cast<uint32_t>(maxMB > 0 ? maxMB : 200));
        if (ws::recorder::Install()) {
            ws::recorder::SetRecordEnabled(record);
            ws::recorder::SetDetailEnabled(recordDetail);
        } else {
            LOG_ERROR("[wormsign] recorder failed to install: matches will not be recorded this session");
        }
        LOG_INFO("[wormsign] tick clock installed");
        return true;
    }
    void Uninstall() override {
        ws::clock::Uninstall();
        ws::session::SetEnabled(false);
    }
};
}  // namespace

MELANGE_MODULE(Wormsign);
