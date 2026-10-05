// launcher.*: the app's own state, theme, Launch game, folders and shortcuts.
#include <windows.h>
#include <objbase.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <atomic>
#include <mutex>

#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/setup/detect.h"
#include "launcher/setup/engine.h"
#include "launcher/setup/running.h"
#include "launcher/util.h"
#include "launcher/window.h"
#include "oasis/standalone/register.h"
#include "tools/json_mini.h"
#include "tools/log_export_core.h"

namespace melange::launcher::rpc {
namespace {
using oasis::Call;
using oasis::Result;
namespace ex = exporter::core;

std::atomic<bool> g_exporting{false};
std::mutex g_exportMx;
std::wstring g_lastExport;  // for launcher.openPath {what: "export"}

void State(const Call&, Result& r, void*) {
    const Settings s = app::GetSettings();
    const std::wstring game = app::GameDir();
    jsonmini::Arr prot;
    for (const auto& p : setup::ProtectFromEnv()) prot.Str(Narrow(p));
    jsonmini::Obj o;
    o.Str("version", app::Version()).Bool("firstRun", !s.firstRunDone || game.empty());
    if (game.empty()) o.Raw("gameDir", "null");
    else o.Str("gameDir", Narrow(game));
    o.Str("theme", s.theme).Bool("webview", app::WebView()).Bool("elevated", app::Elevated()).Raw("protected", prot.End());
    if (!app::Opts().resume.empty()) o.Str("resume", app::Opts().resume);
    r.json = o.End();
}

void SetTheme(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string theme = Str(p, "theme");
    if (theme != "system" && theme != "light" && theme != "dark") return Fail(r, -32602, "theme must be system, light or dark");
    app::UpdateSettings([&](Settings& s) { s.theme = theme; });
    window::ApplyTheme(theme);
    r.json = "{}";
}

void Launch(const Call&, Result& r, void*) {
    const std::wstring game = app::GameDir();
    if (game.empty()) return Fail(r, -32000, "Choose your game folder first.");
    if (!FileExists(game + L"\\WormsMayhem.exe")) return Fail(r, -32000, "WormsMayhem.exe isn't in this folder.");
    if (setup::GameRunning(game)) return Fail(r, -32000, "The game is already running.");
    if (setup::StoreOf(game, setup::SystemRegistry()) == "steam") {
        const auto h = reinterpret_cast<INT_PTR>(ShellExecuteW(app::Window(), L"open", L"steam://rungameid/70600", nullptr, nullptr, SW_SHOWNORMAL));
        if (h <= 32) return Fail(r, -32000, "Steam didn't start the game. Is Steam installed?");
        r.json = "{\"how\":\"steam\"}";
        return;
    }
    std::wstring cmd = L"\"" + game + L"\\WormsMayhem.exe\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, game.c_str(), &si, &pi))
        return Fail(r, -32000, "Could not start the game: " + Win32Message(GetLastError()));
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.json = "{\"how\":\"exe\"}";
}

void OpenPath(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string what = Str(p, "what");
    const std::wstring game = app::GameDir();
    std::wstring path;
    if (what == "game") {
        path = game;
    } else if (what == "logs") {
        path = oasis::standalone::LogsDir(game);
    } else if (what == "backup") {
        const std::string id = Str(p, "id");
        if (id.empty() || id.find_first_of("\\/:") != std::string::npos || id.find("..") != std::string::npos) return Fail(r, -32602, "bad backup id");
        path = game.empty() ? std::wstring() : game + L"\\Melange\\backup\\" + Widen(id);
    } else if (what == "export") {
        // The zip launcher.exportLogs wrote last, selected in Explorer. Only that path: this never reveals an
        // arbitrary file the page names.
        std::wstring zip;
        {
            std::lock_guard lk(g_exportMx);
            zip = g_lastExport;
        }
        if (zip.empty() || !FileExists(zip)) return Fail(r, -32000, "That export isn't there any more.");
        if (!ex::RevealInExplorer(zip)) return Fail(r, -32000, "Explorer didn't open.");
        r.json = "{}";
        return;
    } else {
        return Fail(r, -32602, "what must be game, logs, backup or export");
    }
    if (path.empty() || !DirExists(path)) return Fail(r, -32000, "That folder doesn't exist yet.");
    ShellExecuteW(app::Window(), L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    r.json = "{}";
}

// One click: the last game's logs (the newest session folder, the replays and desync bundles its pid wrote, dumps
// and engine logs from its time window, launcher.log) zipped to the Desktop by the same core and redaction as the
// in-game export, then shown selected in Explorer unless {reveal: false}. Runs on the server thread: an export
// takes a second or two, and the page shows a spinner meanwhile.
void ExportLogs(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const bool reveal = Bool(p, "reveal", true);
    bool expected = false;
    if (!g_exporting.compare_exchange_strong(expected, true)) return Fail(r, -32002, "An export is already running.");

    namespace sa = oasis::standalone;
    const std::wstring game = app::GameDir();
    ex::Request rq;
    rq.scope = ex::Scope::LastGame;
    rq.producer = "launcher";
    rq.opt.includeDumps = sa::IniGet(game, "LogExport", "IncludeDumps", "1") != "0";
    rq.opt.includeFullDumps = sa::IniGet(game, "LogExport", "IncludeFullDumps", "0") == "1";
    rq.opt.redactUserPaths = sa::IniGet(game, "LogExport", "RedactUserPaths", "1") != "0";
    rq.opt.sessions = 1;
    const std::wstring docs = ex::KnownFolder(ex::Folder::Documents);
    rq.src.sessionRoots.push_back(sa::LogsDir(game));
    if (!docs.empty()) rq.src.sessionRoots.push_back(docs + L"\\Melange\\logs");
    if (!game.empty()) {
        rq.src.sessionRoots.push_back(game + L"\\Melange\\logs");
        // melange.asi can sit next to the exe or in the loader's scripts\ or plugins\ folder; its data folder is
        // beside it.
        for (const wchar_t* sub : {L"", L"\\scripts", L"\\plugins"}) rq.src.dataDirs.push_back(game + sub + L"\\Melange");
    }
    rq.src.gameDir = game;
    rq.src.replaysDir = docs.empty() ? std::wstring() : docs + L"\\Melange\\replays";
    rq.src.launcherLogDir = AppDataDir();

    bool onDesktop = false;
    const std::wstring path = ex::OneClickPath(&onDesktop);
    ex::Result res;
    const bool ok = ex::Export(path, rq, &res);
    g_exporting = false;
    if (!ok) return Fail(r, -32000, "Could not export the logs: " + res.error);
    {
        std::lock_guard lk(g_exportMx);
        g_lastExport = path;
    }
    if (reveal) ex::RevealInExplorer(path);
    jsonmini::Obj o;
    o.Str("path", Narrow(path)).UInt("bytes", res.bytes).UInt("entries", res.entries).Bool("onDesktop", onDesktop);
    if (res.sessionId.empty()) o.Raw("sessionId", "null");
    else o.Str("sessionId", res.sessionId);
    o.UInt("pid", res.pid);
    r.json = o.End();
}

bool MakeShortcut(const std::wstring& lnk, const std::wstring& target, std::string* err) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) return *err = "could not create a shortcut", false;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(Parent(target).c_str());
    link->SetDescription(L"Melange for Worms Ultimate Mayhem");
    link->SetIconLocation(target.c_str(), 0);
    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) {
        hr = file->Save(lnk.c_str(), TRUE);
        file->Release();
    }
    link->Release();
    if (FAILED(hr)) return *err = "could not save " + Narrow(lnk), false;
    return true;
}

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

void Shortcuts(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const json::Value* sm = p.Get("startMenu");
    const json::Value* dt = p.Get("desktop");
    if (!sm || !sm->IsBool() || !dt || !dt->IsBool()) return Fail(r, -32602, "expected {startMenu, desktop}");
    const std::wstring game = app::GameDir();
    const std::wstring target = !game.empty() && FileExists(game + L"\\Melange.exe") ? game + L"\\Melange.exe" : ExePath();
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string err;
    bool ok = true;
    const std::pair<std::wstring, bool> places[] = {{Known(FOLDERID_Programs), sm->boolean}, {Known(FOLDERID_Desktop), dt->boolean}};
    for (const auto& [dir, want] : places) {
        if (dir.empty()) continue;
        const std::wstring lnk = dir + L"\\Melange.lnk";
        if (want) ok &= MakeShortcut(lnk, target, &err);
        else DeleteFileW(lnk.c_str());
    }
    if (SUCCEEDED(init)) CoUninitialize();
    if (!ok) return Fail(r, -32000, err);
    r.json = "{}";
}
}  // namespace

void InstallLauncher() {
    using oasis::kRpcMutating;
    using oasis::kRpcServerThread;
    oasis::AddMethod("launcher.state", &State, nullptr, kRpcServerThread);
    oasis::AddMethod("launcher.setTheme", &SetTheme, nullptr, kRpcMutating);
    oasis::AddMethod("launcher.launch", &Launch, nullptr, oasis::kRpcNone);
    oasis::AddMethod("launcher.openPath", &OpenPath, nullptr, oasis::kRpcNone);
    oasis::AddMethod("launcher.exportLogs", &ExportLogs, nullptr, kRpcServerThread);
    oasis::AddMethod("launcher.shortcuts", &Shortcuts, nullptr, kRpcServerThread | kRpcMutating);
}
}  // namespace melange::launcher::rpc
