// Overlay panel "Wormsign/Replay": arm a recording, then pause, change speed, run to a tick, restart or disarm.
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/game.h"
#include "melange/overlay.h"
#include "melange/wormsign.h"
#include "wormsign/player.h"

namespace melange::wormsign::player {
namespace {
constexpr const char* kCompName[kEngineComps] = {"time+rng", "turn", "worms", "tasks", "projectiles", "teams"};
constexpr float kSpeeds[] = {0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f};

char g_file[260] = "";
char g_err[128] = "";
int g_runTo = 0;
bool g_showComps = false;
std::vector<ReplayInfo> g_lib;
uint64_t g_libFrame = 0;

const char* StateName(PlayState s) {
    switch (s) {
        case PlayState::Idle: return "Idle";
        case PlayState::Armed: return "Armed";
        case PlayState::Loading: return "Loading";
        case PlayState::Playing: return "Playing";
        case PlayState::Paused: return "Paused";
        case PlayState::Finished: return "Finished";
        case PlayState::Diverged: return "Diverged";
        case PlayState::Failed: return "Failed";
    }
    return "?";
}

void DoArm(const std::wstring& path) {
    g_err[0] = 0;
    Arm(path.c_str(), g_err, sizeof g_err);
}

void DrawIdle() {
    ImGui::TextWrapped("Arm a recording from the main menu, then start a Quick Game: the match replays the recorded "
                       "one and every tick is checked against it.");
    if (++g_libFrame % 60 == 1) {
        g_lib.resize(64);
        g_lib.resize(static_cast<size_t>((std::max)(0, Library(g_lib.data(), 64))));
    }
    if (!g_lib.empty() && ImGui::BeginTable("lib", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp |
                                                          ImGuiTableFlags_ScrollY, ImVec2(0, 160))) {
        ImGui::TableSetupColumn("Recording");
        ImGui::TableSetupColumn("Land");
        ImGui::TableSetupColumn("Ticks");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < g_lib.size(); ++i) {
            const ReplayInfo& r = g_lib[i];
            const std::string name = game::Narrow(r.path);
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(name.substr(name.find_last_of("\\/") + 1).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.land);
            ImGui::TableNextColumn();
            ImGui::Text("%u%s", r.ticks, r.complete ? "" : " (incomplete)");
            ImGui::TableNextColumn();
            if (r.online) ImGui::TextDisabled("online");
            else if (ImGui::SmallButton("Arm")) DoArm(r.path);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(-60);
    ImGui::InputTextWithHint("##file", "file name or path (.wsr)", g_file, sizeof g_file);
    ImGui::SameLine();
    if (ImGui::Button("Arm") && g_file[0]) DoArm(game::Widen(g_file));
}

void DrawControls(const PlayStatus& s, const Info& info) {
    const bool live = s.state == PlayState::Playing || s.state == PlayState::Paused || s.state == PlayState::Diverged ||
                      s.state == PlayState::Loading;
    if (live) {
        const bool paused = s.state == PlayState::Paused;
        if (ImGui::Button(paused ? "Resume" : "Pause")) SetPaused(!paused);
        ImGui::SameLine();
    }
    if (live || s.state == PlayState::Armed) {
        for (float x : kSpeeds) {
            char label[16];
            snprintf(label, sizeof label, "%gx", static_cast<double>(x));
            const bool on = s.speed == x;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(label)) SetSpeed(x);
            if (on) ImGui::PopStyleColor();
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##runto", &g_runTo, 50, 500);
        if (g_runTo < 0) g_runTo = 0;
        ImGui::SameLine();
        if (ImGui::Button("Run to tick") && g_runTo > 0) {
            if (!RunTo(static_cast<uint32_t>(g_runTo)))
                snprintf(g_err, sizeof g_err, "tick %d is behind: use Restart and run to it", g_runTo);
        }
        if (info.runTo) {
            ImGui::SameLine();
            ImGui::TextDisabled("running to %u", info.runTo);
        }
    }
    if (s.state != PlayState::Armed) {
        if (ImGui::Button("Restart")) Restart(static_cast<uint32_t>(g_runTo), g_err, sizeof g_err);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Arms the recording again (after this match ends) and runs to the tick above, if set");
        ImGui::SameLine();
    }
    if (ImGui::Button("Disarm")) Disarm();
}

void DrawDivergence() {
    DivergenceDetail dd;
    if (!LastDivergence(&dd)) return;
    ImGui::Separator();
    ImGui::TextColored({0.95f, 0.45f, 0.3f, 1.f}, "Diverged at tick %u", dd.d.tick);
    ImGui::SameLine();
    ImGui::Checkbox("diff", &g_showComps);
    if (!g_showComps) return;
    if (ImGui::BeginTable("comps", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Component");
        ImGui::TableSetupColumn("Recorded");
        ImGui::TableSetupColumn("Replay");
        ImGui::TableHeadersRow();
        auto row = [](const char* name, uint64_t a, uint64_t b) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (a != b) ImGui::TextColored({0.95f, 0.45f, 0.3f, 1.f}, "%s", name);
            else ImGui::TextUnformatted(name);
            ImGui::TableNextColumn();
            ImGui::Text("%016llx", a);
            ImGui::TableNextColumn();
            ImGui::Text("%016llx", b);
        };
        row("engine", dd.recorded.engine, dd.live.engine);
        for (int i = 0; i < kEngineComps; ++i) row(kCompName[i], dd.recorded.c[i], dd.live.c[i]);
        row("mods", dd.recorded.mods, dd.live.mods);
        row("logic rng", dd.recorded.rngLogic, dd.live.rngLogic);
        ImGui::EndTable();
    }
}

void DrawPanel(void*) {
    const PlayStatus s = Status();
    const Info info = GetInfo();
    if (!Enabled()) {
        ImGui::TextDisabled("Wormsign is off.");
        return;
    }
    if (s.state == PlayState::Idle) {
        DrawIdle();
    } else {
        const std::string name = game::Narrow(info.path);
        ImGui::Text("%s", name.substr(name.find_last_of("\\/") + 1).c_str());
        ImGui::Text("State: %s", StateName(s.state));
        if (s.state == PlayState::Armed) ImGui::TextWrapped("Start a Quick Game from the main menu.");
        const float frac = s.ticks ? static_cast<float>(s.tick) / static_cast<float>(s.ticks) : 0.f;
        char over[64];
        snprintf(over, sizeof over, "tick %u / %u", s.tick, s.ticks);
        ImGui::ProgressBar(frac > 1.f ? 1.f : frac, ImVec2(-1, 0), over);
        ImGui::Text("Compared %u, matched %u", s.compared, s.matched);
        ImGui::Text("Inputs %zu / %zu", info.nextInput, info.inputs);
        DrawControls(s, info);
        if (info.restartPending) ImGui::TextWrapped("Restart pending: quit to the main menu, then start the Quick Game again.");
    }
    if (s.error[0]) ImGui::TextColored({0.95f, 0.55f, 0.2f, 1.f}, "%s", s.error);
    if (g_err[0]) ImGui::TextColored({0.95f, 0.55f, 0.2f, 1.f}, "%s", g_err);
    if (!info.note.empty()) ImGui::TextWrapped("%s", info.note.c_str());
    DrawDivergence();
}
}  // namespace

void RegisterPanel() { overlay::AddPanel("wormsign.replay", "Wormsign/Replay", &DrawPanel, nullptr); }
}  // namespace melange::wormsign::player
