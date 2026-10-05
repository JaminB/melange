// Overlay panel "Mirage/Post-FX": the effect stack per stage, order, parameters and timings.
#include <imgui.h>

#include <algorithm>

#include "render/mirage/postfx_internal.h"

namespace melange::mirage::postfx {
namespace {
void Move(const std::vector<Effect*>& list, size_t i, int dir) {
    size_t j = dir < 0 ? i - 1 : i + 1;
    Effect& a = *list[i];
    Effect& b = *list[j];
    int oa = a.order, ob = b.order;
    if (oa == ob) {
        melange::postfx::SetOrder(a.id.c_str(), ob + dir);
    } else {
        melange::postfx::SetOrder(a.id.c_str(), ob);
        melange::postfx::SetOrder(b.id.c_str(), oa);
    }
}

void Params(Effect& e) {
    for (size_t k = 0; k < e.desc.params.size() && k < e.values.size(); ++k) {
        const ParamDesc& p = e.desc.params[k];
        if (p.hidden) continue;
        float v[4];
        std::copy(e.values[k].begin(), e.values[k].end(), v);
        bool changed = false;
        const char* label = p.label.c_str();
        ImGui::PushID(static_cast<int>(k));
        switch (p.type) {
        case ParamType::Float:
        case ParamType::Vec2:
        case ParamType::Vec3:
            changed = p.hasRange ? ImGui::SliderScalarN(label, ImGuiDataType_Float, v, p.n, &p.min, &p.max, "%.3f")
                                 : ImGui::DragScalarN(label, ImGuiDataType_Float, v, p.n, 0.01f, nullptr, nullptr, "%.3f");
            break;
        case ParamType::Color:
            changed = p.n == 4 ? ImGui::ColorEdit4(label, v) : ImGui::ColorEdit3(label, v);
            break;
        case ParamType::Int: {
            int i = static_cast<int>(v[0]);
            int lo = static_cast<int>(p.min), hi = static_cast<int>(p.max);
            changed = p.hasRange ? ImGui::SliderInt(label, &i, lo, hi) : ImGui::DragInt(label, &i);
            v[0] = static_cast<float>(i);
            break;
        }
        case ParamType::Bool: {
            bool b = v[0] != 0.f;
            changed = ImGui::Checkbox(label, &b);
            v[0] = b ? 1.f : 0.f;
            break;
        }
        }
        if (changed) melange::postfx::SetParam(e.id.c_str(), p.name.c_str(), v, p.n);
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) melange::postfx::SetParam(e.id.c_str(), p.name.c_str(), p.def, p.n);
        ImGui::PopID();
    }
}

void Row(const std::vector<Effect*>& list, size_t i) {
    Effect& e = *list[i];
    ImGui::PushID(e.id.c_str());
    bool on = e.enabled;
    if (ImGui::Checkbox("##on", &on)) melange::postfx::SetEnabled(e.id.c_str(), on);
    ImGui::SameLine();
    ImGui::BeginDisabled(i == 0);
    if (ImGui::ArrowButton("##up", ImGuiDir_Up)) Move(list, i, -1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(i + 1 == list.size());
    if (ImGui::ArrowButton("##down", ImGuiDir_Down)) Move(list, i, +1);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("%s", e.desc.title.empty() ? e.id.c_str() : e.desc.title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s  order %d%s", e.id.c_str(), e.order, e.code ? "  (code)" : "");
    if (e.enabled && !e.failed && !e.missing) {
        ImGui::SameLine();
        if (e.gpuMs >= 0) ImGui::Text("GPU %.3f ms  CPU %.3f ms", e.gpuMs, e.cpuMs);
        else ImGui::Text("CPU %.3f ms", e.cpuMs);
    }
    if (e.missing || e.failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
        ImGui::TextWrapped("%s: %s", e.missing ? "missing" : "failed", e.error.c_str());
        ImGui::PopStyleColor();
    } else if (!e.skipReason.empty()) {
        ImGui::TextDisabled("skipped: %s", e.skipReason.c_str());
    }
    const bool anyShown = std::any_of(e.desc.params.begin(), e.desc.params.end(), [](const ParamDesc& p) { return !p.hidden; });
    if (anyShown && !e.missing && ImGui::TreeNode("Parameters")) {
        Params(e);
        ImGui::TreePop();
    }
    ImGui::PopID();
}
}  // namespace

void DrawPanel(void*) {
    melange::postfx::Stats st = melange::postfx::GetStats();
    bool bypass = Bypassed(), split = SplitCompare();
    if (ImGui::Checkbox("Bypass all", &bypass)) SetBypassed(bypass);
    ImGui::SameLine();
    if (ImGui::Checkbox("Split compare", &split)) SetSplitCompare(split);
    ImGui::SetItemTooltip("Left half of the screen without the effects");
    ImGui::SameLine();
    if (ImGui::Button("Reload all")) melange::postfx::Reload();
    ImGui::Text("GPU %.3f ms  CPU %.3f ms  passes %u  effects %u", st.gpuMs, st.cpuMs, st.activePasses, st.effects);
    if (st.bypassReason) ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "Bypassed: %s", st.bypassReason);

    std::lock_guard lk(Mutex());
    for (Stage s : {Stage::PostWorld, Stage::Final}) {
        ImGui::SeparatorText(s == Stage::PostWorld ? "PostWorld (world only, under the HUD)" : "Final (whole frame)");
        std::vector<Effect*> list;
        for (auto& e : Effects())
            if (e->stage == s) list.push_back(e.get());
        std::sort(list.begin(), list.end(), [](const Effect* a, const Effect* b) {
            return a->order != b->order ? a->order < b->order : a->id < b->id;
        });
        if (list.empty()) ImGui::TextDisabled("no effects");
        for (size_t i = 0; i < list.size(); ++i) Row(list, i);
    }
}
}  // namespace melange::mirage::postfx
