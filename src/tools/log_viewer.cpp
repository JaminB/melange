// Overlay panels for component C: "Log" (Tail() with filters) and "Events" (top bus ids by rate, with
// allow/deny toggles). Public overlay contract: src/sdk/wumfix/overlay.h (A). See docs/m0-design.md SS3 "C",
// adapter 5. Written against A's frozen header; A's real backend is not implemented here (see the note at the
// top of src/render/overlay.cpp) so these panels are registered but never actually drawn in this worktree.
#include "tools/log_viewer.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/jlog_bus_filter.h"
#include "wumfix/bus.h"
#include "wumfix/jlog.h"
#include "wumfix/overlay.h"

#include <imgui.h>

namespace wf::logviewer {
namespace {

// ---------------------------------------------------------------------------------------------- Log panel
uint64_t g_afterSeq = 0;
std::vector<wf::jlog::Line> g_lines;
constexpr size_t kMaxKept = 20000;

char g_catFilter[64] = {};
char g_textFilter[128] = {};
int g_minLevel = 0;  // index into kLevelNames
bool g_paused = false;
bool g_autoscroll = true;

const char* kLevelNames[] = {"trace", "debug", "info", "warn", "error", "fatal"};

void Pull() {
    if (g_paused) return;
    std::vector<wf::jlog::Line> fresh;
    wf::jlog::Tail(g_afterSeq, fresh, 4000);
    if (fresh.empty()) return;
    g_afterSeq = fresh.back().seq;
    for (auto& l : fresh) g_lines.push_back(std::move(l));
    if (g_lines.size() > kMaxKept) g_lines.erase(g_lines.begin(), g_lines.begin() + (g_lines.size() - kMaxKept));
}

bool PassesFilter(const wf::jlog::Line& l) {
    if (static_cast<int>(l.lvl) < g_minLevel) return false;
    if (g_catFilter[0] && l.category.find(g_catFilter) == std::string::npos) return false;
    if (g_textFilter[0] && l.json.find(g_textFilter) == std::string::npos) return false;
    return true;
}

void OpenLogFolder() {
    const auto& dir = wf::jlog::CurrentSession().dir;
    if (!dir.empty()) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void DrawLogPanel(void*) {
    Pull();
    ImGui::Checkbox("Pause", &g_paused);
    ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &g_autoscroll);
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) OpenLogFolder();

    ImGui::SetNextItemWidth(120);
    ImGui::Combo("Min level", &g_minLevel, kLevelNames, IM_ARRAYSIZE(kLevelNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140);
    ImGui::InputText("Category", g_catFilter, sizeof(g_catFilter));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Text", g_textFilter, sizeof(g_textFilter));

    ImGui::BeginChild("##loglines", ImVec2(0, 0), true);
    for (const auto& l : g_lines) {
        if (!PassesFilter(l)) continue;
        ImGui::PushID(static_cast<int>(l.seq));
        if (ImGui::Selectable(l.json.c_str())) ImGui::SetClipboardText(l.json.c_str());
        ImGui::PopID();
    }
    if (g_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------- Events panel
struct EventRow {
    wf::bus::MsgId id;
    std::string name;
    uint32_t count;
};

std::vector<EventRow> TopEvents(size_t max) {
    std::vector<EventRow> rows;
    size_t cap = wf::bus::Capacity();
    rows.reserve(cap);
    for (size_t id = 0; id < cap; ++id) {
        const char* name = wf::bus::NameOf(static_cast<wf::bus::MsgId>(id));
        if (!name) continue;
        uint32_t count =
            wf::bus::CountOf(static_cast<wf::bus::MsgId>(id), wf::bus::Path::Post) +
            wf::bus::CountOf(static_cast<wf::bus::MsgId>(id), wf::bus::Path::Deliver);
        if (count == 0) continue;
        rows.push_back({static_cast<wf::bus::MsgId>(id), name, count});
    }
    std::sort(rows.begin(), rows.end(), [](const EventRow& a, const EventRow& b) { return a.count > b.count; });
    if (rows.size() > max) rows.resize(max);
    return rows;
}

void DrawEventsPanel(void*) {
    ImGui::TextUnformatted(wf::bus::RegistryReady() ? "registry ready" : "registry not ready");
    ImGui::Separator();
    auto rows = TopEvents(30);
    if (ImGui::BeginTable("##events", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Count");
        ImGui::TableSetupColumn("Allow");
        ImGui::TableSetupColumn("Deny");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::PushID(static_cast<int>(r.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", r.count);
            ImGui::TableNextColumn();
            bool allow = wf::jlog::busfilter::IsAllowed(r.name);
            if (ImGui::Checkbox("##allow", &allow)) wf::jlog::busfilter::SetAllow(r.name, allow);
            ImGui::TableNextColumn();
            bool deny = wf::jlog::busfilter::IsDenied(r.name);
            if (ImGui::Checkbox("##deny", &deny)) wf::jlog::busfilter::SetDeny(r.name, deny);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

}  // namespace

void Install() {
    wf::overlay::AddPanel("log", "Log", &DrawLogPanel, nullptr, wf::overlay::kPanelNone);
    wf::overlay::AddPanel("events", "Events", &DrawEventsPanel, nullptr, wf::overlay::kPanelNone);
}

}  // namespace wf::logviewer
