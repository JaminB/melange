// Melange.exe: the desktop app. Serves the Oasis web app on 127.0.0.1 (the standalone server, with the setup,
// plugin and launcher methods) and shows it in a WebView2 window; --browser uses the default browser instead.
#include <winsock2.h>
#include <windows.h>
#include <objbase.h>

#include <shellapi.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/store_host.h"
#include "launcher/update_host.h"
#include "launcher/updater.h"
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
std::wstring DefaultsIniFn() { return L::ExeDir() + L"\\Melange.ini"; }

void OpenLog(const wchar_t* name = L"launcher") {
    const std::wstring dir = L::AppDataDir();
    const std::wstring log = dir + L"\\" + name + L".log", old = dir + L"\\" + name + L".1.log";
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

// Restore vanilla emptied the game folder but this Melange.exe ran from it: a running exe can't delete itself, so a
// hidden cmd waits for this process to be gone, then deletes the files.
void DeleteAfterExit(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    wchar_t sys[MAX_PATH];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return;
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\cmd.exe\" /d /c ping -n 3 127.0.0.1 >nul";
    for (const auto& p : paths) cmd += L" & del /f /q \"" + p + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        LOG_WARN("[vanilla] could not schedule deleting %zu file(s) after exit (%lu)", paths.size(), GetLastError());
        return;
    }
    LOG_INFO("[vanilla] %zu file(s) of this app are deleted after it exits", paths.size());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    L::app::Options opts;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; ++i) args.push_back(argv[i]);
    // A downloaded Melange.exe started by the old one to install itself: no window, no server.
    if (L::updater::IsApplyCommand(args)) {
        if (argv) LocalFree(argv);
        L::updater::ApplyArgs aa;
        std::string why;
        OpenLog(L"update");
        if (!L::updater::ParseApplyArgs(args, &aa, &why)) {
            LOG_WARN("[update] bad --apply-update command line: %s", why.c_str());
            return 2;
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const int rc = L::updatehost::RunApply(aa);
        CoUninitialize();
        return rc;
    }
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
    sh.defaultsIni = &DefaultsIniFn;
    melange::oasis::standalone::RegisterStandalone(sh);
    if (const std::wstring game = L::app::GameDir(); !game.empty() && L::DirExists(game)) melange::oasis::standalone::RegisterLevels(game);
    L::rpc::InstallSetup();
    L::rpc::InstallLauncher();
    L::rpc::InstallPlugins();
    L::rpc::InstallImport();
    L::rpc::InstallDisplay();
    L::rpc::InstallLaa();
    L::storehost::Install();
    L::updatehost::Install();
    L::updatehost::Start(!opts.serve);   // --serve is for tests: no automatic look at GitHub
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
    DeleteAfterExit(L::app::PendingDeletes());
    CoUninitialize();
    return rc;
}
