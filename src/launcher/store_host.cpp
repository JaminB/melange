// The Store engine inside Melange.exe: the chosen game folder's Mods\, read from disk (the game is closed), with
// Melange.ini [Store] for the settings and the launcher's write gate.
#include "launcher/store_host.h"

#include <windows.h>

#include <algorithm>
#include <mutex>

#include "launcher/app.h"
#include "launcher/setup/exe_check.h"
#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "oasis/standalone/mods_provider.h"
#include "oasis/standalone/register.h"
#include "store/install.h"
#include "store/store.h"
#include "tools/json_read.h"
#include "version.h"

namespace melange::launcher::storehost {
namespace {
namespace modsprov = oasis::standalone::modsprov;

std::wstring g_openFor;

class LauncherHost final : public store::Host {
  public:
    std::string MelangeVersion() override { return MELANGE_VERSION; }
    std::string GameBuild() override {
        const std::wstring dir = app::GameDir();
        return !dir.empty() && setup::CheckExe(dir).verdict == setup::Verdict::Ok ? "1077" : "";
    }
    std::string Gate() override { return app::CachedGate(); }
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
            if (!l.id.empty()) out.push_back(std::move(l));
        }
        return out;
    }
    void Placed(const std::string& id, bool enable) override { modsprov::SetEnabled(app::GameDir(), id, enable); }
    void Unload(const std::string&) override {}
    void Reload(const std::string& id, bool enable) override {
        if (enable) modsprov::SetEnabled(app::GameDir(), id, true);
    }
    void Forget(const std::string& id) override { modsprov::SetEnabled(app::GameDir(), id, false); }
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
}

void Sync() {
    static std::mutex mx;
    std::lock_guard lk(mx);
    const std::wstring dir = app::GameDir();
    if (PathKey(dir) == PathKey(g_openFor)) return;
    if (dir.empty()) {
        store::Close();
        g_openFor.clear();
        return;
    }
    store::SetHost(&g_host, ConfigFor(dir));
    if (store::Open(dir + L"\\Mods")) g_openFor = dir;
}
}  // namespace melange::launcher::storehost
