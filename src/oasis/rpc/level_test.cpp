// level.test: builds a project into the Test workspace (server thread), registers and arms it, then starts Quick Game
// from the main thread once the level is registered at the frontend. An attract demo is ended first with a key tap,
// since a StartGame posted during it asserts. Test states go out on the `erg` channel. levels.live changes map packs.
// A clean-room Test (cleanMods, postfx: false) suppresses the sim mods on the Test level and turns the post-FX stack
// off from the Test's start until it ends, fails or is disarmed; nothing is persisted.
#include <windows.h>

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "erg/names.h"
#include "erg/quickstart.h"
#include "levels/engine.h"
#include "levels/test.h"
#include "lua/sim/bridge_internal.h"
#include "melange/levels.h"
#include "melange/postfx.h"
#include "mods/lobby.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "render/keytap.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

constexpr uint8_t kDikSpace = 0x39;
// Taps go on until the deadline: one asked while the demo is still loading is dropped unread.
constexpr ULONGLONG kStartWaitMs = 15000, kTapEveryMs = 1500;

std::atomic<bool> g_atFrontend{false}, g_inLobby{false}, g_quickOk{false}, g_attract{false}, g_loading{false};
std::mutex g_mx;
std::string g_startKey;
ULONGLONG g_startBy = 0, g_nextTap = 0;
int g_taps = 0;
struct CleanRoom {
    std::string key;
    bool mods = false, postfx = true;
};
CleanRoom g_clean;   // the Test that asked for one; under g_mx

bool OptFlag(const json::Value& v, const char* key, bool* out, Result& r) {
    const json::Value* m = v.Get(key);
    return !m || m->IsNull() || rpc::Flag(v, key, out, r);
}

// Main thread (the Test state events). An Idle without a key is an override that timed out.
void OnTestState(levels::TestState s, const char* key, const char*, void*) {
    using TS = levels::TestState;
    CleanRoom c;
    {
        std::lock_guard lk(g_mx);
        c = g_clean;
    }
    if (c.key.empty() || (key && *key && c.key != key)) return;
    if ((s == TS::Starting || s == TS::Playing) && !c.postfx) postfx::SetSuppressed(true);
    if (s != TS::Ended && s != TS::Failed && s != TS::Idle) return;
    if (!c.postfx) postfx::SetSuppressed(false);
    if (c.mods) simbridge::SetTestCleanRoom("");
    LOG_INFO("[erg] test: the clean room of %s ended (%s)", c.key.c_str(), levels::test::StateName(s));
    std::lock_guard lk(g_mx);
    if (g_clean.key == c.key) g_clean = CleanRoom{};
}

void DeleteShadows(const std::string& stem) {
    namespace fs = std::filesystem;
    const fs::path game(game::GameDir());
    for (const wchar_t* dir : {L"Melange\\erg\\test\\Maps", L"Melange\\cache\\Maps"})
        for (const char* tod : {"DAY", "EVENING", "NIGHT"}) {
            std::error_code ec;
            if (fs::remove(game / dir / game::Widen(stem + tod + ".csh"), ec))
                LOG_INFO("[erg] test: deleted %s%s.csh", stem.c_str(), tod);
        }
}

void OnFrame() {
    const bool front = levels::engine::AtFrontend();
    const levels::engine::FrontendState fs = levels::engine::ReadFrontend();
    const bool attract = fs.valid && fs.attractRunning;
    g_atFrontend = front;
    g_attract = attract;
    g_loading = levels::engine::Loading();
    g_inLobby = handshake::lobby::Current() != 0;
    g_quickOk = front && erg::quickstart::Available();
    std::string key;
    bool demoWon = false;
    {
        std::lock_guard lk(g_mx);
        if (g_startKey.empty()) return;
        const ULONGLONG now = GetTickCount64();
        if (now > g_startBy) {
            demoWon = attract;
            if (!demoWon) LOG_WARN("[erg] test: %s did not start in time; press Quick Game to play it", g_startKey.c_str());
            g_startKey.clear();
            if (g_taps) render::CancelTap();
        } else if (attract) {
            if (now >= g_nextTap) {
                g_nextTap = now + kTapEveryMs;
                ++g_taps;
                const bool tapped = render::TapKey(kDikSpace);
                LOG_INFO("[erg] test: the attract demo is running; ending it with a key tap: %s", tapped ? "sent" : "not sent (an earlier tap is pending, or the keyboard is not hooked)");
            }
            return;
        } else {
            if (g_taps) render::CancelTap();  // the demo is over: a late tap would land on the menu
            if (!g_quickOk || !levels::engine::LevelDetails(g_startKey.c_str(), nullptr)) return;
            key.swap(g_startKey);
        }
    }
    if (demoWon) {
        levels::test::Fail(levels::test::kAttractRefused);
        return;
    }
    if (key.empty()) return;
    char armed[128] = {};
    if (!levels::Armed(armed, sizeof armed) || key != armed) return;
    const bool ok = erg::quickstart::PostQuickGame();
    LOG_INFO("[erg] test: %s registered; Quick Game %s", key.c_str(), ok ? "started" : "not available, press it to play");
}

void Test(const Call& c, Result& r, void*) {
    json::Value p;
    std::string project, todText;
    bool cleanMods = false, postfxOn = true;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "project", &project, r) || !rpc::Str(p, "tod", &todText, r, false) ||
        !OptFlag(p, "cleanMods", &cleanMods, r))
        return;
    // cleanMods turns post-FX off too unless postfx: true says otherwise.
    postfxOn = !cleanMods;
    if (!OptFlag(p, "postfx", &postfxOn, r)) return;
    if (!erg::names::ValidSlug(project)) {
        Fail(r, rpc::kBadParams, "project must match [a-z0-9]{1,24}");
        return;
    }
    levels::Tod tod = levels::Tod::Default;
    if (!levels::test::ParseTod(todText, &tod)) {
        Fail(r, rpc::kBadParams, "tod must be DAY, EVENING or NIGHT");
        return;
    }
    if (g_inLobby) {
        Fail(r, rpc::kRefused, "Test cannot start while you are in a lobby");
        return;
    }
    const bool attract = g_attract;
    if (!g_atFrontend && !attract) {
        Fail(r, rpc::kNotInMatch, "the game must be at the frontend to test a level");
        return;
    }
    if (g_loading && !attract) {
        Fail(r, rpc::kBusy, "the game is loading a level; try again in a moment");
        return;
    }
    std::string stem, title, err;
    if (!BuildTestLevel(project, todText, static_cast<uint64_t>(c.client), &stem, &title, &err)) {
        Fail(r, rpc::kRefused, err);
        return;
    }
    DeleteShadows(stem);
    const std::string key = erg::names::Key(stem);
    char regErr[256] = {};
    if (!levels::RegisterTest(stem.c_str(), title.c_str(), regErr, sizeof regErr)) {
        Fail(r, rpc::kRefused, regErr);
        return;
    }
    if (!levels::ArmNextLevel(key.c_str(), levels::ArmOptions{120, tod})) {
        Fail(r, rpc::kRefused, "could not arm the Test override");
        return;
    }
    {
        std::lock_guard lk(g_mx);
        g_clean = cleanMods || !postfxOn ? CleanRoom{key, cleanMods, postfxOn} : CleanRoom{};
    }
    simbridge::SetTestCleanRoom(cleanMods ? key : "");
    if (postfxOn) postfx::SetSuppressed(false);   // a clean room left over from an earlier Test that never ended
    if (cleanMods || !postfxOn)
        LOG_INFO("[erg] test: clean room for %s: sim mods %s, post-FX %s", key.c_str(), cleanMods ? "suppressed" : "kept",
                 postfxOn ? "kept" : "off");
    const bool quick = g_quickOk || attract;
    if (quick) {
        std::lock_guard lk(g_mx);
        g_startKey = key;
        g_startBy = GetTickCount64() + kStartWaitMs;
        g_nextTap = 0;
        g_taps = 0;
    }
    r.json = jsonmini::Obj().Str("key", key).Str("state", quick ? "starting" : "armed")
                 .Raw("cleanRoom", jsonmini::Obj().Bool("simMods", !cleanMods).Bool("postfx", postfxOn).End())
                 .End();
}

// The clean room in effect (or asked for by the armed Test); its presence also tells a client that level.test takes
// cleanMods and postfx.
void CleanRoomState(const Call&, Result& r, void*) {
    CleanRoom c;
    {
        std::lock_guard lk(g_mx);
        c = g_clean;
    }
    r.json = jsonmini::Obj().Str("key", c.key).Bool("simMods", !c.mods).Bool("postfx", c.postfx)
                 .Bool("postfxOff", postfx::Suppressed()).End();
}

void Live(const Call& c, Result& r, void*) {
    json::Value p;
    std::string mod;
    bool on = false;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "modId", &mod, r) || !rpc::Flag(p, "on", &on, r)) return;
    char err[256] = {};
    const bool ok = on ? levels::EnablePackLive(mod.c_str(), err, sizeof err) : levels::DisablePackLive(mod.c_str(), err, sizeof err);
    r.json = jsonmini::Obj().Bool("ok", ok).Str("reason", ok ? "" : err).End();
}
}  // namespace

void InstallLevelTest() {
    events::Subscribe(events::Event::Frame, &OnFrame);
    levels::OnTestState(&OnTestState, nullptr);
    AddMethod("level.test", &Test, nullptr, kRpcServerThread | kRpcMutating | kRpcGameOnly);
    AddMethod("levels.live", &Live, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.cleanRoom", &CleanRoomState, nullptr, kRpcGameOnly);
}
}  // namespace melange::oasis::providers
