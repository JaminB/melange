#include "launcher/window.h"

#include <objbase.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wrl.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "WebView2.h"
#include "core/log.h"
#include "launcher/app.h"
#include "launcher/store_host.h"
#include "launcher/util.h"
#include "melange/oasis.h"
#include "oasis/core/server.h"
#include "store/store.h"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace melange::launcher::window {
namespace {
constexpr wchar_t kClass[] = L"MelangeLauncher";
constexpr wchar_t kSmallClass[] = L"MelangeLauncherSmall";
constexpr UINT kTimerPump = 1, kTimerQuit = 2;
constexpr UINT WM_APP_THEME = WM_APP + 1, WM_APP_QUIT = WM_APP + 2;
constexpr int kDefaultW = 1120, kDefaultH = 740, kMinW = 880, kMinH = 600;
constexpr wchar_t kWebView2Url[] = L"https://developer.microsoft.com/microsoft-edge/webview2/";

HINSTANCE g_inst;
HWND g_hwnd;
std::string g_url;
std::wstring g_origin;   // "http://127.0.0.1:<port>/"
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
bool g_shown = false, g_inPump = false, g_failed = false;
HRESULT g_failure = S_OK;
HBRUSH g_brush;
std::atomic<bool> g_quit{false};
unsigned g_ticks = 0;
std::chrono::steady_clock::time_point g_lastClient = std::chrono::steady_clock::now();
bool g_idleExit = false;

COLORREF Bg(bool dark) { return dark ? RGB(0x15, 0x17, 0x1a) : RGB(0xf6, 0xf5, 0xf1); }

void Pump() {
    if (g_inPump) return;   // a modal dialog inside a main-thread call still dispatches WM_TIMER
    g_inPump = true;
    oasis::core::Pump();
    if (++g_ticks % 16 == 0) {
        storehost::Sync();
        store::Tick();
    }
    if (g_idleExit) {
        if (oasis::Clients() > 0) g_lastClient = std::chrono::steady_clock::now();
        else if (std::chrono::steady_clock::now() - g_lastClient > std::chrono::minutes(10)) PostQuitMessage(0);
    }
    g_inPump = false;
}

int Scale(HWND hwnd, int v) { return MulDiv(v, static_cast<int>(GetDpiForWindow(hwnd)), 96); }

void SetCaptionTheme(HWND hwnd, bool dark) {
    const BOOL on = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof on);
}

void ApplyThemeNow() {
    const bool dark = DarkTheme(app::GetSettings().theme);
    if (g_brush) DeleteObject(g_brush);
    g_brush = CreateSolidBrush(Bg(dark));
    if (!g_hwnd) return;
    SetClassLongPtrW(g_hwnd, GCLP_HBRBACKGROUND, reinterpret_cast<LONG_PTR>(g_brush));
    SetCaptionTheme(g_hwnd, dark);
    ComPtr<ICoreWebView2Controller2> c2;
    if (g_controller && SUCCEEDED(g_controller.As(&c2))) {
        const COLORREF bg = Bg(dark);
        c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, GetRValue(bg), GetGValue(bg), GetBValue(bg)});
    }
    InvalidateRect(g_hwnd, nullptr, TRUE);
    // Redraw the caption so the new mode shows at once.
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}

void Resize() {
    if (!g_controller) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

bool Allowed(const std::wstring& uri) { return uri.compare(0, g_origin.size(), g_origin) == 0; }

void OpenExternal(const std::wstring& uri) {
    if (uri.rfind(L"https://", 0) == 0) ShellExecuteW(g_hwnd, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void PlaceWindow(HWND hwnd) {
    const Settings s = app::GetSettings();
    if (s.window.saved) {
        RECT rc{s.window.left, s.window.top, s.window.right, s.window.bottom};
        if (rc.right - rc.left >= 200 && rc.bottom - rc.top >= 150 && MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof wp;
            wp.showCmd = s.window.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
            wp.rcNormalPosition = rc;
            SetWindowPlacement(hwnd, &wp);
            return;
        }
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT wa = mi.rcWork;
    const int w = std::min(Scale(hwnd, kDefaultW), static_cast<int>(wa.right - wa.left));
    const int h = std::min(Scale(hwnd, kDefaultH), static_cast<int>(wa.bottom - wa.top));
    SetWindowPos(hwnd, nullptr, wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
}

void SavePlacement(HWND hwnd) {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    if (!GetWindowPlacement(hwnd, &wp)) return;
    app::UpdateSettings([&](Settings& s) {
        s.window.saved = true;
        s.window.maximized = wp.showCmd == SW_SHOWMAXIMIZED;
        s.window.left = wp.rcNormalPosition.left;
        s.window.top = wp.rcNormalPosition.top;
        s.window.right = wp.rcNormalPosition.right;
        s.window.bottom = wp.rcNormalPosition.bottom;
    });
}

// A second Melange.exe sent its command line: come to the front; --game opens the folder-confirmation step.
void OnForwarded(const std::wstring& cmdline) {
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdline.c_str(), &argc);
    for (int i = 0; argv && i + 1 < argc; ++i)
        if (wcscmp(argv[i], L"--game") == 0 && g_webview) {
            std::wstring enc;
            for (char ch : Narrow(FullPath(argv[i + 1]))) {
                if (isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') enc += static_cast<wchar_t>(ch);
                else {
                    wchar_t b[4];
                    swprintf(b, 4, L"%%%02X", static_cast<unsigned char>(ch));
                    enc += b;
                }
            }
            g_webview->Navigate((Widen(g_url) + L"#setup-game=" + enc).c_str());
        }
    if (argv) LocalFree(argv);
}

void Fail(HRESULT hr, const char* where) {
    LOG_WARN("[launcher] WebView2 %s failed: 0x%08lx", where, static_cast<unsigned long>(hr));
    g_failed = true;
    g_failure = hr;
    PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

HRESULT OnController(HRESULT hr, ICoreWebView2Controller* controller) {
    if (FAILED(hr) || !controller) {
        Fail(FAILED(hr) ? hr : E_FAIL, "controller creation");
        return S_OK;
    }
    g_controller = controller;
    g_controller->get_CoreWebView2(&g_webview);
    g_controller->put_IsVisible(FALSE);
    ApplyThemeNow();
    const bool dev = app::Opts().devtools;
    ComPtr<ICoreWebView2Settings> s;
    if (SUCCEEDED(g_webview->get_Settings(&s))) {
        s->put_AreDevToolsEnabled(dev);
        s->put_AreDefaultContextMenusEnabled(dev);
        s->put_IsStatusBarEnabled(FALSE);
        s->put_IsZoomControlEnabled(TRUE);
        ComPtr<ICoreWebView2Settings3> s3;
        if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(dev);
        ComPtr<ICoreWebView2Settings4> s4;
        if (SUCCEEDED(s.As(&s4))) {
            s4->put_IsPasswordAutosaveEnabled(FALSE);
            s4->put_IsGeneralAutofillEnabled(FALSE);
        }
    }
    EventRegistrationToken tok{};
    auto onNavigationStarting = [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
        LPWSTR uri = nullptr;
        a->get_Uri(&uri);
        const std::wstring u = uri ? uri : L"";
        if (uri) CoTaskMemFree(uri);
        if (!Allowed(u)) {
            a->put_Cancel(TRUE);
            OpenExternal(u);
        }
        return S_OK;
    };
    g_webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(onNavigationStarting).Get(), &tok);
    // A sandboxed plugin panel iframe can still navigate itself (sandbox="allow-scripts" alone does not block
    // same-frame navigation): without this, only the top-level handler above would ever see it.
    g_webview->add_FrameNavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(onNavigationStarting).Get(), &tok);
    g_webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                          [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                              LPWSTR uri = nullptr;
                                              a->get_Uri(&uri);
                                              const std::wstring u = uri ? uri : L"";
                                              if (uri) CoTaskMemFree(uri);
                                              a->put_Handled(TRUE);
                                              OpenExternal(u);
                                              return S_OK;
                                          }).Get(),
                                      &tok);
    g_webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                           [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                               if (!g_shown) {
                                                   g_shown = true;
                                                   g_controller->put_IsVisible(TRUE);
                                                   g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                                               }
                                               return S_OK;
                                           }).Get(),
                                       &tok);
    g_webview->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
                                     [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                                         COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                                         a->get_ProcessFailedKind(&kind);
                                         LOG_WARN("[launcher] WebView2 process failed (kind %d)", static_cast<int>(kind));
                                         if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED && g_webview) g_webview->Reload();
                                         return S_OK;
                                     }).Get(),
                                 &tok);
    Resize();
    g_webview->Navigate(Widen(g_url).c_str());
    return S_OK;
}

HRESULT OnEnvironment(HRESULT hr, ICoreWebView2Environment* env) {
    if (FAILED(hr) || !env) {
        Fail(FAILED(hr) ? hr : E_FAIL, "environment creation");
        return S_OK;
    }
    g_env = env;
    hr = env->CreateCoreWebView2Controller(g_hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(&OnController).Get());
    if (FAILED(hr)) Fail(hr, "controller request");
    return S_OK;
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerPump) Pump();
            if (wp == kTimerQuit) {
                KillTimer(hwnd, kTimerQuit);
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        case WM_SIZE:
            Resize();
            return 0;
        case WM_MOVE:
        case WM_MOVING:
            if (g_controller) g_controller->NotifyParentWindowPositionChanged();
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = Scale(hwnd, kMinW);
            mm->ptMinTrackSize.y = Scale(hwnd, kMinH);
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<LPCWSTR>(lp), L"ImmersiveColorSet") == 0) ApplyThemeNow();
            break;
        case WM_APP_THEME:
            ApplyThemeNow();
            return 0;
        case WM_APP_QUIT:
            SetTimer(hwnd, kTimerQuit, 600, nullptr);
            return 0;
        case WM_COPYDATA: {
            const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
            if (cds && cds->dwData == 0x4d454c47 && cds->cbData < 65536)
                OnForwarded(std::wstring(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t)));
            return TRUE;
        }
        case WM_CLOSE:
            if (!g_failed) SavePlacement(hwnd);
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerPump);
            if (g_controller) g_controller->Close();
            g_controller.Reset();
            g_webview.Reset();
            g_env.Reset();
            g_hwnd = nullptr;
            app::SetWindow(nullptr);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Loop() {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

enum class Fallback { Browser, Close };

HRESULT CALLBACK DialogProc(HWND, UINT msg, WPARAM wp, LPARAM, LONG_PTR) {
    if (msg == TDN_BUTTON_CLICKED && wp == 102) {
        ShellExecuteW(nullptr, L"open", kWebView2Url, nullptr, nullptr, SW_SHOWNORMAL);
        return S_FALSE;   // keep the dialog up
    }
    return S_OK;
}

Fallback AskFallback() {
    const TASKDIALOG_BUTTON buttons[] = {{101, L"Open in my browser"}, {102, L"Get WebView2"}};
    TASKDIALOGCONFIG c{};
    c.cbSize = sizeof c;
    c.hInstance = g_inst;
    c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    c.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    c.pszWindowTitle = L"Melange";
    c.pszMainIcon = TD_WARNING_ICON;
    c.pszMainInstruction = L"Melange needs Microsoft Edge WebView2 to show its window";
    c.pszContent = L"WebView2 is part of Windows 11 and most Windows 10 PCs, but it is missing or not working here.\n\n"
                   L"You can open Melange in your web browser instead, or install WebView2 from Microsoft and start Melange again.";
    c.pButtons = buttons;
    c.cButtons = 2;
    c.nDefaultButton = 101;
    c.pfCallback = &DialogProc;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr))) return Fallback::Close;
    return pressed == 101 ? Fallback::Browser : Fallback::Close;
}

// ---------------------------------------------------------------- --browser
HWND g_small;
HFONT g_font;

LRESULT CALLBACK SmallProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == kTimerPump) Pump();
            if (wp == kTimerQuit) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == 201) ShellExecuteW(hwnd, L"open", Widen(g_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            if (LOWORD(wp) == 202) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_CTLCOLORSTATIC: {
            const bool dark = DarkTheme(app::GetSettings().theme);
            SetBkColor(reinterpret_cast<HDC>(wp), Bg(dark));
            SetTextColor(reinterpret_cast<HDC>(wp), dark ? RGB(0xe8, 0xe6, 0xe1) : RGB(0x1f, 0x22, 0x26));
            return reinterpret_cast<LRESULT>(g_brush);
        }
        case WM_APP_QUIT:
            SetTimer(hwnd, kTimerQuit, 600, nullptr);
            return 0;
        case WM_COPYDATA:
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            ShellExecuteW(hwnd, L"open", Widen(g_url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return TRUE;
        case WM_DESTROY:
            KillTimer(hwnd, kTimerPump);
            app::SetWindow(nullptr);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

bool DarkTheme(const std::string& theme) {
    if (theme == "dark") return true;
    if (theme == "light") return false;
    DWORD v = 1, n = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, nullptr, &v, &n) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

void ApplyTheme(const std::string&) {
    if (HWND h = app::Window()) PostMessageW(h, WM_APP_THEME, 0, 0);
}

void QuitSoon() {
    g_quit = true;
    if (HWND h = app::Window()) PostMessageW(h, WM_APP_QUIT, 0, 0);
}

int RunBrowser(HINSTANCE inst, const std::string& url) {
    g_inst = inst;
    g_url = url;
    g_idleExit = true;
    const bool dark = DarkTheme(app::GetSettings().theme);
    g_brush = CreateSolidBrush(Bg(dark));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &SmallProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brush;
    wc.lpszClassName = kSmallClass;
    RegisterClassExW(&wc);
    g_small = CreateWindowExW(0, kSmallClass, L"Melange", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                              340, 150, nullptr, nullptr, inst, nullptr);
    if (!g_small) return 1;
    app::SetWindow(g_small);
    SetCaptionTheme(g_small, dark);
    const int dpi = static_cast<int>(GetDpiForWindow(g_small));
    auto S = [&](int v) { return MulDiv(v, dpi, 96); };
    RECT want{0, 0, S(340), S(150)};
    AdjustWindowRectExForDpi(&want, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, static_cast<UINT>(dpi));
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(g_small, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int w = want.right - want.left, h = want.bottom - want.top;
    SetWindowPos(g_small, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w,
                 h, SWP_NOZORDER);
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, static_cast<UINT>(dpi));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    HWND text = CreateWindowExW(0, L"STATIC", L"Melange is open in your browser.", WS_CHILD | WS_VISIBLE, S(20), S(24), S(300), S(24), g_small, nullptr,
                                inst, nullptr);
    HWND again = CreateWindowExW(0, L"BUTTON", L"Open again", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, S(132), S(72), S(96), S(32),
                                 g_small, reinterpret_cast<HMENU>(201), inst, nullptr);
    HWND quit = CreateWindowExW(0, L"BUTTON", L"Quit", WS_CHILD | WS_VISIBLE | WS_TABSTOP, S(236), S(72), S(84), S(32), g_small,
                                reinterpret_cast<HMENU>(202), inst, nullptr);
    for (HWND c : {text, again, quit}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    ShowWindow(g_small, SW_SHOWNORMAL);
    SetTimer(g_small, kTimerPump, 15, nullptr);
    ShellExecuteW(g_small, L"open", Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(g_small, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

int Run(HINSTANCE inst, const std::string& url, int port) {
    g_inst = inst;
    g_url = url;
    g_origin = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    LPWSTR ver = nullptr;
    const HRESULT hrVer = GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    const bool runtime = SUCCEEDED(hrVer) && ver;
    if (ver) {
        LOG_INFO("[launcher] WebView2 runtime %ls", ver);
        CoTaskMemFree(ver);
    }
    if (!runtime) {
        LOG_WARN("[launcher] WebView2 runtime not found (0x%08lx)", static_cast<unsigned long>(hrVer));
        return AskFallback() == Fallback::Browser ? RunBrowser(inst, url) : 0;
    }

    const bool dark = DarkTheme(app::GetSettings().theme);
    g_brush = CreateSolidBrush(Bg(dark));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_brush;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(0, kClass, L"Melange", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                             inst, nullptr);
    if (!g_hwnd) return 1;
    app::SetWindow(g_hwnd);
    app::SetWebView(true);
    SetCaptionTheme(g_hwnd, dark);
    PlaceWindow(g_hwnd);
    UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, kTimerPump, 15, nullptr);

    const std::wstring udf = AppDataDir() + L"\\WebView2";
    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr,
                                                                Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(&OnEnvironment).Get());
    if (FAILED(hr)) Fail(hr, "environment request");
    Loop();
    if (g_failed && !g_quit) {
        app::SetWebView(false);
        if (AskFallback() == Fallback::Browser) return RunBrowser(inst, url);
    }
    return 0;
}

int RunHeadless() {
    while (!g_quit.load()) {
        oasis::core::Pump();
        if (++g_ticks % 16 == 0) {
            storehost::Sync();
            store::Tick();
        }
        Sleep(15);
    }
    Sleep(300);
    return 0;
}
}  // namespace melange::launcher::window
