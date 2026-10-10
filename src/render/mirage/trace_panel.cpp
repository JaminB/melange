// Overlay panel "Mirage/GL".
#include <windows.h>
#include <shellapi.h>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <thread>

#include "core/game.h"
#include "core/thread_guard.h"
#include "melange/gltrace.h"
#include "melange/overlay.h"
#include "render/mirage/hub.h"
#include "render/mirage/trace_internal.h"

namespace melange::mirage::trace {
namespace {
void OpenFolder(std::wstring path, bool select) {
    std::thread([path = std::move(path), select] {
        GuardedThreadBody("mirage-trace", [&] {
            std::wstring args = select ? L"/select,\"" + path + L"\"" : L"\"" + path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        });
    }).detach();
}

float Max(const std::vector<float>& v) {
    float m = 1.f;
    for (float x : v) m = std::max(m, x);
    return m;
}

void Draw(void*) {
    gltrace::FrameStats last = gltrace::Last(), avg = gltrace::Average(60);
    bool hub = hub::Installed();
    int mode = static_cast<int>(gltrace::GetMode());
    ImGui::Text("Mode:");
    ImGui::SameLine();
    ImGui::BeginDisabled(!hub);
    bool changed = ImGui::RadioButton("Off", &mode, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Count", &mode, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Log", &mode, 2);
    ImGui::EndDisabled();
    if (changed) gltrace::SetMode(static_cast<gltrace::Mode>(mode));
    if (!hub) ImGui::TextDisabled("The GL hub is off ([MirageTrace] Mode=off at start); set Mode=count and restart.");
    ImGui::SameLine();
    ImGui::TextDisabled("scene: %s", Scene().c_str());

    if (ImGui::BeginTable("stats", 3, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("");
        ImGui::TableSetupColumn("last frame");
        ImGui::TableSetupColumn("avg 60");
        ImGui::TableHeadersRow();
        auto row = [](const char* k, uint32_t a, uint32_t b) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(k);
            ImGui::TableNextColumn();
            ImGui::Text("%u", a);
            ImGui::TableNextColumn();
            ImGui::Text("%u", b);
        };
        row("GL calls", last.calls, avg.calls);
        row("Draw calls", last.draws, avg.draws);
        row("Cg runtime calls", last.cgCalls, avg.cgCalls);
        row("Program switches", last.programSwitches, avg.programSwitches);
        row("Parameter flushes", last.paramFlushes, avg.paramFlushes);
        row("FBO binds", last.fboBinds, avg.fboBinds);
        row("Texture binds", last.texBinds, avg.texBinds);
        row("Texture uploads", last.texUploads, avg.texUploads);
        row("glGetError", last.getErrors, avg.getErrors);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Busy ms");
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", last.busyMs);
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", avg.busyMs);
        ImGui::EndTable();
    }
    if (ImGui::CollapsingHeader("GPU timers", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char* kNames[] = {"Swap-to-swap", "World", "WorldLate", "PostWorld", "Hud", "Final"};
        constexpr int kN = static_cast<int>(gltrace::GpuRegion::Count);
        if (!gltrace::GpuTimerSupported()) {
            ImGui::TextDisabled("not supported by this context (needs GL_ARB_timer_query or GL 3.3+)");
        } else if (ImGui::BeginTable("gputimers", 2, ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("region");
            ImGui::TableSetupColumn("GPU ms");
            ImGui::TableHeadersRow();
            for (int i = 0; i < kN; ++i) {
                gltrace::GpuTime t = gltrace::GetGpuTime(static_cast<gltrace::GpuRegion>(i));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kNames[i]);
                ImGui::TableNextColumn();
                if (t.valid) ImGui::Text("%.3f", t.ms);
                else ImGui::TextDisabled("n/a");
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("verb: gltrace.gpu   ini: [Mirage] GpuTimers=1");
    }
    History h = GetHistory();
    float w = ImGui::GetContentRegionAvail().x;
    if (!h.calls.empty()) {
        ImGui::PlotLines("##calls", h.calls.data(), static_cast<int>(h.calls.size()), 0, "GL calls (240 frames)", 0.f,
                         Max(h.calls) * 1.1f, ImVec2(w, 48));
        ImGui::PlotLines("##busy", h.busyMs.data(), static_cast<int>(h.busyMs.size()), 0, "busy ms", 0.f, Max(h.busyMs) * 1.1f,
                         ImVec2(w, 48));
    }

    if (ImGui::CollapsingHeader("Top 20 functions (per frame)", ImGuiTreeNodeFlags_DefaultOpen)) {
        gltrace::FnStat top[20];
        size_t n = gltrace::Top(top, 20);
        if (ImGui::BeginTable("top", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            for (size_t i = 0; i < n; ++i) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%.1f", top[i].perFrame);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(top[i].name);
                ImGui::TableNextColumn();
                const char* src = top[i].source == gltrace::kExeImport ? "exe" : top[i].source == gltrace::kExeProc ? "exe proc"
                                  : top[i].source == gltrace::kCgGLImport ? "cgGL" : "cgGL proc";
                ImGui::TextDisabled("%s", src);
            }
            ImGui::EndTable();
        }
        ImGui::TextDisabled("%d thunks, %u extension procs handed out", hub::Count(), gltrace::ProcsHandedOut());
    }

    ImGui::Separator();
    std::wstring path;
    std::string err;
    gltrace::CaptureState st = gltrace::CaptureStatus(&path, &err);
    bool busy = st == gltrace::CaptureState::Armed || st == gltrace::CaptureState::Recording || st == gltrace::CaptureState::Writing;
    ImGui::BeginDisabled(!hub || busy);
    if (ImGui::Button("Capture frame")) gltrace::RequestCapture(gltrace::CaptureOptions{});
    ImGui::SameLine();
    if (ImGui::Button("Dump next 50 textures")) gltrace::StartTextureDump(50);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%u textures dumped", gltrace::TexturesDumped());
    static const char* kState[] = {"idle", "armed", "recording", "writing", "done", "failed"};
    ImGui::Text("Capture: %s%s%s", kState[static_cast<int>(st)], err.empty() ? "" : " - ", err.c_str());
    if (!path.empty()) {
        ImGui::TextWrapped("%s", game::Narrow(path).c_str());
        if (ImGui::Button("Open folder")) OpenFolder(path, true);
        ImGui::SameLine();
    }
    if (ImGui::Button("Open textures folder") && !TexdumpDir().empty()) {
        EnsureDir(TexdumpDir());
        OpenFolder(TexdumpDir(), false);
    }
    ImGui::TextWrapped("Captures and texture dumps contain the game's own textures. They stay on this PC and are never "
                       "added to Save-logs exports.");
}
}  // namespace

void RegisterPanel() { overlay::AddPanel("mirage.gl", "Mirage/GL", &Draw, nullptr); }
}  // namespace melange::mirage::trace
