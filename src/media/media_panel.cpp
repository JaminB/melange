// Overlay panel "Melange/Media": the media pack's state, version and folder, the installed yt-dlp, which mod asked for
// the download, and the "Uninstall media pack" button. There is no install button: the consent is the user's click in
// the mod's own panel (wum.media.install).
#include <imgui.h>

#include <cstdio>

#include "media/pack.h"
#include "media/pack_internal.h"
#include "melange/overlay.h"

namespace melange::media::pack::panel {
namespace {
const ImVec4 kAmber(1.0f, 0.75f, 0.3f, 1.0f);
bool g_confirm = false;

void Draw(void*) {
    const Info i = Get();
    ImGui::Text("Media pack %s: %s", i.version.c_str(), StateName(i.state));
    if (i.state == State::Downloading || i.state == State::Verifying || i.state == State::Installing || i.state == State::Updating) {
        char overlay[64];
        snprintf(overlay, sizeof overlay, "%.1f / %.1f MB", i.bytes / 1048576.0, i.total / 1048576.0);
        ImGui::ProgressBar(static_cast<float>(i.progress), ImVec2(-1, 0), i.state == State::Downloading ? overlay : nullptr);
    }
    if (!i.error.empty()) ImGui::TextColored(kAmber, "Last error: %s (details in the log)", i.error.c_str());
    ImGui::Text("yt-dlp: %s", i.ytdlp.empty() ? "not installed" : i.ytdlp.c_str());
    ImGui::TextWrapped("Folder: %s", core::Narrow(i.folder).c_str());
    ImGui::Text("Size: about %u MB to download, %u MB on disk", i.downloadMB, i.diskMB);
    const std::string by = RequestedBy();
    if (!by.empty()) ImGui::Text("Download requested by: %s", by.c_str());
    ImGui::Spacing();

    const bool busy = i.state == State::Downloading || i.state == State::Verifying || i.state == State::Installing;
    if (busy && ImGui::Button("Cancel download")) CancelInstall();
    if (i.state == State::Ready) {
        if (ImGui::Button("Check for a yt-dlp update")) CheckYtdlpLane(true);
        ImGui::SameLine();
    }
    const bool canRemove = !busy && i.state != State::Disabled && i.state != State::Unsupported && !InUse();
    if (!canRemove) ImGui::BeginDisabled();
    if (!g_confirm) {
        if (ImGui::Button("Uninstall media pack")) g_confirm = true;
    } else {
        if (ImGui::Button("Delete it")) {
            Uninstall();
            g_confirm = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep it")) g_confirm = false;
    }
    if (!canRemove) ImGui::EndDisabled();
    if (InUse()) ImGui::TextColored(kAmber, "A video player is running: close it to uninstall.");
}
}  // namespace

void Register() {
    static bool done = false;
    if (done) return;
    done = true;
    overlay::AddPanel("melange.media", "Melange/Media", &Draw, nullptr);
}
}  // namespace melange::media::pack::panel
