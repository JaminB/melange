#include "launcher/app.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "launcher/setup/detect.h"
#include "launcher/setup/running.h"
#include "launcher/util.h"
#include "melange/oasis.h"
#include "oasis/standalone/register.h"
#include "tools/json_mini.h"
#include "version.h"

namespace melange::launcher::app {
namespace {
Options g_opts;
std::mutex g_mx;
Settings g_settings;
std::wstring g_gameDir;
std::vector<std::wstring> g_protect;
std::mutex g_tx;
std::atomic<HWND> g_hwnd{nullptr};
std::atomic<bool> g_webview{false};
oasis::ChannelId g_channel = 0;
std::mutex g_gateMx;
std::string g_gate;

std::string Fingerprint(const std::wstring& dir) {
    if (dir.empty()) return "none";
    std::string fp = setup::GameRunning(dir) ? "R" : "r";
    fp += setup::MelangeLoaded(dir) ? "L" : "l";
    static const wchar_t* const kFiles[] = {
        L"WormsMayhem.exe", L"dinput8.dll", L"melange.asi", L"melange.asi.off", L"scripts\\melange.asi", L"plugins\\melange.asi",
        L"scripts\\melange.asi.off", L"plugins\\melange.asi.off", L"WUMFix.asi", L"oasis.exe", L"Melange.ini", L"Melange.exe",
        L"Melange\\install.json", L"Melange\\backup", L"dsound.dll", L"winmm.dll", L"version.dll", L"d3d9.dll", L"xinput1_3.dll",
        L"winhttp.dll", L"wininet.dll"};
    for (const wchar_t* f : kFiles) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW((dir + L"\\" + f).c_str(), GetFileExInfoStandard, &fa)) {
            fp += "-";
            continue;
        }
        char b[64];
        snprintf(b, sizeof b, "%lx.%lx.%lx;", fa.nFileSizeLow, fa.ftLastWriteTime.dwLowDateTime, fa.ftLastWriteTime.dwHighDateTime);
        fp += b;
    }
    return fp;
}

void OnSub(oasis::ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (subscribed) oasis::PublishTo(ch, client, "{\"status\":" + StatusJson() + "}");
}

void Poll() {
    std::string last;
    for (;;) {
        const std::wstring dir = GameDir();
        const std::string gate = WriteGate();
        {
            std::lock_guard lk(g_gateMx);
            g_gate = gate;
        }
        const std::string fp = Narrow(dir) + "|" + Fingerprint(dir);
        if (fp != last) {
            last = fp;
            if (oasis::HasSubscribers(g_channel)) PublishStatus();
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}
}  // namespace

void Init(const Options& o) {
    g_opts = o;
    g_protect = setup::ProtectFromEnv();
    Settings s;
    LoadSettings(SettingsPath(), &s);
    std::lock_guard lk(g_mx);
    g_settings = s;
    g_gameDir = !o.game.empty() ? FullPath(o.game) : s.gameDir;
}

const Options& Opts() { return g_opts; }
std::string Version() { return MELANGE_VERSION; }

std::wstring GameDir() {
    std::lock_guard lk(g_mx);
    return g_gameDir;
}

void SetGameDir(const std::wstring& dir, bool save) {
    {
        std::lock_guard lk(g_mx);
        g_gameDir = FullPath(dir);
        if (save) {
            g_settings.gameDir = g_gameDir;
            SaveSettings(SettingsPath(), g_settings);
        }
    }
    PublishStatus();
}

Settings GetSettings() {
    std::lock_guard lk(g_mx);
    return g_settings;
}

void UpdateSettings(const std::function<void(Settings&)>& fn) {
    std::lock_guard lk(g_mx);
    fn(g_settings);
    SaveSettings(SettingsPath(), g_settings);
}

setup::Context MakeContext() {
    setup::Context c;
    c.gameDir = GameDir();
    c.payloadDir = ExeDir();
    c.selfExe = ExePath();
    c.version = MELANGE_VERSION;
    c.logsDir = c.gameDir.empty() ? std::wstring() : oasis::standalone::LogsDir(c.gameDir);
    c.protect = g_protect;
    c.storeOf = [](const std::wstring& dir) { return setup::StoreOf(dir, setup::SystemRegistry()); };
    c.progress = [](int step, int of, const std::string& label) { PublishProgress("", step, of, label); };
    return c;
}

std::string WriteGate() {
    const setup::Context c = MakeContext();
    return setup::WriteGate(c);
}

std::string CachedGate() {
    std::lock_guard lk(g_gateMx);
    return g_gate;
}

std::mutex& Tx() { return g_tx; }

std::string StatusJson() { return setup::StatusJson(setup::Inspect(MakeContext())); }

void StartChannel() {
    g_channel = oasis::AddChannel("setup");
    oasis::OnSubscribe(g_channel, &OnSub, nullptr);
    {
        std::lock_guard lk(g_gateMx);
        g_gate = WriteGate();
    }
    std::thread(&Poll).detach();
}

void PublishStatus() {
    if (g_channel && oasis::HasSubscribers(g_channel)) oasis::Publish(g_channel, "{\"status\":" + StatusJson() + "}");
}

void PublishProgress(const std::string& action, int step, int of, const std::string& label) {
    if (!g_channel) return;
    oasis::Publish(g_channel, jsonmini::Obj()
                                  .Raw("progress", jsonmini::Obj().Str("action", action).Int("step", step).Int("of", of).Str("label", label).End())
                                  .End());
}

void SetWindow(HWND hwnd) { g_hwnd = hwnd; }
HWND Window() { return g_hwnd.load(); }
bool WebView() { return g_webview.load(); }
void SetWebView(bool on) { g_webview = on; }

bool Elevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return ok;
}
}  // namespace melange::launcher::app
