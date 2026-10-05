// Wormsign: the tick clock and the per-tick engine hash; recordings, replays and the desync detector build on it.
//   wormsign.stats                 session, tick and hash cost
//   wormsign.contrib               hash contributors, their state and the mod environment digest cost
//   wormsign.detail [tick]         the detail record of a tick (default: the last) and its diff against the tick before
//   wormsign.fpu                   the FPU watch
//   wormsign.peers                 the hash exchange with each lobby member
//   wormsign.replay ...            the replay player (player.cpp)
#include <lua.hpp>
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "core/config.h"
#include "core/log.h"
#include "core/module.h"
#include "lua/sim/sim_hash.h"
#include "melange/lua.h"
#include "melange/testcmd.h"
#include "melange/wormsign.h"
#include "tools/json_mini.h"
#include "wormsign/clock.h"
#include "wormsign/contrib.h"
#include "wormsign/detail.h"
#include "wormsign/detector.h"
#include "wormsign/fpu.h"
#include "wormsign/hash_engine.h"
#include "wormsign/library.h"
#include "wormsign/recorder.h"
#include "wormsign/player.h"
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

bool VerbContrib(std::string_view, void*) {
    ws::contrib::Info list[64];
    const size_t n = ws::contrib::List(list, 64);
    LOG_INFO("[wormsign] %zu contributors, list hash %016llx", n, ws::contrib::ListHash());
    for (size_t i = 0; i < n; ++i)
        LOG_INFO("[wormsign]   %s v%u%s: %s, %llu calls, last p95 %u.%u us", list[i].name, list[i].version,
                 list[i].inReplayCompare ? "" : " (not in replay compare)", ws::contrib::Describe(list[i]).c_str(),
                 list[i].calls, list[i].lastP95Us10 / 10, list[i].lastP95Us10 % 10);
    const auto c = melange::simhash::GetCost();
    LOG_INFO("[wormsign] mod environment digests: %llu, %llu values, p50 %u.%u us, p95 %u.%u us, max %u.%u us",
             c.digests, c.entries, c.p50Us10 / 10, c.p50Us10 % 10, c.p95Us10 / 10, c.p95Us10 % 10, c.maxUs10 / 10,
             c.maxUs10 % 10);
    return true;
}

bool VerbDetail(std::string_view a, void*) {
    const std::string s(a);
    const uint32_t tick = s.empty() ? ws::Tick() : static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 10));
    ws::detail::DetailRec cur, prev;
    if (!ws::detail::Get(tick, &cur)) {
        LOG_INFO("[wormsign] detail: tick %u is not in the ring", tick);
        return true;
    }
    melange::log::WriteRaw(("[wormsign] detail " + ws::detail::ToJson(cur) + "\r\n").c_str());
    ws::TickHash th{};
    if (ws::TickAt(tick, &th)) {
        uint64_t c[ws::kEngineComps] = {};
        const uint8_t mask = ws::detail::Recompute(cur, c);
        std::string bad;
        for (int i = 0; i < ws::kEngineComps; ++i)
            if ((mask >> i & 1) && c[i] != th.c[i]) bad += " c" + std::to_string(i);
        LOG_INFO("[wormsign] detail %u against its tick hash: %s", tick, bad.empty() ? "c0 c1 c2 c4 c5 match" : bad.c_str());
    }
    ws::contrib::Entry ce[128];
    const size_t ne = ws::contrib::HashesAt(tick, ce, 128);
    for (size_t i = 0; i < ne; ++i)
        if (strcmp(ce[i].name, ws::kCameraContrib) == 0 && ce[i].computed)
            LOG_INFO("[wormsign] detail %u camera against the %s hash: %s", tick, ws::kCameraContrib,
                     ce[i].hash == ws::detail::CameraHash(cur.cam) ? "match" : "differs");
    if (tick && ws::detail::Get(tick - 1, &prev)) {
        const std::string d = ws::detail::Diff(prev, cur);
        melange::log::WriteRaw(("[wormsign] diff " + std::to_string(tick - 1) + " -> " + std::to_string(tick) +
                                ":\r\n" + (d.empty() ? "(none)\n" : d))
                                   .c_str());
    }
    melange::simhash::EnvChange ch[32];
    const size_t k = melange::simhash::EnvChanges(tick, tick, ch, 32);
    for (size_t i = 0; i < k; ++i) LOG_INFO("[wormsign]   %s", melange::simhash::Format(ch[i]).c_str());
    return true;
}

void ContribNames(std::vector<ws::wire::ContribName>* out) {
    ws::contrib::Info list[128];
    const size_t n = ws::contrib::List(list, 128);
    for (size_t i = 0; i < n; ++i) out->push_back({list[i].name, list[i].version});
}

bool ContribHashes(uint32_t tick, std::vector<uint64_t>* out) {
    ws::contrib::Entry e[128];
    const size_t n = ws::contrib::HashesAt(tick, e, 128);
    for (size_t i = 0; i < n; ++i) out->push_back(e[i].hash);
    return n > 0;
}

bool DetailJson(uint32_t tick, std::string* json) {
    ws::detail::DetailRec r;
    if (!ws::detail::Get(tick, &r)) return false;
    // The exchanged record leaves out the second RNG (never simulation state, it differs between machines) and adds
    // the mod globals and storage keys that changed in this tick, so a mod desync names its key.
    std::string j = ws::detail::ToJson(r);
    if (const size_t p = j.find("\"rng2\":"); p != std::string::npos) {
        const size_t e = j.find(',', p);
        if (e != std::string::npos) j.erase(p, e - p + 1);
    }
    melange::simhash::EnvChange ch[16];
    const size_t n = melange::simhash::EnvChanges(tick, tick, ch, 16);
    if (n && !j.empty() && j.back() == '}') {
        melange::jsonmini::Obj env;
        for (size_t i = 0; i < n; ++i) {
            const std::string t = melange::simhash::Format(ch[i]);
            const size_t c = t.rfind(": ");
            env.Str(t.substr(0, c), c == std::string::npos ? "" : t.substr(c + 2));
        }
        j.pop_back();
        j += ",\"env\":" + env.End() + "}";
    }
    *json = std::move(j);
    return true;
}

bool VerbFpu(std::string_view, void*) {
    LOG_INFO("[wormsign] %s", ws::fpu::NoteJson().c_str());
    return true;
}

class Wormsign final : public melange::Module {
  public:
    const char* Name() const override { return "Wormsign"; }
    const char* Description() const override { return "tick clock, per-tick state hashes and match replays"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 58; }
    bool Install() override {
        const bool record = Bool("Record", true);
        const bool recordDetail = Bool("RecordDetail", true);
        const int keepMatches = Int("KeepMatches", 20);
        const int maxMB = Int("MaxMB", 200);
        ws::detector::Options det;
        det.exchange = Bool("Exchange", true);
        melange::config::EnsureKey(Name(), "OnDesync", "report");
        const std::string onDesync = melange::config::GetString(Name(), "OnDesync", "report");
        det.onDesync = onDesync == "bundle-only" ? ws::detector::OnDesync::BundleOnly : ws::detector::OnDesync::Report;
        if (onDesync != "report" && onDesync != "bundle-only")
            LOG_WARN("[wormsign] OnDesync=%s is not report or bundle-only; using report", onDesync.c_str());
        Bool("ReplayAnyContent", false);
        melange::config::EnsureKey(Name(), "EnvDigest", "changed");
        const auto mode = melange::simhash::ParseMode(melange::config::GetString(Name(), "EnvDigest", "changed").c_str());
        melange::testcmd::Register("wormsign.stats", &VerbStats);
        melange::testcmd::Register("wormsign.contrib", &VerbContrib);
        melange::testcmd::Register("wormsign.detail", &VerbDetail);
        melange::testcmd::Register("wormsign.fpu", &VerbFpu);
        // Opt-in until two peers are seen to agree on it through whole matches: the engine checks the logical camera
        // only at turn ends and time syncs, so whether it also agrees between those is not yet verified live.
        const bool hashCamera = Bool("HashCamera", false);
        if (!ws::clock::Install()) return true;
        ws::session::SetEnabled(true);
        if (hashCamera && !ws::AddCameraContributor())
            LOG_ERROR("[wormsign] the %s contributor could not be added", ws::kCameraContrib);
        melange::simhash::Install(mode);
        melange::lua::AddLibrary("wormsign", &OpenLib);
        ws::library::Configure(keepMatches, static_cast<uint32_t>(maxMB > 0 ? maxMB : 200));
        if (ws::recorder::Install()) {
            ws::recorder::SetRecordEnabled(record);
            ws::recorder::SetDetailEnabled(recordDetail);
        } else {
            LOG_ERROR("[wormsign] recorder failed to install: matches will not be recorded this session");
        }
        LOG_INFO("[wormsign] tick clock installed (mod environment digest: %s)", melange::simhash::ModeName(mode));
        ws::detector::SetContribSource(&ContribNames, &ContribHashes);
        ws::detector::SetDetailSource(&DetailJson);
        ws::detector::SetRecordingSource(&ws::recorder::RecordingPath);
        ws::detector::Install(det);
        ws::player::Install();
        return true;
    }
    void Uninstall() override {
        ws::player::Uninstall();
        ws::detector::Uninstall();
        melange::simhash::Uninstall();
        ws::RemoveCameraContributor();
        ws::clock::Uninstall();
        ws::session::SetEnabled(false);
    }
};
}  // namespace

MELANGE_MODULE(Wormsign);
