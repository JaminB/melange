// Melange.exe: the desktop app. Serves the Oasis web app on 127.0.0.1 (the standalone server, with the setup,
// plugin and launcher methods) and shows it in a WebView2 window; --browser uses the default browser instead.
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

#include <shellapi.h>

#include <cstdio>
#include <memory>
#include <string>

#include "core/log.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/store_host.h"
#include "launcher/util.h"
#include "launcher/window.h"
#include "oasis/core/files.h"
#include "oasis/core/router.h"
#include "oasis/core/server.h"
#include "oasis/providers.h"
#include "oasis/standalone/register.h"
#include "store/store.h"
#include "version.h"

namespace L = melange::launcher;
namespace oc = melange::oasis::core;

namespace {

std::wstring GameDirFn() { return L::app::GameDir(); }
std::string WriteGateFn() { return L::app::WriteGate(); }

void OpenLog() {
    const std::wstring dir = L::AppDataDir();
    const std::wstring log = dir + L"\\launcher.log", old = dir + L"\\launcher.1.log";
    if (L::FileSize(log) > 0) MoveFileExW(log.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    melange::log::Init(log);
}

void Print(const std::string& s) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!out || out == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) out = GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (!out || out == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(out, s.data(), static_cast<DWORD>(s.size()), &wr, nullptr);
}

// Window mode is single-instance: a second start hands its command line to the first and exits. After an elevated
// restart (--resume) the new instance waits for the old one to close instead.
bool SecondInstance(HANDLE* mutex, bool resume) {
    *mutex = CreateMutexW(nullptr, TRUE, L"Local\\Melange-Launcher");
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    if (resume) {
        const DWORD w = WaitForSingleObject(*mutex, 15000);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) return false;
    }
    HWND other = FindWindowW(L"MelangeLauncher", nullptr);
    if (!other) other = FindWindowW(L"MelangeLauncherSmall", nullptr);
    if (other) {
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        const std::wstring cmd = GetCommandLineW();
        COPYDATASTRUCT cds{0x4d454c47, static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)), const_cast<wchar_t*>(cmd.c_str())};
        DWORD_PTR res = 0;
        SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG, 3000, &res);
    }
    return true;
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    L::app::Options opts;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--game" && i + 1 < argc) opts.game = argv[++i];
        else if (a == L"--web-root" && i + 1 < argc) opts.webRoot = argv[++i];
        else if (a == L"--resume" && i + 1 < argc) opts.resume = L::Narrow(argv[++i]);
        else if (a == L"--browser") opts.browser = true;
        else if (a == L"--serve") opts.serve = true;
        else if (a == L"--devtools") opts.devtools = true;
    }
    if (argv) LocalFree(argv);

    HANDLE mutex = nullptr;
    if (!opts.serve && !opts.browser && SecondInstance(&mutex, !opts.resume.empty())) return 0;

    OpenLog();
    LOG_INFO("[launcher] Melange %s starting%s%s", MELANGE_VERSION, opts.serve ? " (--serve)" : "", opts.browser ? " (--browser)" : "");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    L::app::Init(opts);
    for (const auto& p : melange::launcher::setup::ProtectFromEnv()) LOG_INFO("[launcher] protected: %ls", p.c_str());

    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    std::unique_ptr<oc::Files> files;
    std::string build = "standalone-dev";
    if (!opts.webRoot.empty()) {
        files = oc::DirFiles(opts.webRoot);
        std::string b;
        if (L::ReadAll(opts.webRoot + L"\\build.txt", &b)) {
            while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back();
            build = b;
        }
    } else {
        HRSRC res = FindResourceW(inst, L"OASIS_WEB", MAKEINTRESOURCEW(10));
        HGLOBAL g = res ? LoadResource(inst, res) : nullptr;
        const void* data = g ? LockResource(g) : nullptr;
        const size_t size = res ? SizeofResource(inst, res) : 0;
        files = oc::ZipFiles(data, size);
        build = oc::ZipEntryText(data, size, "build.txt");
        if (build.empty()) build = "standalone";
    }
    oc::Host host;
    host.server = "standalone";
    host.caps = {"launcher"};
    oc::SetHost(host);
    oc::SetBuild(build);

    melange::oasis::standalone::StandaloneHost sh;
    sh.gameDir = &GameDirFn;
    sh.writeGate = &WriteGateFn;
    sh.version = MELANGE_VERSION;
    melange::oasis::standalone::RegisterStandalone(sh);
    if (const std::wstring game = L::app::GameDir(); !game.empty() && L::DirExists(game)) melange::oasis::standalone::RegisterLevels(game);
    L::rpc::InstallSetup();
    L::rpc::InstallLauncher();
    L::rpc::InstallPlugins();
    L::storehost::Install();
    L::app::StartChannel();

    oc::Config cfg;
    if (!oc::Start(cfg, melange::oasis::providers::MakeAuth(), files.get())) {
        LOG_WARN("[launcher] could not start the server (ports %d..%d taken?)", cfg.port, cfg.port + cfg.portRange - 1);
        MessageBoxW(nullptr, L"Melange could not start its local server: every port it tries is in use.", L"Melange", MB_ICONERROR);
        return 1;
    }
    const std::string url = oc::LaunchUrl();
    LOG_INFO("[launcher] listening on port %d", oc::Port());

    int rc;
    if (opts.serve) {
        Print("melange: listening, " + url + "\n");
        rc = L::window::RunHeadless();
    } else if (opts.browser) {
        rc = L::window::RunBrowser(inst, url);
    } else {
        rc = L::window::Run(inst, url, oc::Port());
    }
    melange::store::Shutdown();
    oc::Stop();
    LOG_INFO("[launcher] exit %d", rc);
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    CoUninitialize();
    return rc;
}
