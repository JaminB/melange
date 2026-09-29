// Overlay panel "Oasis": status, the launch URL (hidden behind "Show"), Copy URL, Open in browser, a client
// list with Kick, and stats. Hotkey + menu item "Oasis/Open". AutoOpen and oasis_url.txt (for test scripts)
// live here too: the Oasis module itself only reads their ini defaults, it does not act on them.
#include "oasis/panel.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <imgui.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "melange/oasis.h"
#include "melange/overlay.h"
#include "oasis/core/router.h"
#include "oasis/core/server.h"

#pragma comment(lib, "ole32.lib")  // CoTaskMemFree (SHGetKnownFolderPath's result)

namespace melange::oasis::panel {
namespace {
constexpr char kSection[] = "Oasis";

// All of this file's own state is touched only on the main thread: overlay hotkeys, menu actions and panel
// draw calls all fire there (overlay.h), and so does the Frame event.
bool g_userOpenPending = false;
bool g_wasRunning = false;
bool g_showToken = false;
bool g_lastStartFailed = false;  // e.g. every port in Port..Port+PortRange-1 is taken; see Melange.log

std::string MaskUrl(const std::string& url) {
    const size_t k = url.find("k=");
    if (k == std::string::npos) return url;
    return url.substr(0, k + 2 + (std::min<size_t>)(4, url.size() - k - 2)) + "...";
}

void OpenBrowser(const std::string& url) {
    if (!url.empty()) ShellExecuteW(nullptr, L"open", game::Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CopyToClipboard(const std::string& text) {
    const std::wstring w = game::Widen(text);
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        if (void* p = GlobalLock(mem)) {
            memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
    }
    CloseClipboard();
}

// Documents\Melange\oasis_url.txt: the unmasked launch URL, for non-browser test scripts. Never read by
// anything in melange.asi itself.
std::wstring UrlFilePath() {
    PWSTR docs = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        dir = std::wstring(docs) + L"\\Melange";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    if (docs) CoTaskMemFree(docs);
    return dir.empty() ? std::wstring() : dir + L"\\oasis_url.txt";
}

void DeleteUrlFile() {
    const std::wstring p = UrlFilePath();
    if (!p.empty()) DeleteFileW(p.c_str());
}

void WriteUrlFile() {
    const std::wstring p = UrlFilePath();
    if (p.empty()) return;
    if (FILE* f = _wfopen(p.c_str(), L"wb")) {
        const std::string url = oasis::Url();
        fwrite(url.data(), 1, url.size(), f);
        fclose(f);
    }
}

// Starts the server if needed and opens exactly one browser tab: the hotkey, the menu item and the panel's own
// button all go through here. The AutoStart+AutoOpen combination (no user action at all) goes through OnFrame.
void OpenOasisNow() {
    const bool wasRunning = oasis::Running();
    if (!wasRunning) g_userOpenPending = true;
    g_lastStartFailed = !oasis::Start();
    if (g_lastStartFailed) return;  // nothing to open; the panel shows why
    const std::string url = oasis::Url();
    OpenBrowser(url);
    LOG_INFO("[oasis] opened %s", MaskUrl(url).c_str());
}

void OnHotkeyOrMenu(void*) { OpenOasisNow(); }

void OnFrame() {
    const bool running = oasis::Running();
    if (running && !g_wasRunning) {
        WriteUrlFile();
        if (!std::exchange(g_userOpenPending, false) && config::GetBool(kSection, "AutoOpen", false)) {
            const std::string url = oasis::Url();
            OpenBrowser(url);
            LOG_INFO("[oasis] auto-opened %s", MaskUrl(url).c_str());
        }
    }
    g_wasRunning = running;
}

void DrawClients() {
    const std::vector<int> ids = core::router::ListClients();
    if (ids.empty()) {
        ImGui::TextDisabled("No clients connected.");
        return;
    }
    if (!ImGui::BeginTable("oasis_clients", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) return;
    ImGui::TableSetupColumn("Client", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableHeadersRow();
    for (int id : ids) {
        ImGui::PushID(id);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("#%d", id);
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Kick")) core::Kick(id);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void Draw(void*) {
    const bool running = oasis::Running();
    ImGui::TextColored(running ? ImVec4(0.55f, 0.9f, 0.55f, 1.f) : ImVec4(0.6f, 0.6f, 0.6f, 1.f), "%s",
                        running ? "Running" : "Stopped");
    if (!running) {
        ImGui::SameLine();
        if (ImGui::Button("Open Oasis")) OpenOasisNow();
        if (g_lastStartFailed)
            ImGui::TextColored(ImVec4(1.f, 0.45f, 0.35f, 1.f),
                                "Could not start: no free port in %d..%d (see Melange.log).", config::GetInt(kSection, "Port", 8765),
                                config::GetInt(kSection, "Port", 8765) + config::GetInt(kSection, "PortRange", 10) - 1);
        else
            ImGui::TextDisabled("Nothing listens until Oasis is opened.");
        return;
    }
    g_lastStartFailed = false;
    ImGui::SameLine();
    ImGui::TextDisabled("port %d", core::Port());

    const std::string url = oasis::Url();
    ImGui::Checkbox("Show", &g_showToken);
    ImGui::SameLine();
    ImGui::TextUnformatted(g_showToken ? url.c_str() : MaskUrl(url).c_str());
    if (ImGui::Button("Copy URL")) CopyToClipboard(url);
    ImGui::SameLine();
    if (ImGui::Button("Open in browser")) OpenBrowser(url);

    const Stats s = oasis::GetStats();
    ImGui::Text("%u client(s)  %u channel(s)  %u method(s)  %llu rpc call(s)  %llu auth failure(s)", s.clients,
                s.channels, s.methods, static_cast<unsigned long long>(s.rpcCalls),
                static_cast<unsigned long long>(s.authFailures));

    ImGui::Separator();
    DrawClients();
}
}  // namespace

void Install() {
    DeleteUrlFile();  // a stale token from a previous run should never linger
    overlay::AddPanel("oasis", "Oasis", &Draw, nullptr);
    const std::string hotkeyText = config::GetString(kSection, "Hotkey", "Ctrl+Shift+O");
    uint8_t dik = 0, mods = 0;
    if (!hotkeyText.empty()) {
        if (overlay::ParseHotkey(hotkeyText.c_str(), &dik, &mods))
            overlay::AddHotkey(dik, mods, &OnHotkeyOrMenu, nullptr);
        else
            LOG_WARN("[oasis] Hotkey '%s' not understood, no hotkey registered", hotkeyText.c_str());
    }
    overlay::AddMenuItem("Oasis/Open", &OnHotkeyOrMenu, nullptr, hotkeyText.c_str());
    events::Subscribe(events::Event::Frame, &OnFrame);
}
}  // namespace melange::oasis::panel
