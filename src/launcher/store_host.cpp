// The Store engine inside Melange.exe: the chosen game folder's Mods\, read from disk (the game is closed), with
// Melange.ini [Store] for the settings and the launcher's write gate. Also runs the compatibility sweep there.
#include "launcher/store_host.h"

#include <windows.h>

#include <algorithm>
#include <mutex>

#include "core/log.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/setup/engine.h"
#include "launcher/setup/exe_check.h"
#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "oasis/standalone/mods_provider.h"
#include "oasis/standalone/register.h"
#include "store/install.h"
#include "store/store.h"
#include "tools/json_read.h"
#include "update/release.h"
#include "version.h"

namespace melange::launcher::storehost {
namespace {
namespace modsprov = oasis::standalone::modsprov;

std::mutex g_syncMx;
std::wstring g_openFor;   // guarded by g_syncMx
std::mutex g_sweepMx;
bool g_sweepDue = false;
std::string g_sweepVersion;

class LauncherHost final : public store::Host {
  public:
    std::string MelangeVersion() override { return MELANGE_VERSION; }
    std::string GameBuild() override {
        const std::wstring dir = app::GameDir();
        return !dir.empty() && setup::CheckExe(dir).verdict == setup::Verdict::Ok ? "1077" : "";
    }
    std::string Gate() override { return rpc::ImportRunning() ? "an import is running" : app::CachedGate(); }
    std::vector<store::LocalMod> InstalledMods() override {
        std::vector<store::LocalMod> out;
        const std::wstring dir = app::GameDir();
        if (dir.empty()) return out;
        json::Value v;
        json::Error e;
        if (!json::Parse(modsprov::ListJson(dir, MELANGE_VERSION), &v, &e) || !v.IsArray()) return out;
        std::vector<spice::Manifest> enabled = modsprov::Enabled(dir, MELANGE_VERSION);
        for (const auto& m : v.items) {
            auto s = [&](const char* k) {
                const json::Value* x = m.Get(k);
                return x && x->IsString() ? x->string : std::string();
            };
            store::LocalMod l;
            l.id = s("id");
            l.version = s("version");
            l.state = s("state");
            l.enabled = l.state == "enabled";
            for (const auto& man : enabled)
                if (man.id == l.id) {
                    for (const auto& d : man.dependencies) l.dependencies.push_back(d.id);
                    for (const auto& d : man.conflicts) l.conflicts.push_back(d.id);
                }
            if (!l.id.empty() && !l.version.empty()) out.push_back(std::move(l));   // no version: its spice.json is broken
        }
        return out;
    }
    void Placed(const std::string& id, bool enable) override { modsprov::SetEnabled(app::GameDir(), id, enable); }
    void Unload(const std::string&) override {}
    void Reload(const std::string& id, bool enable) override {
        if (enable) modsprov::SetEnabled(app::GameDir(), id, true);
    }
    // SetEnabled would refuse (the folder is gone) and leave a stale entry: drop it, its grant and its pins.
    void Forget(const std::string& id) override { modsprov::Forget(app::GameDir(), id); }
    void DeleteData(const std::string& id) override {
        const std::wstring dir = app::GameDir();
        if (dir.empty()) return;
        WritePrivateProfileStringW(Widen("Mod." + id).c_str(), nullptr, nullptr, (dir + L"\\Melange.ini").c_str());
        store::install::DeleteTree(dir + L"\\Melange\\mods\\" + Widen(id));
    }
};
LauncherHost g_host;

store::Config ConfigFor(const std::wstring& dir) {
    store::Config c;
    std::string url = oasis::standalone::IniGet(dir, "Store", "IndexUrl", store::kDefaultIndex);
    std::string why;
    if (url.empty() || !store::CheckIndexUrl(url, &why)) url = store::kDefaultIndex;
    c.indexUrl = url;
    c.custom = url != store::kDefaultIndex;
    c.maxDownload = static_cast<uint64_t>(std::clamp(atoi(oasis::standalone::IniGet(dir, "Store", "MaxDownloadMB", "64").c_str()), 1, 256)) << 20;
    c.showIncompatible = oasis::standalone::IniGet(dir, "Store", "ShowIncompatible", "0") == "1";
    return c;
}
}  // namespace

void Install() {
    store::SetHost(&g_host, ConfigFor(app::GameDir()));
    store::InstallRpc(false);
    Sync();
    Tick();   // before the window and the server: the Plugins page never sees a plugin that is about to move
}

void Sync() {
    std::lock_guard lk(g_syncMx);
    const std::wstring dir = app::GameDir();
    if (PathKey(dir) == PathKey(g_openFor)) return;
    if (dir.empty()) {
        store::Close();
        g_openFor.clear();
        return;
    }
    const store::Status st = store::GetStatus();
    if (st.busy || st.fetching) return;   // the worker reads the settings; switch once it is idle
    store::SetHost(&g_host, ConfigFor(dir));
    if (store::Open(dir + L"\\Mods")) {
        g_openFor = dir;
        RequestSweep();
    }
}

void Tick() {
    std::string version;
    {
        std::lock_guard lk(g_sweepMx);
        if (!g_sweepDue) return;
        version = g_sweepVersion;
    }
    // CachedGate is the 2 s poll's (cheap); SweepNow asks for a fresh one before it touches anything.
    if (!app::CachedGate().empty()) return;
    if (!SweepNow(version)) return;
    std::lock_guard lk(g_sweepMx);
    if (g_sweepVersion == version) {
        g_sweepDue = false;
        g_sweepVersion.clear();
    }
}

void RequestSweep(const std::string& melangeVersion) {
    std::lock_guard lk(g_sweepMx);
    g_sweepDue = true;
    g_sweepVersion = melangeVersion;
}

bool SweepNow(const std::string& melangeVersion, compat::Report* report) {
    std::wstring dir;
    {
        std::lock_guard lk(g_syncMx);
        dir = g_openFor;
    }
    if (dir.empty() || PathKey(dir) != PathKey(app::GameDir())) return false;   // the Store is not open for it yet
    if (rpc::ImportRunning() || store::GetStatus().busy) return false;
    std::unique_lock tx(app::Tx(), std::try_to_lock);   // no setup.apply or plugins write against the same folder
    if (!tx.owns_lock()) return false;
    if (const std::string gate = app::WriteGate(); !gate.empty()) return false;   // the game runs from it, or similar
    const std::string version = melangeVersion.empty() ? std::string(MELANGE_VERSION) : melangeVersion;
    // An older Melange.exe (a stale copy run again) must not judge plugins made for the newer melange.asi the game
    // actually loads: they would be set aside, removed or downgraded.
    if (const setup::Status st = setup::Inspect(app::MakeContext());
        !st.melangeVersion.empty() && update::CompareVersions(st.melangeVersion, version) > 0) {
        LOG_INFO("[launcher] compatibility sweep skipped: the game has Melange %s, newer than %s", st.melangeVersion.c_str(), version.c_str());
        return true;
    }
    if (oasis::standalone::IniGet(dir, "Thumper", "SweepIncompatible", "1") == "0") {
        LOG_INFO("[launcher] compatibility sweep skipped: [Thumper] SweepIncompatible=0");
        return true;
    }
    compat::SweepContext c;
    c.modsDir = dir + L"\\Mods";
    c.melangeVersion = version;
    c.forget = [&](const std::string& id) { modsprov::Forget(dir, id); };
    const compat::Report r = compat::Sweep(c);
    for (const std::string& e : r.errors) LOG_WARN("[launcher] incompatible plugin left in place: %s", e.c_str());
    // The Store half: fetches the list first only when a Store plugin cannot load (to update it rather than remove it).
    store::Reconcile(r.store, true, melangeVersion);
    store::MarkDirty();
    LOG_INFO("[launcher] compatibility sweep against Melange %s: %zu local plugin(s) moved to Mods\\.incompatible, %zu Store "
             "plugin(s) to update or remove",
             version.c_str(), r.quarantined.size(), r.store.size());
    if (report) *report = r;
    return true;
}
}  // namespace melange::launcher::storehost
