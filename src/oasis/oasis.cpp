// Oasis: the local web app. Nothing listens until it is opened (oasis.start, the overlay, AutoStart=1).
//   oasis.start            start the server
//   oasis.url              log the launch URL (token masked)
//   oasis.stats            log server counters
//   oasis.kick [client]    close one client, or all
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/oasis.h"
#include "melange/testcmd.h"
#include "oasis/core/files.h"
#include "oasis/core/router.h"
#include "oasis/core/server.h"
#include "oasis/panel.h"
#include "oasis/providers.h"
#include "tools/json_mini.h"
#include "version.h"

namespace melange::oasis {
namespace {
std::atomic<bool> g_enabled{false};
core::Config g_cfg;
std::unique_ptr<core::Files> g_files;
std::mutex g_startMx;
std::atomic<bool> g_autoStart{false};

std::string Masked(const std::string& url) {
    const size_t k = url.find("k=");
    if (k == std::string::npos) return url;
    return url.substr(0, k + 2 + (std::min<size_t>)(4, url.size() - k - 2)) + "...";
}

HMODULE Self() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Self), &m);
    return m;
}

void LoadBundle(const std::string& webRoot) {
    if (!webRoot.empty()) {
        std::wstring dir = game::Widen(webRoot);
        if (dir.size() < 2 || dir[1] != L':') dir = game::GameDir() + L"\\" + dir;
        g_files = core::DirFiles(dir);
        std::string build = "dev";
        if (FILE* f = _wfopen((dir + L"\\build.txt").c_str(), L"rb")) {
            char b[80] = {};
            if (fgets(b, sizeof b, f)) build = b;
            fclose(f);
            while (!build.empty() && (build.back() == '\n' || build.back() == '\r')) build.pop_back();
        }
        core::SetBuild(build);
        LOG_INFO("[oasis] web app from folder %s (build %s)", webRoot.c_str(), build.c_str());
        return;
    }
    HMODULE self = Self();
    HRSRC r = FindResourceW(self, L"OASIS_WEB", MAKEINTRESOURCEW(10));
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    const size_t size = r ? SizeofResource(self, r) : 0;
    g_files = core::ZipFiles(data, size);
    const std::string build = core::ZipEntryText(data, size, "build.txt");
    core::SetBuild(build.empty() ? "unknown" : build);
    LOG_INFO("[oasis] embedded web app: %zu bytes, build %s%s", size, build.c_str(), g_files ? "" : " (unreadable)");
}

void CreateInstanceMutex() {
    std::wstring dir = game::GameDir();
    for (auto& c : dir) c = static_cast<wchar_t>(towlower(c));
    uint32_t h = 2166136261u;
    for (wchar_t c : dir) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    wchar_t name[64];
    swprintf(name, 64, L"Local\\Melange-%08x", h);
    CreateMutexW(nullptr, FALSE, name);  // held for the life of the process
}

bool VerbStart(std::string_view, void*) {
    const bool ok = Start();
    LOG_INFO("[oasis] start: %s %s", ok ? "listening" : "failed", Masked(Url()).c_str());
    return ok;
}

bool VerbUrl(std::string_view, void*) {
    LOG_INFO("[oasis] url: %s", Running() ? Masked(Url()).c_str() : "(not running)");
    return true;
}

bool VerbStats(std::string_view, void*) {
    const Stats s = GetStats();
    LOG_INFO("[oasis] stats: running=%d port=%d clients=%u channels=%u methods=%u framesOut=%llu bytesOut=%llu "
             "bytesIn=%llu dropped=%llu authFailures=%llu rpcCalls=%llu",
             Running(), core::Port(), s.clients, s.channels, s.methods, s.framesOut, s.bytesOut, s.bytesIn, s.dropped,
             s.authFailures, s.rpcCalls);
    return true;
}

bool VerbKick(std::string_view a, void*) {
    core::Kick(atoi(std::string(a).c_str()));
    return true;
}

uint64_t Frame() { return events::FrameCount(); }

class Oasis final : public Module {
  public:
    const char* Name() const override { return "Oasis"; }
    const char* Description() const override { return "local web app on 127.0.0.1 (opens on request)"; }
    int Order() const override { return 60; }
    bool Install() override {
        g_cfg.port = Int("Port", 8765);
        g_cfg.portRange = (std::max)(1, Int("PortRange", 10));
        g_cfg.maxClients = (std::max)(1, Int("MaxClients", 4));
        g_cfg.readOnly = Bool("ReadOnly", false);
        g_autoStart = Bool("AutoStart", false);
        Bool("AutoOpen", false);
        Bool("RawInspect", true);
        config::EnsureKey(Name(), "WebRoot", "");
        config::EnsureKey(Name(), "Hotkey", "Ctrl+Shift+O");
        const std::string webRoot = config::GetString(Name(), "WebRoot", "");

        core::Host host;
        host.server = "game";
        host.gameJson = jsonmini::Obj().Int("exeBuild", game::IsKnownBuild() ? 1077 : 0).Str("melange", MELANGE_VERSION).End();
        host.frame = &Frame;
        core::SetHost(host);
        core::router::Configure(g_cfg);
        LoadBundle(webRoot);
        CreateInstanceMutex();

        providers::InstallLog();
        providers::InstallBus();
        providers::InstallNet();
        providers::InstallLobby();
        providers::InstallStats();
        providers::InstallState();
        providers::InstallEntities();
        providers::InstallLua();
        providers::InstallMods();
        providers::InstallIni();
        providers::InstallLevels();
        providers::InstallErgAssetRoute(game::GameDir(), config::GetInt("Erg", "PreviewCacheMB", 512));
        providers::InstallErgChannel();
        providers::InstallCapture();
        providers::InstallWebPanels();
        providers::InstallWormsign();
        panel::Install();  // A: overlay panel, hotkey/menu, AutoOpen, oasis_url.txt

        testcmd::Register("oasis.start", &VerbStart);
        testcmd::Register("oasis.url", &VerbUrl);
        testcmd::Register("oasis.stats", &VerbStats);
        testcmd::Register("oasis.kick", &VerbKick);
        events::Subscribe(events::Event::Frame, [] {
            if (g_autoStart.exchange(false)) Start();
            core::Pump();
        });
        g_enabled = true;
        LOG_INFO("[oasis] installed: port=%d range=%d maxClients=%d readOnly=%d autoStart=%d", g_cfg.port,
                 g_cfg.portRange, g_cfg.maxClients, g_cfg.readOnly, g_autoStart.load());
        return true;
    }
};
}  // namespace

bool Enabled() { return g_enabled.load(); }
bool Running() { return core::Running(); }

bool Start() {
    if (!g_enabled) return false;
    std::lock_guard lk(g_startMx);
    return core::Start(g_cfg, providers::MakeAuth(), g_files.get());
}

std::string Url() { return core::LaunchUrl(); }

MELANGE_MODULE(Oasis);
}  // namespace melange::oasis
