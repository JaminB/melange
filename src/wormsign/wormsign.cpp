// Wormsign: the tick clock and the per-tick engine hash; recordings, replays and the desync detector build on it.
//   wormsign.stats         session, tick and hash cost
//   wormsign.peers         the hash exchange with each lobby member
#include <lua.hpp>

#include <cstdio>

#include "core/config.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/lua.h"
#include "melange/testcmd.h"
#include "melange/wormsign.h"
#include "wormsign/clock.h"
#include "wormsign/detector.h"
#include "wormsign/session.h"

namespace ws = melange::wormsign;
namespace melange::sandbox {
int WormsignOnDivergence(lua_State* L);
}

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

int OpenLib(lua_State* L) {
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, &LTick);
    lua_setfield(L, -2, "tick");
    lua_pushcfunction(L, &melange::sandbox::WormsignOnDivergence);
    lua_setfield(L, -2, "onDivergence");
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
        Bool("Record", true);
        Bool("RecordDetail", true);
        Int("KeepMatches", 20);
        Int("MaxMB", 200);
        ws::detector::Options det;
        det.exchange = Bool("Exchange", true);
        melange::config::EnsureKey(Name(), "OnDesync", "report");
        const std::string onDesync = melange::config::GetString(Name(), "OnDesync", "report");
        det.onDesync = onDesync == "bundle-only" ? ws::detector::OnDesync::BundleOnly : ws::detector::OnDesync::Report;
        if (onDesync != "report" && onDesync != "bundle-only")
            LOG_WARN("[wormsign] OnDesync=%s is not report or bundle-only; using report", onDesync.c_str());
        Bool("ReplayAnyContent", false);
        melange::testcmd::Register("wormsign.stats", &VerbStats);
        if (!ws::clock::Install()) return true;
        ws::session::SetEnabled(true);
        melange::lua::AddLibrary("wormsign", &OpenLib);
        LOG_INFO("[wormsign] tick clock installed");
        ws::detector::Install(det);
        return true;
    }
    void Uninstall() override {
        ws::detector::Uninstall();
        ws::clock::Uninstall();
        ws::session::SetEnabled(false);
    }
};
}  // namespace

MELANGE_MODULE(Wormsign);
