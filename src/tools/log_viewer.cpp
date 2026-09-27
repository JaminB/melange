// Overlay panels for component C: "Log" (Tail() with filters) and "Events" (top bus ids by rate, with
// allow/deny toggles). Public overlay contract: src/sdk/wumfix/overlay.h (A). See docs/m0-design.md SS3 "C",
// adapter 5. Written against A's frozen header; panels draw only their contents (the overlay owns Begin/End).
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
    // Only the visible rows are submitted (up to 20000 kept lines cost several ms per frame otherwise).
    static std::vector<int> visible;
    visible.clear();
    for (int i = 0; i < static_cast<int>(g_lines.size()); ++i)
        if (PassesFilter(g_lines[i])) visible.push_back(i);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(visible.size()));
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
            const auto& l = g_lines[visible[r]];
            ImGui::PushID(static_cast<int>(l.seq));
            if (ImGui::Selectable(l.json.c_str())) ImGui::SetClipboardText(l.json.c_str());
            ImGui::PopID();
        }
    }
    if (g_autoscroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------- Events panel
struct EventRow {
    wf::bus::MsgId id;
    std::string name;
    uint32_t count;
    double rate;  // messages/s (Post + Deliver) over the last sampling window
};

// Registry ids are 0x8000 | slot (docs/m0-design.md SS1.2), so iterate the registry itself rather than
// 0..Capacity(), plus the system ids the probe saw at runtime. Rates are sampled once a second.
std::vector<EventRow> g_rows;
std::vector<uint32_t> g_prevCount;  // indexed by id & 0x7fff for registry ids
uint32_t g_prevSys[3] = {};
ULONGLONG g_lastSample = 0;
constexpr wf::bus::MsgId kSysIds[3] = {0x103, 0x104, 0x1004};

uint32_t TotalCount(wf::bus::MsgId id) {
    return wf::bus::CountOf(id, wf::bus::Path::Post) + wf::bus::CountOf(id, wf::bus::Path::Deliver);
}

void SampleEvents(size_t max) {
    ULONGLONG now = GetTickCount64();
    if (g_lastSample && now - g_lastSample < 1000) return;
    double dt = g_lastSample ? (now - g_lastSample) / 1000.0 : 0.0;
    g_lastSample = now;
    std::vector<EventRow> rows;
    if (g_prevCount.size() < wf::bus::Capacity()) g_prevCount.resize(wf::bus::Capacity(), 0);
    auto add = [&](wf::bus::MsgId id, const char* name, uint32_t& prev) {
        uint32_t count = TotalCount(id);
        double rate = dt > 0 ? (count - prev) / dt : 0.0;
        prev = count;
        if (count) rows.push_back({id, name, count, rate});
    };
    wf::bus::ForEachName([&](wf::bus::MsgId id, const char* name) {
        size_t slot = id & 0x7fff;
        if (slot < g_prevCount.size()) add(id, name, g_prevCount[slot]);
    });
    for (int i = 0; i < 3; ++i) add(kSysIds[i], wf::bus::NameOf(kSysIds[i]), g_prevSys[i]);
    std::sort(rows.begin(), rows.end(), [](const EventRow& a, const EventRow& b) {
        return a.rate != b.rate ? a.rate > b.rate : a.count > b.count;
    });
    if (rows.size() > max) rows.resize(max);
    g_rows = std::move(rows);
}

void DrawEventsPanel(void*) {
    ImGui::TextUnformatted(wf::bus::RegistryReady() ? "registry ready" : "registry not ready");
    ImGui::Separator();
    SampleEvents(30);
    const auto& rows = g_rows;
    if (ImGui::BeginTable("##events", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Rate/s");
        ImGui::TableSetupColumn("Total");
        ImGui::TableSetupColumn("Allow");
        ImGui::TableSetupColumn("Deny");
        ImGui::TableHeadersRow();
        for (const auto& r : rows) {
            ImGui::PushID(static_cast<int>(r.id));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.1f", r.rate);
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
