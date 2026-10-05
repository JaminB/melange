// [Update] in the game: at most once a day, a background look at GitHub's latest Melange release. A newer one shows a
// toast; nothing is downloaded here, Melange.exe installs it the next time it opens.
#include <windows.h>

#include <shlobj.h>

#include <atomic>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include "core/events.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/draw.h"
#include "melange/overlay.h"
#include "store/fetch.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"
#include "update/release.h"
#include "version.h"

namespace melange::update {
namespace {
constexpr long long kIntervalSeconds = 24 * 60 * 60;
constexpr uint64_t kStartFrame = 600;   // about 10 s in: the main menu, past the loading screens
constexpr uint64_t kToastMs = 12000;

std::atomic<bool> g_started{false};
std::mutex g_mx;
std::string g_toast;   // set by the check thread, drawn on the main thread
uint64_t g_toastSince = 0;

std::wstring StampPath() {
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = std::wstring(docs) + L"\\Melange\\update-check.json";
    if (docs) CoTaskMemFree(docs);
    return out;
}

long long LastCheck(const std::wstring& path) {
    json::Value v;
    json::Error e;
    if (path.empty() || !json::ParseFile(path, &v, &e) || !v.IsObject()) return 0;
    const json::Value* t = v.Get("lastCheck");
    return t && t->IsInteger() ? static_cast<long long>(t->number) : 0;
}

void SaveStamp(const std::wstring& path, long long now, const std::string& latest) {
    if (path.empty()) return;
    CreateDirectoryW(path.substr(0, path.rfind(L'\\')).c_str(), nullptr);
    const std::string j = jsonmini::Obj().Int("lastCheck", now).Str("latest", latest).End();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, j.data(), static_cast<DWORD>(j.size()), &w, nullptr);
    CloseHandle(f);
}

void Check() {
    const std::wstring stamp = StampPath();
    const long long now = static_cast<long long>(_time64(nullptr)), last = LastCheck(stamp);
    if (last > 0 && now >= last && now - last < kIntervalSeconds) {
        LOG_INFO("[update] checked for a newer Melange less than a day ago; not again yet");
        return;
    }
    store::fetch::StringSink sink;
    store::fetch::Options o;
    o.cap = kMaxApiBytes;
    o.totalMs = 20000;
    o.stallMs = 10000;
    o.registerActive = false;
    o.userAgent = std::string("Melange/") + MELANGE_VERSION;
    std::string err;
    Release rel;
    if (!store::fetch::Get(kLatestUrl, sink, o, &err) || !ParseRelease(sink.data, &rel, &err)) {
        LOG_INFO("[update] could not check for a newer Melange: %s", err.c_str());   // offline is fine; try next start
        return;
    }
    SaveStamp(stamp, now, rel.version);
    if (CompareVersions(rel.version, MELANGE_VERSION) <= 0) {
        LOG_INFO("[update] Melange %s is the latest release", MELANGE_VERSION);
        return;
    }
    LOG_INFO("[update] Melange %s is available (this is %s)", rel.version.c_str(), MELANGE_VERSION);
    std::lock_guard lk(g_mx);
    g_toast = "Melange " + rel.version + " is available";
}

void DrawToast() {
    std::string text;
    {
        std::lock_guard lk(g_mx);
        if (g_toast.empty()) return;
        if (!g_toastSince) g_toastSince = GetTickCount64();
        if (GetTickCount64() - g_toastSince > kToastMs) {
            g_toast.clear();
            return;
        }
        text = g_toast;
    }
    static const char kLine2[] = "It installs the next time you open Melange.exe";
    const overlay::GlInfo gl = overlay::Gl();
    const float w = gl.viewportW > 0 ? static_cast<float>(gl.viewportW) : 1280.f;
    const float tw = 10.f * static_cast<float>(sizeof kLine2 - 1) + 24.f;
    const float x0 = (w - tw) * 0.5f, y0 = 48.f;
    draw::HudRect(x0, y0, x0 + tw, y0 + 56.f, 0xd8101018, true);
    draw::HudRect(x0, y0, x0 + tw, y0 + 56.f, 0xff3080ff, false, 2.f);
    draw::HudText(x0 + 12.f, y0 + 7.f, text.c_str(), 0xff70c0ff, 18.f);
    draw::HudText(x0 + 12.f, y0 + 31.f, kLine2, 0xffd0d0d0, 16.f);
}

void OnFrame() {
    if (!g_started.load() && events::FrameCount() >= kStartFrame && !g_started.exchange(true)) std::thread(&Check).detach();
    DrawToast();
}

class Update final : public Module {
public:
    const char* Name() const override { return "Update"; }
    const char* Description() const override { return "at most once a day, checks GitHub for a newer Melange and says so"; }

    bool Install() override {
        if (!Bool("CheckInGame", true)) {
            LOG_INFO("[update] in-game check off (CheckInGame=0)");
            return true;
        }
        events::Subscribe(events::Event::Frame, &OnFrame);
        return true;
    }
};
}  // namespace

MELANGE_MODULE(Update);
}  // namespace melange::update
