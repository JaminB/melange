// Overlay panel "Mirage/Shaders": programs, reload, parameter sliders, errors and the FXAA switch.
#include <windows.h>
#include <imgui.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "melange/overlay.h"
#include "melange/shaders.h"
#include "render/mirage/engine.h"
#include "render/mirage/shaders_internal.h"

namespace melange::mirage::shaders {
namespace {
struct Rate {
    uint32_t binds = 0;
    uint64_t tick = 0;
    float perSec = 0;
};
std::map<std::string, Rate> g_rates;
std::string g_fxaaWhy;

void DrawToast() {
    uint64_t tick = 0;
    std::string t = LastToast(&tick);
    if (t.empty()) return;
    bool fresh = GetTickCount64() - tick < 8000;
    ImVec4 col = t.find("error") != std::string::npos ? ImVec4(1.f, 0.45f, 0.35f, 1.f) : ImVec4(0.55f, 0.9f, 0.55f, 1.f);
    if (!fresh) col.w = 0.55f;
    ImGui::TextColored(col, "%s", t.c_str());
    if (!fresh) return;
    ImVec2 size = ImGui::CalcTextSize(t.c_str());
    ImVec2 disp = ImGui::GetIO().DisplaySize;
    ImVec2 p((disp.x - size.x) * 0.5f, 36.f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(ImVec2(p.x - 10, p.y - 6), ImVec2(p.x + size.x + 10, p.y + size.y + 6), IM_COL32(20, 20, 24, 230), 6.f);
    dl->AddText(p, ImGui::ColorConvertFloat4ToU32(col), t.c_str());
}

void DrawParams() {
    std::vector<ParamRow> rows = Params();
    if (rows.empty()) {
        ImGui::TextDisabled("No parameters (a mod's shaders\\params.ini adds sliders).");
        return;
    }
    std::string group;
    for (ParamRow& r : rows) {
        std::string g = r.file + ":" + r.entryGlob;
        if (g != group) {
            group = g;
            ImGui::SeparatorText(g.c_str());
        }
        std::string label = (r.label.empty() ? r.name : r.label) + "##" + g + r.name;
        bool changed = false;
        if (r.type == "color") changed = ImGui::ColorEdit3(label.c_str(), r.v, ImGuiColorEditFlags_Float);
        else if (r.hasSpec) changed = ImGui::SliderScalarN(label.c_str(), ImGuiDataType_Float, r.v, r.n, &r.min, &r.max, "%.3f");
        else changed = ImGui::InputScalarN(label.c_str(), ImGuiDataType_Float, r.v, std::min(r.n, 4), nullptr, nullptr, "%.3f");
        if (!r.owner.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (from %s)", r.name.c_str(), r.owner.c_str());
        if (changed) melange::shaders::SetParam(r.file.c_str(), r.entryGlob.c_str(), r.name.c_str(), r.v, r.n);
    }
}

void DrawPrograms() {
    std::vector<melange::shaders::ProgramInfo> list(melange::shaders::ListPrograms(nullptr, 0) + 8);
    list.resize(melange::shaders::ListPrograms(list.data(), list.size()));
    uint64_t now = GetTickCount64();
    std::set<std::string> files;
    for (const auto& p : list) files.insert(p.file);
    ImGui::TextUnformatted("Reload:");
    for (const std::string& f : files) {
        ImGui::SameLine();
        if (ImGui::SmallButton(f.c_str())) melange::shaders::Reload(f.c_str());
    }
    constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                                       ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("programs", 7, kFlags, ImVec2(0, 260))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("File");
    ImGui::TableSetupColumn("Entry");
    ImGui::TableSetupColumn("Stage");
    ImGui::TableSetupColumn("Binds/s");
    ImGui::TableSetupColumn("Owner");
    ImGui::TableSetupColumn("State");
    ImGui::TableSetupColumn("GLSL");
    ImGui::TableHeadersRow();
    for (const auto& p : list) {
        Rate& r = g_rates[std::string(p.file) + ":" + p.entry];
        if (now - r.tick >= 1000) {
            if (r.tick) r.perSec = static_cast<float>(p.binds - r.binds) * 1000.f / static_cast<float>(now - r.tick);
            r.binds = p.binds;
            r.tick = now;
        }
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.file);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.entry);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.stage ? "fragment" : "vertex");
        ImGui::TableNextColumn();
        ImGui::Text("%.0f", r.perSec);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(p.owner);
        ImGui::TableNextColumn();
        if (p.failed) ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "failed ");
        else if (p.pendingReload) ImGui::TextDisabled("reload ");
        if (p.overridden) {
            ImGui::SameLine(0, 0);
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.f, 1.f), "overridden ");
        }
        if (p.glsl) {
            ImGui::SameLine(0, 0);
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 1.f, 1.f), "glsl");
        }
        ImGui::TableNextColumn();
        if (p.glslAvailable) {
            bool on = melange::shaders::GetGlslEnabled(p.file, p.entry);
            std::string id = std::string("##glsl-") + p.file + ":" + p.entry;
            if (ImGui::Checkbox(id.c_str(), &on)) melange::shaders::SetGlslEnabled(p.file, p.entry, on);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("enable/disable this program's GLSL replacement (takes effect at once)");
        } else {
            ImGui::TextDisabled("-");
        }
    }
    ImGui::EndTable();
}

void Draw(void*) {
    melange::shaders::Stats s = melange::shaders::GetStats();
    ImGui::Text("Profiles %s / %s   programs %u   overridden %u   glsl %u   reloads %u   errors %u   passthrough loads %u",
                melange::shaders::Profile(0), melange::shaders::Profile(1), s.programs, s.overridden, s.glsl, s.reloads,
                s.compileErrors, s.passthroughLoads);
    bool fxaa = engine::FxaaOn();
    if (ImGui::Checkbox("FXAA", &fxaa)) {
        g_fxaaWhy.clear();
        FxaaToggle(fxaa, &g_fxaaWhy);
    }
    ImGui::SameLine();
    if (g_fxaaWhy.empty()) ImGui::TextDisabled("the game's own FXAA pass (same as launching with /FXAA)");
    else ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "%s", g_fxaaWhy.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Reload all")) melange::shaders::Reload("");
    DrawToast();
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) DrawParams();
    if (ImGui::CollapsingHeader("Programs", ImGuiTreeNodeFlags_DefaultOpen)) DrawPrograms();
    if (ImGui::CollapsingHeader("Errors")) {
        melange::shaders::CompileError e[64];
        size_t n = melange::shaders::LastErrors(e, 64);
        if (!n) ImGui::TextDisabled("none");
        for (size_t i = n; i-- > 0;) ImGui::TextWrapped("%s(%d): %s  [%s]", e[i].file, e[i].line, e[i].text, e[i].owner);
    }
}
}  // namespace

void RegisterPanel() { melange::overlay::AddPanel("mirage.shaders", "Mirage/Shaders", &Draw, nullptr); }
}  // namespace melange::mirage::shaders
