// Built-in overlay panels: "About / Stats" (open by default; its first widget is the "Test" button used by the
// capture acceptance test, which logs "overlay: test button") and the Dear ImGui demo window behind
// [Overlay] Demo=1.
#include <imgui.h>

#include "core/game.h"
#include "core/log.h"
#include "render/internal.h"
#include "version.h"
#include "melange/overlay.h"

namespace {
bool g_demoEnabled = false;
bool g_demoOpen = false;

void DrawAbout(void*) {
    if (ImGui::Button("Test")) WF_INFO("overlay: test button");
    ImGui::SameLine();
    ImGui::TextDisabled("logs 'overlay: test button'");

    ImGui::Text("Melange %s  |  Dear ImGui %s", MELANGE_VERSION, IMGUI_VERSION);
    const melange::game::ExeInfo& exe = melange::game::Exe();
    ImGui::Text("Game build: %s%s", exe.build, exe.known ? "" : " (unrecognised exe)");
    ImGui::Text("Engine fps: %.1f", melange::render::Fps());
    ImGui::Separator();

    if (ImGui::CollapsingHeader("OpenGL", ImGuiTreeNodeFlags_DefaultOpen)) {
        melange::overlay::GlInfo gl = melange::overlay::Gl();
        ImGui::Text("Vendor:   %s", gl.vendor.c_str());
        ImGui::Text("Renderer: %s", gl.renderer.c_str());
        ImGui::Text("Version:  %s", gl.version.c_str());
        ImGui::Text("GLSL:     %s", gl.glsl.empty() ? "-" : gl.glsl.c_str());
        ImGui::Text("Viewport: %d x %d", gl.viewportW, gl.viewportH);
    }
    if (ImGui::CollapsingHeader("Overlay", ImGuiTreeNodeFlags_DefaultOpen)) {
        melange::overlay::Stats s = melange::overlay::GetStats();
        ImGui::Text("Frame cost: %.0f us (p95 %.0f us) over %llu frames", s.lastUs, s.p95Us,
                    static_cast<unsigned long long>(s.frames));
        ImGui::Text("State mismatches: %u%s   Context resets: %u", s.stateMismatches,
                    melange::render::VerifyStateOn() ? "" : " (VerifyState=0)", s.contextResets);
        ImGui::Text("Mode: %s", melange::overlay::Capturing() ? "interactive (input captured)" : "pass-through");
    }
    if (ImGui::CollapsingHeader("Input")) {
        melange::render::InputStats in = melange::render::GetInputStats();
        ImGui::Text("DirectInput keyboard hooked: %s   SetCursorPos hooked: %s   window subclassed: %s",
                    in.diHooked ? "yes" : "no", in.cursorHooked ? "yes" : "no", in.subclassed ? "yes" : "no");
        ImGui::Text("Keys dropped: %llu   synthetic releases: %llu   hotkeys fired: %llu",
                    static_cast<unsigned long long>(in.keysDropped), static_cast<unsigned long long>(in.syntheticReleases),
                    static_cast<unsigned long long>(in.hotkeysFired));
        ImGui::Text("Window messages dropped: %llu mouse, %llu key   cursor re-centres blocked: %llu",
                    static_cast<unsigned long long>(in.mouseMsgsDropped), static_cast<unsigned long long>(in.keyMsgsDropped),
                    static_cast<unsigned long long>(in.cursorPosBlocked));
        ImGui::SeparatorText("Hotkeys");
        ImGui::Text("%s  toggle overlay", melange::render::HotkeyText(0).c_str());
        ImGui::Text("%s  pass-through (drawn, game keeps input)", melange::render::HotkeyText(1).c_str());
        for (const melange::render::HotkeyInfo& h : melange::render::ListHotkeys()) ImGui::BulletText("#%d %s", h.handle, h.label.c_str());
    }
    if (g_demoEnabled) ImGui::Checkbox("Show Dear ImGui demo", &g_demoOpen);
}

void ToggleDemo(void*) { g_demoOpen = !g_demoOpen; }
}  // namespace

namespace melange::render {
void RegisterBuiltinPanels(bool demo) {
    g_demoEnabled = demo;
    g_demoOpen = demo;
    int about = melange::overlay::AddPanel("melange.about", "About / Stats", &DrawAbout, nullptr, melange::overlay::kPanelOpenByDefault);
    SetPanelDefaultRect(about, 24.f, 40.f, 460.f, 360.f);  // fixed spot: the Test button's centre is at about (50, 76)
    if (demo) melange::overlay::AddMenuItem("View/Dear ImGui demo", &ToggleDemo, nullptr);
}

void DrawBuiltinExtras() {
    if (g_demoEnabled && g_demoOpen) ImGui::ShowDemoWindow(&g_demoOpen);
}
}  // namespace melange::render
