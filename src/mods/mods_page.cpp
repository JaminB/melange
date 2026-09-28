// Overlay panels "Thumper/Mods" and "Thumper/Deep Desert".
#include <windows.h>

#include <shellapi.h>

#include <imgui.h>

#include <string>
#include <vector>

#include "lua/sandbox_internal.h"
#include "melange/overlay.h"
#include "mods/thumper_internal.h"

namespace melange::thumper {
namespace {
const char* KindLabel(const spice::Manifest& m) { return m.content ? "content" : "client-only"; }

ImVec4 StateColor(mods::State s) {
    switch (s) {
        case mods::State::Enabled: return {0.55f, 0.9f, 0.55f, 1.f};
        case mods::State::Disabled: return {0.6f, 0.6f, 0.6f, 1.f};
        case mods::State::Blocked: return {1.f, 0.45f, 0.35f, 1.f};
        case mods::State::Incompatible: return {1.f, 0.45f, 0.35f, 1.f};
        case mods::State::PendingConsent: return {1.f, 0.75f, 0.3f, 1.f};
        case mods::State::RestartRequired: return {0.6f, 0.75f, 1.f, 1.f};
    }
    return {1.f, 1.f, 1.f, 1.f};
}

const char* StateLabel(mods::State s) {
    switch (s) {
        case mods::State::Enabled: return "enabled";
        case mods::State::Disabled: return "disabled";
        case mods::State::Blocked: return "blocked";
        case mods::State::Incompatible: return "incompatible";
        case mods::State::PendingConsent: return "pending consent";
        case mods::State::RestartRequired: return "restart required";
    }
    return "?";
}

void OpenFolder(const std::wstring& dir) {
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string Narrow(const wchar_t* w) {
    std::string s;
    for (const wchar_t* p = w; p && *p; ++p) s.push_back(static_cast<char>(*p < 128 ? *p : '?'));
    return s;
}

void DrawModsPanel(void*) {
    std::vector<Entry> entries = Snapshot();
    ImGui::TextDisabled("%zu mods discovered under %s", entries.size(), Narrow(mods::ModsDir()).c_str());
    bool anyRestart = false;
    for (const Entry& e : entries)
        if (e.state == mods::State::RestartRequired) anyRestart = true;
    if (anyRestart)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "Restart required for at least one content mod to take effect.");

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX;
    if (!ImGui::BeginTable("mods", 7, kFlags, ImVec2(0, 360))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 28);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 170);
    ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 300);
    ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 70);
    ImGui::TableSetupColumn("Deep Desert", ImGuiTableColumnFlags_WidthFixed, 90);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableHeadersRow();

    for (const Entry& e : entries) {
        ImGui::PushID(e.manifest.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        // The checkbox reflects the user's persisted preference, not the (possibly frozen-until-restart)
        // session state: for a content mod mid-session those can briefly disagree (state == RestartRequired).
        auto pref = Live().enabled.find(e.manifest.id);
        bool on = pref != Live().enabled.end() ? pref->second : e.state == mods::State::Enabled;
        if (ImGui::Checkbox("##on", &on)) SetEnabled(e.manifest.id, on);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.manifest.name.empty() ? e.manifest.id.c_str() : e.manifest.name.c_str());
        if (e.manifest.implicit) {
            ImGui::SameLine();
            ImGui::TextDisabled("(M1)");
        }
        ImGui::TableNextColumn();
        ImGui::TextColored(StateColor(e.state), "%s", StateLabel(e.state));
        ImGui::PushTextWrapPos(0.f);
        if (!e.reason.empty()) ImGui::TextDisabled("%s", e.reason.c_str());
        sandbox::ModStatus st;
        if (sandbox::Status(e.manifest.id.c_str(), &st)) {
            if (!st.error.empty()) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", st.error.c_str());
            if (st.disabledCallbacks)
                ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "%u callback(s) disabled after faults", st.disabledCallbacks);
            if (st.loaded) ImGui::TextDisabled("%u callbacks, %.2f ms last frame", st.callbacks, st.msLastFrame);
        }
        ImGui::PopTextWrapPos();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(e.manifest.version.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(KindLabel(e.manifest));
        ImGui::TableNextColumn();
        if (e.manifest.unsafe) {
            ImVec4 col = e.deepDesertGranted ? ImVec4(1.f, 0.75f, 0.3f, 1.f) : ImVec4(0.6f, 0.6f, 0.6f, 1.f);
            ImGui::TextColored(col, "%s", e.deepDesertGranted ? "granted" : "not granted");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Deep Desert: raw memory read/write and calling internal game functions.");
        } else {
            ImGui::TextDisabled("-");
        }
        ImGui::TableNextColumn();
        if (ImGui::SmallButton("Open folder")) OpenFolder(e.dir);
        if (!e.manifest.content) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reload")) sandbox::ReloadMod(e.manifest.id.c_str());
        }
        if (e.manifest.unsafe) {
            ImGui::SameLine();
            const char* label = e.deepDesertGranted ? "Revoke" : "Review";
            if (ImGui::SmallButton(label)) {
                if (e.deepDesertGranted)
                    SetDeepDesert(e.manifest.id, false);
                else
                    RequestConsent(e.manifest.id);
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    DrawConsentModals();
}

void DrawDeepDesertPanel(void*) {
    std::vector<Entry> entries = Snapshot();
    bool any = false;
    if (ImGui::Button("Revoke all")) {
        for (const Entry& e : entries)
            if (e.manifest.unsafe) SetDeepDesert(e.manifest.id, false);
    }
    ImGui::Separator();
    for (const Entry& e : entries) {
        if (!e.manifest.unsafe) continue;
        any = true;
        ImGui::PushID(e.manifest.id.c_str());
        ImGui::Text("%s", e.manifest.name.empty() ? e.manifest.id.c_str() : e.manifest.name.c_str());
        ImGui::SameLine();
        ImGui::TextColored(e.deepDesertGranted ? ImVec4(1.f, 0.75f, 0.3f, 1.f) : ImVec4(0.6f, 0.6f, 0.6f, 1.f), "%s",
                            e.deepDesertGranted ? "granted" : "not granted");
        ImGui::SameLine();
        if (e.deepDesertGranted) {
            if (ImGui::SmallButton("Revoke")) SetDeepDesert(e.manifest.id, false);
        } else {
            if (ImGui::SmallButton("Review")) RequestConsent(e.manifest.id);
        }
        ImGui::PopID();
    }
    if (!any) ImGui::TextDisabled("No mod has ever requested Deep Desert access.");
    DrawConsentModals();
}
}  // namespace

void RegisterPanels() {
    overlay::AddPanel("thumper.mods", "Thumper/Mods", &DrawModsPanel, nullptr);
    overlay::AddPanel("thumper.deepdesert", "Thumper/Deep Desert", &DrawDeepDesertPanel, nullptr);
}
}  // namespace melange::thumper
