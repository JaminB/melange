// The Store inside the game: [Store] settings, Thumper for the installed mods, the match session as the gate, and a
// main-thread queue for every Thumper call the worker makes.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "levels/live.h"
#include "levels/session.h"
#include "melange/levels.h"
#include "melange/mods.h"
#include "mods/thumper_internal.h"
#include "store/install.h"
#include "store/store.h"
#include "version.h"

namespace melange::store {
namespace {
std::mutex g_mainMx;
std::vector<std::function<void()>> g_mainQueue;
std::mutex g_dropMx;
std::vector<std::string> g_dropState;

const char* StateName(mods::State s) {
    switch (s) {
        case mods::State::Enabled: return "enabled";
        case mods::State::Disabled: return "disabled";
        case mods::State::Blocked: return "blocked";
        case mods::State::PendingConsent: return "pending-consent";
        case mods::State::Incompatible: return "incompatible";
        case mods::State::RestartRequired: return "restart-required";
    }
    return "unknown";
}

std::wstring ModsDirFromIni() {
    const std::string cfg = config::GetString("Thumper", "ModsDir", "Mods");
    std::wstring dir(cfg.begin(), cfg.end());
    if (dir.size() < 2 || (dir[1] != L':' && dir[0] != L'\\')) dir = game::GameDir() + L"\\" + dir;
    return dir;
}

bool RunOnMain(std::function<void()> fn) {
    if (GetCurrentThreadId() == events::MainThreadId()) {
        fn();
        return true;
    }
    auto done = std::make_shared<std::promise<void>>();
    std::future<void> f = done->get_future();
    {
        std::lock_guard lk(g_mainMx);
        g_mainQueue.push_back([fn = std::move(fn), done] {
            fn();
            done->set_value();
        });
    }
    return f.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
}

void DropThumperState(const std::string& id) {
    thumper::State& s = thumper::Live();
    s.enabled.erase(id);
    s.deepDesert.erase(id);
    std::erase_if(s.pins, [&](const thumper::PinEntry& p) { return p.id == id || p.before == id || p.after == id; });
    thumper::Save();
    thumper::Rescan();
}

class GameHost final : public Host {
  public:
    std::string MelangeVersion() override { return MELANGE_VERSION; }
    std::string GameBuild() override { return game::IsKnownBuild() ? "1077" : ""; }
    std::string Gate() override { return session::ChangeRefusal(session::Now(), session::For::Plugins); }
    std::vector<LocalMod> InstalledMods() override {
        std::vector<LocalMod> out;
        for (const thumper::Entry& e : thumper::Snapshot()) {
            LocalMod l;
            l.id = e.manifest.id;
            l.version = e.manifest.version;
            l.state = StateName(e.state);
            l.enabled = e.state == mods::State::Enabled;
            l.sessionActive = e.sessionActive;
            l.contentRelevant = e.contentRelevant;
            for (const auto& d : e.manifest.dependencies) l.dependencies.push_back(d.id);
            for (const auto& d : e.manifest.conflicts) l.conflicts.push_back(d.id);
            out.push_back(std::move(l));
        }
        return out;
    }
    void Placed(const std::string& id, bool enable) override {
        RunOnMain([id, enable] {
            thumper::Rescan();
            if (!enable) {
                thumper::Live().enabled[id] = false;
                thumper::Save();
                thumper::Rescan();
                return;
            }
            thumper::Entry e2;
            if (levels::Enabled() && thumper::FindEntry(id, &e2) && e2.manifest.content && !e2.manifest.levels.empty() &&
                levels::live::PackRefusal(e2.manifest).empty()) {
                char why[256] = {};
                if (levels::EnablePackLive(id.c_str(), why, sizeof why)) return;
            }
            thumper::SetEnabled(id, true);
        });
    }
    void Unload(const std::string& id) override {
        RunOnMain([id] { thumper::SetEnabled(id, false); });
        RunOnMain([] {});
    }
    void Reload(const std::string& id, bool enable) override {
        RunOnMain([id, enable] {
            thumper::Rescan();
            if (enable) thumper::SetEnabled(id, true);
        });
    }
    void Forget(const std::string& id) override {
        RunOnMain([id] { DropThumperState(id); });
    }
    void DeleteData(const std::string& id) override {
        const std::wstring section = game::Widen("Mod." + id);
        WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, config::Path().c_str());
        install::DeleteTree(game::DataDir() + L"\\mods\\" + game::Widen(id));
    }
};
GameHost g_gameHost;

void OnFrame() {
    static unsigned frame = 0;
    if (frame++ % 15 == 0) Tick();
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard lk(g_mainMx);
        todo.swap(g_mainQueue);
    }
    for (auto& fn : todo) fn();
    std::vector<std::string> drop;
    {
        std::lock_guard lk(g_dropMx);
        drop.swap(g_dropState);
    }
    for (const std::string& id : drop) DropThumperState(id);
}

class Store final : public Module {
public:
    const char* Name() const override { return "Store"; }
    const char* Description() const override { return "plugin store: browse, install, update and remove mods from a curated list"; }
    int Order() const override { return 35; }   // before Thumper, so deferred updates land before its first scan

    bool Install() override {
        config::EnsureKey(Name(), "IndexUrl", kDefaultIndex);
        std::string url = config::GetString(Name(), "IndexUrl", kDefaultIndex);
        std::string why;
        if (url.empty()) url = kDefaultIndex;
        if (!CheckIndexUrl(url, &why)) {
            LOG_WARN("[store] IndexUrl refused (%s); using the default list", why.c_str());
            url = kDefaultIndex;
        }
        Config c;
        c.indexUrl = url;
        c.custom = url != kDefaultIndex;
        c.maxDownload = static_cast<uint64_t>(std::clamp(Int("MaxDownloadMB", 64), 1, 256)) << 20;
        c.showIncompatible = Bool("ShowIncompatible", false);
        SetHost(&g_gameHost, c);
        std::vector<std::string> dropped;
        Open(ModsDirFromIni(), &dropped);
        {
            std::lock_guard lk(g_dropMx);
            g_dropState = std::move(dropped);
        }
        events::Subscribe(events::Event::Frame, &OnFrame);
        events::Subscribe(events::Event::Shutdown, [] { Shutdown(); });
        mods::OnChange([](void*) { MarkDirty(); }, nullptr);
        RegisterPage();
        InstallRpc();
        LOG_INFO("[store] ready%s%s", c.custom ? ": custom index " : "", c.custom ? c.indexUrl.c_str() : "");
        return true;
    }
};
}  // namespace

MELANGE_MODULE(Store);
}  // namespace melange::store
