// Overlay panel "Thumper/Lobby": each lobby member's handshake status.
#include <imgui.h>

#include <cstdint>
#include <string>

#include "melange/mods.h"
#include "melange/overlay.h"

namespace melange::handshake {
std::string PeerModsDiff(uint64_t steamId);  // handshake.cpp

namespace {

const char* StatusText(mods::PeerStatus s) {
    switch (s) {
    case mods::PeerStatus::Vanilla: return "Vanilla";
    case mods::PeerStatus::MelangeVanilla: return "Melange, no content";
    case mods::PeerStatus::Match: return "Match";
    case mods::PeerStatus::Mismatch: return "Mismatch";
    default: return "Unknown";
    }
}
ImVec4 StatusColor(mods::PeerStatus s) {
    switch (s) {
    case mods::PeerStatus::Match: return {0.4f, 0.9f, 0.4f, 1.f};
    case mods::PeerStatus::Mismatch: return {0.95f, 0.55f, 0.2f, 1.f};
    default: return {0.7f, 0.7f, 0.7f, 1.f};
    }
}

void DrawPanel(void*) {
    mods::ContentId ours = mods::LocalContent();
    if (!ours.vanilla && !mods::SimAllowedThisMatch())
        ImGui::TextColored({0.95f, 0.55f, 0.2f, 1.f},
                            "Content mods are off for this online match: lobby content does not match.");
    ImGui::Text("Our content: %s", ours.vanilla ? "vanilla" : ours.hash);

    mods::Peer peers[64];
    int n = mods::Peers(peers, 64);
    if (n == 0) {
        ImGui::TextDisabled("Not in a lobby.");
        return;
    }
    if (ImGui::BeginTable("peers", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Hash");
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; ++i) {
            const mods::Peer& p = peers[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", p.name[0] ? p.name : "?");
            ImGui::TableNextColumn();
            ImGui::TextColored(StatusColor(p.status), "%s", StatusText(p.status));
            if (p.status == mods::PeerStatus::Mismatch) {
                const std::string diff = PeerModsDiff(p.steamId);
                if (!diff.empty()) ImGui::TextWrapped("%s", diff.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s", p.version[0] ? p.version : "-");
            ImGui::TableNextColumn();
            ImGui::Text("%s", p.hash16[0] ? p.hash16 : "-");
        }
        ImGui::EndTable();
    }
}

}  // namespace

void RegisterPanel() { overlay::AddPanel("thumper.lobby", "Thumper/Lobby", &DrawPanel, nullptr); }

}  // namespace melange::handshake
