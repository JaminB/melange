// Overlay panel "Thumper/Lobby": each lobby member's handshake status, the clone weapons column and the joiner's
// clone-lobby modal.
#include <imgui.h>

#include <cstdint>
#include <string>

#include "melange/mods.h"
#include "melange/overlay.h"
#include "mods/handshake_internal.h"
#include "mods/lobby.h"
#include "mods/weapon_gate.h"

namespace melange::handshake {
namespace {
constexpr ImVec4 kAmber = {0.95f, 0.55f, 0.2f, 1.f};

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
    case mods::PeerStatus::Mismatch: return kAmber;
    default: return {0.7f, 0.7f, 0.7f, 1.f};
    }
}

std::string WeaponsCell(uint64_t member) {
    std::string hash;
    uint32_t n = 0;
    if (!ParseWpnValue(lobby::MemberData(member, "mlg.wpn"), &hash, &n)) return "-";
    return std::to_string(n) + " " + hash;
}

void DrawCloneModal(const wpngate::View& v) {
    if (wpngate::TakeModalRequest()) ImGui::OpenPopup("Clone weapons");
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Clone weapons", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("%s", v.why.empty() ? "This lobby uses clone weapons your mods do not match." : v.why.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("Clone weapons stay off for everyone while your mods differ. You can stay, or leave and "
                       "rejoin with the host's mods.");
    ImGui::Spacing();
    const bool canLeave = wpngate::LeaveAvailable();
    if (!canLeave) ImGui::BeginDisabled();
    if (ImGui::Button("Leave lobby", ImVec2(140, 0))) {
        wpngate::RequestLeave();
        wpngate::ModalClosed();
        ImGui::CloseCurrentPopup();
    }
    if (!canLeave) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Stay", ImVec2(100, 0))) {
        wpngate::ModalClosed();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawPanel(void*) {
    mods::ContentId ours = mods::LocalContent();
    const wpngate::View wv = wpngate::Current();
    DrawCloneModal(wv);
    if (!ours.vanilla && !mods::SimAllowedThisMatch())
        ImGui::TextColored(kAmber, "Content mods are off for this online match: lobby content does not match.");
    if (wv.hostHeld) {
        ImGui::TextColored(kAmber, "%s", wv.refusing ? "Clone weapons: the match start is held until every player has the same mods."
                                                     : "Clone weapons will be off this match.");
        for (const auto& m : wv.members) ImGui::BulletText("%s", m.c_str());
    } else if (!wv.why.empty()) {
        ImGui::TextColored(kAmber, "%s", wv.why.c_str());
    }
    ImGui::Text("Our content: %s", ours.vanilla ? "vanilla" : ours.hash);
    if (lobby::Current() && wpngate::LocalClones()) ImGui::Text("Our clone weapons: %s", WeaponsCell(lobby::Me()).c_str());

    mods::Peer peers[64];
    int n = mods::Peers(peers, 64);
    if (n == 0) {
        ImGui::TextDisabled("Not in a lobby.");
        return;
    }
    if (ImGui::BeginTable("peers", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Status");
        ImGui::TableSetupColumn("Version");
        ImGui::TableSetupColumn("Hash");
        ImGui::TableSetupColumn("Weapons");
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
                const std::string msg = PeerMsgDiff(p.steamId);
                if (!diff.empty()) ImGui::TextWrapped("%s", diff.c_str());
                if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::Text("%s", p.version[0] ? p.version : "-");
            ImGui::TableNextColumn();
            ImGui::Text("%s", p.hash16[0] ? p.hash16 : "-");
            ImGui::TableNextColumn();
            ImGui::Text("%s", WeaponsCell(p.steamId).c_str());
        }
        ImGui::EndTable();
    }
}

}  // namespace

void RegisterPanel() { overlay::AddPanel("thumper.lobby", "Thumper/Lobby", &DrawPanel, nullptr); }

}  // namespace melange::handshake
