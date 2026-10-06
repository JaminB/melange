// LogExport: the in-game log exports. Public API: melange/export.h. One click ("Export last game's logs", the
// hotkey) and "Save logs as..." (a dialog, the newest N sessions); both build the zip in tools/log_export_core.cpp,
// which Melange.exe shares.
#include "melange/export.h"

#include <windows.h>

#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")  // CLSID_FileSaveDialog / IID_IFileSaveDialog definitions

#include "core/config.h"
#include "core/debug.h"
#include "core/dump_paths.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "render/mirage/compat.h"
#include "tools/json_mini.h"
#include "tools/log_export_core.h"
#include "tools/sysinfo.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"

namespace melange::exporter {
namespace {
namespace core = melange::exporter::core;

std::string Narrow(const std::wstring& w) { return melange::game::Narrow(w); }

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::wstring DocumentsDir() { return core::KnownFolder(core::Folder::Documents); }

// Where this game's files are, plus what only the running game can report.
core::Request GameRequest(core::Scope scope, const Options& opt) {
    core::Request rq;
    rq.scope = scope;
    rq.opt = opt;
    rq.producer = "game";
    const melange::jlog::Session& session = melange::jlog::CurrentSession();
    const std::wstring docs = DocumentsDir();
    const std::wstring& game = melange::game::GameDir();
    if (!session.root.empty()) {
        rq.src.sessionRoots.push_back(session.root);
    } else {
        // Session logging is off or failed to start: look where it would have written.
        const std::wstring dir = Widen(melange::config::GetString("Logging", "Dir", ""));
        if (!dir.empty()) rq.src.sessionRoots.push_back(dir.size() > 1 && dir[1] == L':' ? dir : game + L"\\" + dir);
        if (!docs.empty()) rq.src.sessionRoots.push_back(docs + L"\\Melange\\logs");
        rq.src.sessionRoots.push_back(game + L"\\Melange\\logs");
    }
    rq.src.currentSessionDir = session.dir;
    rq.src.currentPid = GetCurrentProcessId();
    rq.src.dataDirs = {melange::game::DataDir()};
    rq.src.dumpDirs = melange::debug::DumpDirs(L"", docs);  // the fallback WriteMiniDump uses
    rq.src.gameDir = game;
    rq.src.replaysDir = docs.empty() ? std::wstring() : docs + L"\\Melange\\replays";
    const std::wstring local = core::KnownFolder(core::Folder::LocalAppData);
    rq.src.launcherLogDir = local.empty() ? std::wstring() : local + L"\\Melange";

    rq.prov.beforeCollect = [] { melange::jlog::Flush(); };
    rq.prov.systemJson = [] { return melange::sysinfo::CollectJson(); };
    rq.prov.pluginsJson = [] { return melange::sysinfo::PluginsJson(); };
    // Installed modules only; skipped/disabled ones are not tracked.
    rq.prov.modulesJson = [] {
        jsonmini::Arr arr;
        for (const auto* m : melange::modules::Installed()) {
            jsonmini::Obj o;
            o.Str("name", m->Name()).Str("description", m->Description()).Int("order", m->Order())
                .Bool("enabled", true)
                .Bool("installed", true);
            arr.Raw(o.End());
        }
        return arr.End();
    };
    rq.prov.gpuCompatJson = [] { return melange::mirage::compat::Json(); };
    rq.prov.gpuCompatText = [] { return melange::mirage::compat::Text(); };
    return rq;
}

// Runs on a worker thread; touches no main-thread state.
bool RunExport(const std::wstring& zipPath, core::Scope scope, const Options& opt, std::string* error,
               core::Result* result = nullptr) {
    if (GetCurrentThreadId() == melange::events::MainThreadId())
        LOG_WARN("[LogExport] export called on the main thread; this blocks rendering until it's done");
    const uint64_t frameStart = melange::events::FrameCount();
    core::Result local;
    core::Result* res = result ? result : &local;
    const bool ok = core::Export(zipPath, GameRequest(scope, opt), res);
    if (!ok && error) *error = res->error;
    LOG_INFO("[LogExport] frame %llu -> %llu during the export", static_cast<unsigned long long>(frameStart),
             static_cast<unsigned long long>(melange::events::FrameCount()));
    return ok;
}

// Status

std::mutex g_statusMx;
State g_state = State::Idle;
std::wstring g_lastPath;
std::string g_lastError;
std::atomic<bool> g_busy{false};

void SetState(State s) {
    std::lock_guard lk(g_statusMx);
    g_state = s;
}
void SetDone(const std::wstring& path) {
    std::lock_guard lk(g_statusMx);
    g_state = State::Done;
    g_lastPath = path;
    g_lastError.clear();
}
void SetFailed(const std::string& err) {
    std::lock_guard lk(g_statusMx);
    g_state = State::Failed;
    g_lastError = err;
}
void SetCancelled() {
    std::lock_guard lk(g_statusMx);
    g_state = State::Cancelled;
}

// Save dialog

// Heuristic: no caption/frame and the window covers its monitor (exclusive and borderless alike).
bool IsFullscreen() {
    HWND h = static_cast<HWND>(melange::events::GameWindow());
    if (!h) return false;
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & (WS_CAPTION | WS_THICKFRAME)) return false;
    RECT wr{};
    if (!GetWindowRect(h, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

enum class DialogResult { Ok, Cancelled, ApiUnavailable };

DialogResult ShowSaveDialogCOM(std::wstring* outPath) {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return DialogResult::ApiUnavailable;
    COMDLG_FILTERSPEC filter[] = {{L"Zip files", L"*.zip"}};
    dlg->SetFileTypes(1, filter);
    dlg->SetDefaultExtension(L"zip");
    dlg->SetFileName(core::DefaultZipName().c_str());
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(DocumentsDir().c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
        dlg->SetFolder(folder);
        folder->Release();
    }
    HRESULT hr = dlg->Show(nullptr);  // no owner, so the game keeps rendering
    DialogResult result = DialogResult::Cancelled;
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                *outPath = path;
                CoTaskMemFree(path);
                result = DialogResult::Ok;
            }
            item->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result = DialogResult::ApiUnavailable;  // not a cancel: fall back to the legacy dialog
    }
    dlg->Release();
    return result;
}

DialogResult ShowSaveDialogLegacy(std::wstring* outPath) {
    wchar_t buf[MAX_PATH] = L"";
    wcsncpy_s(buf, core::DefaultZipName().c_str(), _TRUNCATE);
    std::wstring docs = DocumentsDir();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Zip files\0*.zip\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"zip";
    ofn.lpstrInitialDir = docs.empty() ? nullptr : docs.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        *outPath = buf;
        return DialogResult::Ok;
    }
    return DialogResult::Cancelled;
}

// Toast: a topmost click-through popup showing where an export was saved.
LRESULT CALLBACK ToastWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 240, 240));
            auto* text = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (text)
                DrawTextW(dc, text->c_str(), -1, &rc,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

DWORD WINAPI ToastThreadProc(LPVOID param) {
    std::unique_ptr<std::wstring> text(static_cast<std::wstring*>(param));
    static std::atomic<bool> s_classRegistered{false};
    const wchar_t* kClassName = L"MelangeExportToast";
    if (!s_classRegistered.exchange(true)) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &ToastWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        RegisterClassW(&wc);
    }
    RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (HWND game = static_cast<HWND>(melange::events::GameWindow())) GetWindowRect(game, &area);
    int w = std::min<int>(720, area.right - area.left - 32), h = 56;
    int x = area.left + ((area.right - area.left) - w) / 2;
    int y = area.top + 36;
    HWND toast = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 kClassName, L"", WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr),
                                 nullptr);
    if (!toast) return 0;
    SetLayeredWindowAttributes(toast, 0, 235, LWA_ALPHA);
    SetWindowLongPtrW(toast, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(text.get()));
    ShowWindow(toast, SW_SHOWNOACTIVATE);
    SetTimer(toast, 1, 6000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void ShowToast(const std::wstring& text) {
    auto* data = new std::wstring(text);
    HANDLE th = CreateThread(nullptr, 0, &ToastThreadProc, data, 0, nullptr);
    if (th)
        CloseHandle(th);
    else
        delete data;
}

DWORD WINAPI SaveAsWorkerProc(LPVOID) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool fullscreen = IsFullscreen();
    std::wstring path;
    bool haveDialog = true;

    if (fullscreen) {
        std::wstring dir = DocumentsDir();
        dir = (dir.empty() ? std::wstring(L".") : dir) + L"\\Melange\\exports";
        path = dir + L"\\" + core::DefaultZipName();
    } else {
        SetState(State::Dialog);
        DialogResult r = ShowSaveDialogCOM(&path);
        if (r == DialogResult::ApiUnavailable) r = ShowSaveDialogLegacy(&path);
        haveDialog = r == DialogResult::Ok;
        if (!haveDialog) {
            SetCancelled();
            LOG_INFO("[LogExport] save dialog cancelled (state Cancelled, nothing written)");
        }
    }

    if (haveDialog) {
        SetState(State::Writing);
        std::string err;
        bool ok = RunExport(path, core::Scope::RecentSessions, DefaultOptions(), &err);
        if (ok) {
            SetDone(path);
            if (fullscreen) ShowToast(L"Melange: logs saved to " + path);
        } else {
            SetFailed(err);
        }
    }

    if (SUCCEEDED(coHr)) CoUninitialize();
    g_busy = false;
    return 0;
}

// One click: no dialog, the Desktop, then a toast and (windowed) Explorer with the zip selected. Fullscreen stays
// in the game: Explorer taking the focus would minimise an exclusive-fullscreen window.
DWORD WINAPI LastGameWorkerProc(LPVOID) {
    const bool fullscreen = IsFullscreen();
    bool onDesktop = false;
    const std::wstring path = core::OneClickPath(&onDesktop);
    std::string err;
    core::Result res;
    if (RunExport(path, core::Scope::LastGame, DefaultOptions(), &err, &res)) {
        SetDone(path);
        ShowToast(onDesktop ? L"Melange: logs saved to your Desktop as " + path.substr(path.find_last_of(L'\\') + 1)
                            : L"Melange: logs saved to " + path);
        if (!fullscreen) core::RevealInExplorer(path);
    } else {
        SetFailed(err);
        ShowToast(L"Melange: exporting the logs failed (see Melange.log)");
    }
    g_busy = false;
    return 0;
}

bool StartWorker(LPTHREAD_START_ROUTINE proc, State initial) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        LOG_WARN("[LogExport] an export is already running, ignoring request");
        return false;
    }
    SetState(initial);
    HANDLE th = CreateThread(nullptr, 0, proc, nullptr, 0, nullptr);
    if (!th) {
        g_busy = false;
        SetFailed("CreateThread failed");
        return false;
    }
    CloseHandle(th);
    return true;
}

struct VerbJob {
    std::wstring path;
    core::Scope scope;
    const char* verb;
};

// savelogs <path.zip>: the "Save logs as..." contents. savelogs-last [<path.zip>]: the one-click contents, to the
// one-click destination when no path is given (never revealed or toasted: automation must not steal the focus).
bool StartVerb(std::string_view args, core::Scope scope, const char* verb) {
    std::string path = Trim(std::string(args));
    if (path.empty() && scope == core::Scope::RecentSessions) {
        LOG_WARN("[auto] savelogs: usage: savelogs <path.zip>");
        return false;
    }
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        LOG_WARN("[auto] %s: an export is already running", verb);
        return false;
    }
    SetState(State::Writing);
    bool onDesktop = false;
    auto* job = new VerbJob{path.empty() ? core::OneClickPath(&onDesktop) : Widen(path), scope, verb};
    HANDLE th = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            std::unique_ptr<VerbJob> j(static_cast<VerbJob*>(param));
            std::string err;
            core::Result res;
            bool ok = RunExport(j->path, j->scope, DefaultOptions(), &err, &res);
            if (ok) {
                SetDone(j->path);
                LOG_INFO("[auto] %s: wrote %s (%llu bytes, session %s)", j->verb, Narrow(j->path).c_str(),
                         static_cast<unsigned long long>(res.bytes), res.sessionId.empty() ? "-" : res.sessionId.c_str());
            } else {
                SetFailed(err);
                LOG_ERROR("[auto] %s: failed: %s", j->verb, err.c_str());
            }
            g_busy = false;
            return 0;
        },
        job, 0, nullptr);
    if (!th) {
        LOG_ERROR("[auto] %s: CreateThread failed", verb);
        delete job;
        g_busy = false;
        return false;
    }
    CloseHandle(th);
    return true;  // result is logged asynchronously; testcmd handlers must not block
}

bool OnSaveLogsVerb(std::string_view args, void*) { return StartVerb(args, core::Scope::RecentSessions, "savelogs"); }
bool OnSaveLastVerb(std::string_view args, void*) { return StartVerb(args, core::Scope::LastGame, "savelogs-last"); }

void OnExportLastGame(void*) { RequestExportLastGame(); }
void OnSaveAs(void*) { RequestSaveAs(); }

bool ExportSync(const std::wstring& zipPath, core::Scope scope, const Options& opt, std::string* error) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        if (error) *error = "an export is already running";
        LOG_WARN("[LogExport] ExportTo: an export is already running");
        return false;
    }
    SetState(State::Writing);
    std::string err;
    bool ok = RunExport(zipPath, scope, opt, &err);
    if (ok)
        SetDone(zipPath);
    else
        SetFailed(err.empty() ? "export failed" : err);
    if (error) *error = err;
    g_busy = false;
    return ok;
}

class LogExport final : public melange::Module {
public:
    const char* Name() const override { return "LogExport"; }
    const char* Description() const override {
        return "one-click \"Export last game's logs\" and \"Save logs as...\": zip of logs, replays, dumps, ini and system info";
    }
    int Order() const override { return 60; }

    bool Install() override {
        melange::config::EnsureKey(Name(), "Hotkey", "Ctrl+Shift+F11");
        std::string hotkeyText = melange::config::GetString(Name(), "Hotkey", "Ctrl+Shift+F11");

        uint8_t dik = 0, mods = 0;
        if (!hotkeyText.empty()) {
            if (melange::overlay::ParseHotkey(hotkeyText.c_str(), &dik, &mods))
                melange::overlay::AddHotkey(dik, mods, &OnExportLastGame, nullptr);
            else
                LOG_WARN("[LogExport] Hotkey '%s' not understood, no hotkey registered", hotkeyText.c_str());
        }
        // Registration order is menu order: the one-click export is the File menu's first item.
        melange::overlay::AddMenuItem("File/Export last game's logs", &OnExportLastGame, nullptr, hotkeyText.c_str());
        melange::overlay::AddMenuItem("File/Save logs as...", &OnSaveAs, nullptr);
        melange::testcmd::Register("savelogs", &OnSaveLogsVerb);
        melange::testcmd::Register("savelogs-last", &OnSaveLastVerb);
        LOG_INFO("[LogExport] ready (hotkey %s)", hotkeyText.empty() ? "(none)" : hotkeyText.c_str());
        return true;
    }
};
}  // namespace

MELANGE_MODULE(LogExport);

Options DefaultOptions() {
    Options o;
    melange::config::EnsureKey("LogExport", "Sessions", "3");
    o.sessions = melange::config::GetInt("LogExport", "Sessions", 3);
    melange::config::EnsureKey("LogExport", "IncludeDumps", "1");
    o.includeDumps = melange::config::GetBool("LogExport", "IncludeDumps", true);
    melange::config::EnsureKey("LogExport", "IncludeFullDumps", "0");
    o.includeFullDumps = melange::config::GetBool("LogExport", "IncludeFullDumps", false);
    melange::config::EnsureKey("LogExport", "RedactUserPaths", "1");
    o.redactUserPaths = melange::config::GetBool("LogExport", "RedactUserPaths", true);
    return o;
}

bool RequestExportLastGame() { return StartWorker(&LastGameWorkerProc, State::Writing); }

bool RequestSaveAs() { return StartWorker(&SaveAsWorkerProc, State::Dialog); }

bool ExportTo(const std::wstring& zipPath, const Options& opt, std::string* error) {
    return ExportSync(zipPath, core::Scope::RecentSessions, opt, error);
}

bool ExportLastGameTo(const std::wstring& zipPath, const Options& opt, std::string* error) {
    return ExportSync(zipPath, core::Scope::LastGame, opt, error);
}

State Status(std::wstring* lastPath, std::string* lastError) {
    std::lock_guard lk(g_statusMx);
    if (lastPath) *lastPath = g_lastPath;
    if (lastError) *lastError = g_lastError;
    return g_state;
}
}  // namespace melange::exporter
