// level.test: builds a project into the Test workspace (server thread), registers and arms it, then starts Quick Game
// from the main thread once the level is registered at the frontend. Test states go out on the `erg` channel.
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
#include "melange/levels.h"
#include "mods/lobby.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

std::atomic<bool> g_atFrontend{false}, g_inLobby{false}, g_quickOk{false};
std::mutex g_mx;
std::string g_startKey;
ULONGLONG g_startBy = 0;

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
    g_atFrontend = front;
    g_inLobby = handshake::lobby::Current() != 0;
    g_quickOk = front && erg::quickstart::Available();
    std::string key;
    {
        std::lock_guard lk(g_mx);
        if (g_startKey.empty()) return;
        if (GetTickCount64() > g_startBy) {
            LOG_WARN("[erg] test: %s was not registered in time; press Quick Game to play it", g_startKey.c_str());
            g_startKey.clear();
            return;
        }
        if (!front || !levels::engine::LevelDetails(g_startKey.c_str(), nullptr)) return;
        key.swap(g_startKey);
    }
    char armed[128] = {};
    if (!levels::Armed(armed, sizeof armed) || key != armed) return;
    const bool ok = erg::quickstart::PostQuickGame();
    LOG_INFO("[erg] test: %s registered; Quick Game %s", key.c_str(), ok ? "started" : "not available, press it to play");
}

void Test(const Call& c, Result& r, void*) {
    json::Value p;
    std::string project;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "project", &project, r)) return;
    if (!erg::names::ValidSlug(project)) {
        Fail(r, rpc::kBadParams, "project must match [a-z0-9]{1,24}");
        return;
    }
    if (g_inLobby) {
        Fail(r, rpc::kRefused, "Test cannot start while you are in a lobby");
        return;
    }
    if (!g_atFrontend) {
        Fail(r, rpc::kNotInMatch, "the game must be at the frontend to test a level");
        return;
    }
    std::string stem, title, err;
    if (!BuildTestLevel(project, &stem, &title, &err)) {
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
    if (!levels::ArmNextLevel(key.c_str(), 120)) {
        Fail(r, rpc::kRefused, "could not arm the Test override");
        return;
    }
    const bool quick = g_quickOk;
    if (quick) {
        std::lock_guard lk(g_mx);
        g_startKey = key;
        g_startBy = GetTickCount64() + 10000;
    }
    r.json = jsonmini::Obj().Str("key", key).Str("state", quick ? "starting" : "armed").End();
}
}  // namespace

void InstallLevelTest() {
    events::Subscribe(events::Event::Frame, &OnFrame);
    AddMethod("level.test", &Test, nullptr, kRpcServerThread | kRpcMutating | kRpcGameOnly);
}
}  // namespace melange::oasis::providers
