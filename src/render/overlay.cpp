// Overlay (component A, docs/m0-design.md section 3.A): Dear ImGui on the Present path, panel / menu / hotkey
// registration (wumfix/overlay.h), GL state guard (gl_guard.*), input capture (input.cpp).
//
// Present path: events::SetPresentHook(&OnPresent) runs inside the existing gdi32!SwapBuffers IAT hook, after the
// Frame subscribers and right before the real SwapBuffers, on the main thread with the game's context current
// (no second hook at 0x795540). On the first Present with a context the ImGui context and both backends are
// created and GlInfo is captured. While the overlay is hidden a frame costs a couple of branches.
//
// States: hidden (no capture, only hotkeys are swallowed) -> ToggleKey -> visible + capturing; PassthroughKey ->
// visible without capture (drawn, not interactive, the game gets all input). ToggleKey from pass-through makes
// the overlay interactive; ToggleKey while interactive hides it; PassthroughKey while in pass-through hides it.
//
// Context changes: if wglGetCurrentContext() or the window differ from the ones at init, both backends are shut
// down without deleting GL objects, re-initialised, the window is re-subclassed and Stats.contextResets counts it.
// Any fault inside the ImGui frame or the draw disables the overlay with an error in the log (the game goes on).
#include <windows.h>
#include <GL/gl.h>

#include <imgui.h>
#include <imgui_impl_opengl2.h>
#include <imgui_impl_win32.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "render/backend.h"
#include "render/gl_guard.h"
#include "render/input_logic.h"
#include "render/internal.h"
#include "render/selftest.h"
#include "wumfix/overlay.h"
#include "wumfix/testcmd.h"

namespace wf::render {
// SEH wrapper shared by the overlay's translation units (no C++ objects in this frame).
bool SehCall(void (*fn)(void*), void* arg, unsigned long* code) {
    __try {
        fn(arg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (code) *code = GetExceptionCode();
        return false;
    }
}
}  // namespace wf::render

namespace {
using wf::render::SehCall;

// ---------------------------------------------------------------- registry (any thread)
struct Panel {
    int handle;
    std::string id, title, label;  // label = "title###id" (the ImGui window id follows `id`, not the title)
    wf::overlay::DrawFn fn;
    void* user;
    bool open;
    bool settingsApplied = false;
    bool disabled = false;
    bool hasRect = false;
    float x = 0, y = 0, w = 0, h = 0;
};
struct MenuEntry {
    int handle;
    std::vector<std::string> segs;
    std::string shortcut;
    wf::overlay::ActionFn fn;
    void* user;
};
std::mutex g_regMx;
std::vector<Panel> g_panels;
std::vector<MenuEntry> g_menu;
int g_nextHandle = 1;
std::recursive_mutex g_drawMx;  // held around each panel's DrawFn so RemovePanel can wait for a running draw

// ---------------------------------------------------------------- state
std::atomic<bool> g_visible{false}, g_capture{false}, g_installed{false};
bool g_initTried = false;  // main thread
std::atomic<bool> g_disabled{false};  // set on a fault or failed init; the overlay stays hidden and captures nothing
HGLRC g_ctx = nullptr;
HWND g_hwnd = nullptr;
bool g_appliedCapture = false, g_appliedVisible = false;
bool g_demo = false, g_verify = false;
uint8_t g_toggleDik = 0x29, g_toggleMods = 0, g_passDik = 0x29, g_passMods = wf::render::kModShift;

std::mutex g_glMx;
wf::overlay::GlInfo g_gl;

std::mutex g_statMx;
wf::overlay::Stats g_stats{};
double g_samples[240] = {};
int g_sampleCount = 0, g_sampleIdx = 0;
uint32_t g_verifyChecks = 0, g_glErrors = 0, g_mismatchLogs = 0;
uint64_t g_lastVerifyLog = 0;

// fps from the engine frame counter (sampled by the About panel and the stats verb)
std::atomic<double> g_fps{0};
uint64_t g_fpsTick = 0, g_fpsFrame = 0;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ---------------------------------------------------------------- panel open state in imgui.ini
// [WUMFixPanel][<id>]  Open=0|1   (main thread: ImGui calls these from NewFrame/EndFrame)
std::map<std::string, bool> g_savedOpen;

void* SettingsReadOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) { return &g_savedOpen[name]; }
void SettingsReadLine(ImGuiContext*, ImGuiSettingsHandler*, void* entry, const char* line) {
    int v = 0;
    if (sscanf(line, "Open=%d", &v) == 1) *static_cast<bool*>(entry) = v != 0;
}
void SettingsWriteAll(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    std::lock_guard lk(g_regMx);
    for (const Panel& p : g_panels) g_savedOpen[p.id] = p.open;
    for (const auto& [id, open] : g_savedOpen) buf->appendf("[%s][%s]\nOpen=%d\n\n", h->TypeName, id.c_str(), open ? 1 : 0);
}
void OnImGuiCreated() {
    ImGuiSettingsHandler h;
    h.TypeName = "WUMFixPanel";
    h.TypeHash = ImHashStr("WUMFixPanel");
    h.ReadOpenFn = SettingsReadOpen;
    h.ReadLineFn = SettingsReadLine;
    h.WriteAllFn = SettingsWriteAll;
    ImGui::AddSettingsHandler(&h);
}

// ---------------------------------------------------------------- menu bar
struct Clicked {
    wf::overlay::ActionFn fn = nullptr;
    void* user = nullptr;
    std::string what;
};

void DrawMenuLevel(const std::vector<const MenuEntry*>& items, size_t depth, Clicked& clicked) {
    std::vector<std::string> seen;
    for (const MenuEntry* e : items) {
        const std::string& name = e->segs[depth];
        if (e->segs.size() == depth + 1) {
            if (ImGui::MenuItem(name.c_str(), e->shortcut.empty() ? nullptr : e->shortcut.c_str())) {
                clicked.fn = e->fn;
                clicked.user = e->user;
                clicked.what = name;
            }
            continue;
        }
        if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
        seen.push_back(name);
        std::vector<const MenuEntry*> sub;
        for (const MenuEntry* f : items)
            if (f->segs.size() > depth + 1 && f->segs[depth] == name) sub.push_back(f);
        if (ImGui::BeginMenu(name.c_str())) {
            DrawMenuLevel(sub, depth + 1, clicked);
            ImGui::EndMenu();
        }
    }
}

void DrawMenuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    Clicked clicked;
    {
        std::vector<MenuEntry> copy;
        {
            std::lock_guard lk(g_regMx);
            copy = g_menu;
        }
        std::vector<const MenuEntry*> items;
        for (const MenuEntry& e : copy) items.push_back(&e);
        DrawMenuLevel(items, 0, clicked);
    }
    // "View" (merges with user items under View/...): panel toggles and overlay mode
    if (ImGui::BeginMenu("View")) {
        {
            std::lock_guard lk(g_regMx);
            for (Panel& p : g_panels) {
                if (ImGui::MenuItem(p.title.c_str(), nullptr, &p.open)) ImGui::MarkIniSettingsDirty();
            }
        }
        ImGui::Separator();
        std::string pass = wf::render::HotkeyText(1), toggle = wf::render::HotkeyText(0);
        if (ImGui::MenuItem("Pass-through (game keeps input)", pass.c_str())) wf::overlay::SetCapture(false);
        if (ImGui::MenuItem("Hide overlay", toggle.c_str())) wf::overlay::SetVisible(false);
        ImGui::EndMenu();
    }
    char right[96];
    snprintf(right, sizeof(right), "WUMFix  |  %s: close", wf::render::HotkeyText(0).c_str());
    float w = ImGui::CalcTextSize(right).x + ImGui::GetStyle().ItemSpacing.x * 2;
    if (ImGui::GetContentRegionAvail().x > w) {
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - w);
        ImGui::TextDisabled("%s", right);
    }
    ImGui::EndMainMenuBar();
    if (clicked.fn) {
        WF_INFO("[overlay] menu: %s", clicked.what.c_str());
        unsigned long code = 0;
        if (!SehCall(clicked.fn, clicked.user, &code))
            WF_ERROR("[overlay] menu item '%s' raised exception 0x%08lx", clicked.what.c_str(), code);
    }
}

// ---------------------------------------------------------------- panels
struct DrawCall {
    wf::overlay::DrawFn fn;
    void* user;
};
void RunDraw(void* p) {
    auto* d = static_cast<DrawCall*>(p);
    d->fn(d->user);
}

void DrawPanels() {
    struct Item {
        int handle;
        std::string label;
        wf::overlay::DrawFn fn;
        void* user;
        int index;
        bool hasRect;
        float x, y, w, h;
    };
    std::vector<Item> items;
    {
        std::lock_guard lk(g_regMx);
        int index = 0;
        for (Panel& p : g_panels) {
            if (!p.settingsApplied) {
                p.settingsApplied = true;
                auto it = g_savedOpen.find(p.id);
                if (it != g_savedOpen.end()) p.open = it->second;
            }
            if (p.open && !p.disabled) items.push_back({p.handle, p.label, p.fn, p.user, index, p.hasRect, p.x, p.y, p.w, p.h});
            ++index;
        }
    }
    for (const Item& it : items) {
        std::lock_guard draw(g_drawMx);
        {
            std::lock_guard lk(g_regMx);  // removed by an earlier panel this frame?
            bool alive = std::any_of(g_panels.begin(), g_panels.end(), [&](const Panel& p) { return p.handle == it.handle; });
            if (!alive) continue;
        }
        if (it.hasRect) {
            ImGui::SetNextWindowPos(ImVec2(it.x, it.y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(it.w, it.h), ImGuiCond_FirstUseEver);
        } else {
            float off = 60.f + 28.f * static_cast<float>(it.index % 10);
            ImGui::SetNextWindowPos(ImVec2(off + 120.f, off), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(440, 320), ImGuiCond_FirstUseEver);
        }
        bool open = true;
        bool expanded = ImGui::Begin(it.label.c_str(), &open);
        bool ok = true;
        unsigned long code = 0;
        if (expanded) {
            ImGuiErrorRecoveryState st;
            ImGui::ErrorRecoveryStoreState(&st);
            DrawCall dc{it.fn, it.user};
            ok = SehCall(&RunDraw, &dc, &code);
            if (!ok) ImGui::ErrorRecoveryTryToRecoverWindowState(&st);
        }
        ImGui::End();
        if (!open || !ok) {
            std::lock_guard lk(g_regMx);
            for (Panel& p : g_panels) {
                if (p.handle != it.handle) continue;
                if (!open) {
                    p.open = false;
                    ImGui::MarkIniSettingsDirty();
                }
                if (!ok) {
                    p.disabled = true;
                    WF_ERROR("[overlay] panel '%s' raised exception 0x%08lx while drawing; panel disabled", p.id.c_str(), code);
                }
            }
        }
    }
}

// ---------------------------------------------------------------- frame
void ApplyInputMode() {
    ImGuiIO& io = ImGui::GetIO();
    bool cap = wf::overlay::Capturing();
    bool justShown = !g_appliedVisible;
    g_appliedVisible = true;
    if (!justShown && cap == g_appliedCapture) return;
    g_appliedCapture = cap;
    io.MouseDrawCursor = cap;
    const ImGuiConfigFlags passive = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    if (cap)
        io.ConfigFlags &= ~passive;
    else
        io.ConfigFlags |= passive;
    io.AddFocusEvent(false);  // drop buttons/keys held from before (a stale click must not fire)
    if (cap) io.AddFocusEvent(true);
}

void BuildFrame(void*) {
    ImGui_ImplOpenGL2_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawMenuBar();
    DrawPanels();
    wf::render::DrawBuiltinExtras();
    ImGui::Render();
}

void RenderDrawData(void*) { ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData()); }

void Disable(const char* why, unsigned long code) {
    g_disabled = true;
    wf::render::SetImGuiInputReady(false);
    g_visible = false;
    g_capture = false;
    WF_ERROR("[overlay] %s (exception 0x%08lx): overlay disabled for this session, the game continues", why, code);
}

void CaptureGlInfo(HWND hwnd) {
    wf::overlay::GlInfo gi;
    auto str = [](GLenum e) {
        const char* s = reinterpret_cast<const char*>(glGetString(e));
        return std::string(s ? s : "");
    };
    gi.vendor = str(GL_VENDOR);
    gi.renderer = str(GL_RENDERER);
    gi.version = str(GL_VERSION);
    int major = 0;
    sscanf(gi.version.c_str(), "%d", &major);
    if (major >= 2) gi.glsl = str(0x8B8C);  // GL_SHADING_LANGUAGE_VERSION
    GLint vp[4] = {};
    glGetIntegerv(GL_VIEWPORT, vp);
    gi.viewportW = vp[2];
    gi.viewportH = vp[3];
    gi.valid = !gi.version.empty();
    wf::render::gl::DrainErrors();
    {
        std::lock_guard lk(g_glMx);
        g_gl = gi;
    }
    WF_INFO("[overlay] GL: %s | %s | %s | GLSL %s | viewport %dx%d | window %p", gi.vendor.c_str(), gi.renderer.c_str(),
            gi.version.c_str(), gi.glsl.empty() ? "-" : gi.glsl.c_str(), gi.viewportW, gi.viewportH,
            static_cast<void*>(hwnd));
}

bool InitBackends(HWND hwnd) {
    std::string ini = Utf8(wf::game::DataDir() + L"\\imgui.ini");
    if (!wf::render::backend::Init(hwnd, ini.c_str(), &OnImGuiCreated)) {
        WF_ERROR("[overlay] ImGui backend init failed (window %p): overlay disabled", static_cast<void*>(hwnd));
        g_disabled = true;
        return false;
    }
    wf::render::SetImGuiInputReady(true);
    g_appliedVisible = false;  // re-apply the input mode on the next visible frame
    return true;
}

void FirstPresent(HDC dc) {
    HGLRC ctx = wglGetCurrentContext();
    if (!ctx) return;  // try again next frame
    g_initTried = true;
    HWND hwnd = WindowFromDC(dc);
    if (!hwnd) hwnd = static_cast<HWND>(wf::events::GameWindow());
    CaptureGlInfo(hwnd);
    wf::render::SubclassGameWindow(hwnd);
    g_ctx = ctx;
    g_hwnd = hwnd;
    if (InitBackends(hwnd)) WF_INFO("[overlay] ImGui %s ready (opengl2 + win32 backends), ini %s", IMGUI_VERSION, ImGui::GetIO().IniFilename ? ImGui::GetIO().IniFilename : "-");
}

void HandleContextChange(HGLRC ctx, HWND hwnd) {
    const bool lost = ctx != g_ctx;
    WF_WARN("[overlay] %s changed (context %p -> %p, window %p -> %p): re-initialising the ImGui backends",
            lost ? "GL context" : "game window", static_cast<void*>(g_ctx), static_cast<void*>(ctx),
            static_cast<void*>(g_hwnd), static_cast<void*>(hwnd));
    wf::render::SetImGuiInputReady(false);
    wf::render::backend::Shutdown(lost);
    wf::render::gl::Reset();
    wf::render::SubclassGameWindow(hwnd);
    g_ctx = ctx;
    g_hwnd = hwnd;
    {
        std::lock_guard lk(g_statMx);
        ++g_stats.contextResets;
    }
    // A wrapper that presents from a different context/window every frame would make this thrash: give up instead.
    static uint64_t windowStart = 0;
    static int inWindow = 0;
    uint64_t now = GetTickCount64();
    if (now - windowStart > 10000) {
        windowStart = now;
        inWindow = 0;
    }
    if (++inWindow > 3) {
        Disable("GL context or window changed more than 3 times in 10 s", 0);
        return;
    }
    if (InitBackends(hwnd)) {
        GLint vp[4] = {};
        glGetIntegerv(GL_VIEWPORT, vp);
        std::lock_guard lk(g_glMx);
        g_gl.viewportW = vp[2];
        g_gl.viewportH = vp[3];
    }
}

void RecordTiming(double us) {
    std::lock_guard lk(g_statMx);
    g_stats.lastUs = us;
    ++g_stats.frames;
    g_samples[g_sampleIdx] = us;
    g_sampleIdx = (g_sampleIdx + 1) % 240;
    if (g_sampleCount < 240) ++g_sampleCount;
    if (g_stats.frames % 30 == 0 || g_sampleCount < 30) {
        double tmp[240];
        std::copy(g_samples, g_samples + g_sampleCount, tmp);
        size_t k = static_cast<size_t>(0.95 * (g_sampleCount - 1));
        std::nth_element(tmp, tmp + k, tmp + g_sampleCount);
        g_stats.p95Us = tmp[k];
    }
}

void UpdateFps() {
    uint64_t now = GetTickCount64(), frame = wf::events::FrameCount();
    if (!g_fpsTick) {
        g_fpsTick = now;
        g_fpsFrame = frame;
        return;
    }
    if (now - g_fpsTick < 1000) return;
    g_fps = static_cast<double>(frame - g_fpsFrame) * 1000.0 / static_cast<double>(now - g_fpsTick);
    g_fpsTick = now;
    g_fpsFrame = frame;
}

void RenderOverlay(HDC dc) {
    HGLRC ctx = wglGetCurrentContext();
    if (!ctx) return;
    HWND hwnd = WindowFromDC(dc);
    if (!hwnd) hwnd = g_hwnd;
    if (ctx != g_ctx || hwnd != g_hwnd) HandleContextChange(ctx, hwnd);
    if (g_disabled || !wf::render::backend::Ready() || IsIconic(hwnd)) return;

    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    ApplyInputMode();

    static uint32_t verifyCounter = 0;
    const bool verify = g_verify && (++verifyCounter % 60 == 0);
    wf::render::gl::Snapshot before;
    if (verify) before = wf::render::gl::Read();

    unsigned long code = 0;
    if (!SehCall(&BuildFrame, nullptr, &code)) {
        Disable("fault while building the ImGui frame", code);
        return;
    }
    int setupErr = 0, restoreErr = 0;
    bool drawn = false;
    {
        wf::render::gl::Guard guard;
        if (guard.Ok()) {
            drawn = SehCall(&RenderDrawData, nullptr, &code);
            setupErr = guard.SetupErrors();
        } else if (guard.Fbo()) {
            static bool logged = false;
            if (!logged) {
                logged = true;
                WF_WARN("[overlay] framebuffer object %d bound at Present: overlay frame skipped (never rebinds; logged once)",
                        guard.Fbo());
            }
        }
        guard.Restore();  // pops everything even after a fault in the draw
        restoreErr = guard.RestoreErrors();
        if (guard.Ok() && !drawn) Disable("fault while drawing", code);
    }
    if (setupErr || restoreErr) {
        static int logged = 0;
        if (logged++ < 10)
            WF_WARN("[overlay] glGetError during the overlay frame: %d while neutralising, %d while drawing/restoring",
                    setupErr, restoreErr);
    }
    if (verify) {
        wf::render::gl::Snapshot after = wf::render::gl::Read();
        std::string diff;
        int d = wf::render::gl::Diff(before, after, &diff);
        uint32_t checks, mismatches;
        {
            std::lock_guard lk(g_statMx);
            ++g_verifyChecks;
            if (d) ++g_stats.stateMismatches;
            if (setupErr || restoreErr) ++g_glErrors;
            checks = g_verifyChecks;
            mismatches = g_stats.stateMismatches;
        }
        if (d && g_mismatchLogs++ < 20) WF_WARN("[overlay] VerifyState: %d value(s) changed across the overlay draw: %s", d, diff.c_str());
        uint64_t now = GetTickCount64();
        if (now - g_lastVerifyLog >= 10000 || checks == 1) {
            g_lastVerifyLog = now;
            WF_INFO("[overlay] VerifyState: checks=%u stateMismatches=%u frames with glGetError=%u (values/check=%d)",
                    checks, mismatches, g_glErrors, before.n);
        }
    }
    if (verify) return;  // read-back frames are test-only and would skew the timing
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    RecordTiming(static_cast<double>(t1.QuadPart - t0.QuadPart) * 1e6 / static_cast<double>(freq.QuadPart));
}

void OnPresent(void* hdc) {
    if (wf::render::HotkeysPending()) wf::render::RunPendingHotkeys();
    if (!g_initTried) FirstPresent(static_cast<HDC>(hdc));
    if (!g_visible.load(std::memory_order_relaxed)) {
        g_appliedVisible = false;
        return;
    }
    if (g_disabled) return;
    UpdateFps();
    RenderOverlay(static_cast<HDC>(hdc));
}

// ---------------------------------------------------------------- hotkey actions
void ToggleAction(void*) {
    if (!wf::overlay::Visible())
        wf::overlay::SetVisible(true);
    else if (!wf::overlay::Capturing())
        wf::overlay::SetCapture(true);
    else
        wf::overlay::SetVisible(false);
}

void PassthroughAction(void*) {
    if (wf::overlay::Visible() && !wf::overlay::Capturing()) {
        wf::overlay::SetVisible(false);
    } else {
        wf::overlay::SetVisible(true);
        wf::overlay::SetCapture(false);
    }
}

// ---------------------------------------------------------------- test verbs
bool VerbShow(std::string_view args, void*) {
    wf::overlay::SetVisible(true);
    if (args == "passthrough") wf::overlay::SetCapture(false);
    return true;
}
bool VerbHide(std::string_view, void*) {
    wf::overlay::SetVisible(false);
    return true;
}
bool VerbToggle(std::string_view, void*) {
    ToggleAction(nullptr);
    return true;
}
bool VerbCapture(std::string_view args, void*) {
    if (args != "0" && args != "1") return false;
    wf::overlay::SetCapture(args == "1");
    return true;
}
bool VerbStats(std::string_view, void*) {
    wf::overlay::Stats s = wf::overlay::GetStats();
    wf::overlay::GlInfo g = wf::overlay::Gl();
    wf::render::InputStats in = wf::render::GetInputStats();
    WF_INFO("[overlay] stats: installed=%d visible=%d capturing=%d frames=%llu lastUs=%.1f p95Us=%.1f stateMismatches=%u "
            "contextResets=%u fps=%.1f | GL valid=%d %s | %s | %s | viewport %dx%d",
            wf::overlay::Installed(), wf::overlay::Visible(), wf::overlay::Capturing(),
            static_cast<unsigned long long>(s.frames), s.lastUs, s.p95Us, s.stateMismatches, s.contextResets,
            wf::render::Fps(), g.valid, g.vendor.c_str(), g.renderer.c_str(), g.version.c_str(), g.viewportW, g.viewportH);
    WF_INFO("[overlay] input: diHooked=%d cursorHooked=%d subclassed=%d polls=%llu keysDropped=%llu synthetic=%llu "
            "keyMsgsDropped=%llu mouseMsgsDropped=%llu cursorBlocked=%llu hotkeys=%llu",
            in.diHooked, in.cursorHooked, in.subclassed, static_cast<unsigned long long>(in.diPolls),
            static_cast<unsigned long long>(in.keysDropped), static_cast<unsigned long long>(in.syntheticReleases),
            static_cast<unsigned long long>(in.keyMsgsDropped), static_cast<unsigned long long>(in.mouseMsgsDropped),
            static_cast<unsigned long long>(in.cursorPosBlocked), static_cast<unsigned long long>(in.hotkeysFired));
    return true;
}
bool VerbSelftest(std::string_view, void*) {
    std::string report;
    int failed = wf::render::RunLogicSelfTests(&report);
    size_t p = 0;
    while (p < report.size()) {
        size_t nl = report.find('\n', p);
        std::string line = report.substr(p, nl == std::string::npos ? std::string::npos : nl - p);
        if (failed)
            WF_ERROR("[overlay] %s", line.c_str());
        else
            WF_INFO("[overlay] %s", line.c_str());
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    WF_INFO("[overlay] SELFTEST %s", failed ? "FAIL" : "PASS");
    return failed == 0;
}

// ---------------------------------------------------------------- module
class Overlay final : public wf::Module {
public:
    const char* Name() const override { return "Overlay"; }
    const char* Description() const override { return "ImGui overlay (hidden until the toggle key), panels, input capture"; }
    int Order() const override { return 35; }  // after Automation (30): our DirectInput hooks wrap its injection

    bool Install() override {
        g_demo = Bool("Demo", false);
        g_verify = Bool("VerifyState", false);
        ParseKey("ToggleKey", "GRAVE", &g_toggleDik, &g_toggleMods);
        ParseKey("PassthroughKey", "Shift+GRAVE", &g_passDik, &g_passMods);
        if (g_toggleDik == g_passDik && g_toggleMods == g_passMods)
            WF_WARN("[overlay] ToggleKey and PassthroughKey are the same key; pass-through is unreachable");
        if (wf::config::GetBool("Probe", "Enabled", false))
            WF_WARN("[overlay] [Probe] Enabled=1: Probe and Overlay both hook input and Present; do not run them together");

        wf::render::InstallInput();
        wf::overlay::AddHotkey(g_toggleDik, g_toggleMods, &ToggleAction, nullptr);
        wf::overlay::AddHotkey(g_passDik, g_passMods, &PassthroughAction, nullptr);
        wf::render::RegisterBuiltinPanels(g_demo);

        wf::testcmd::Register("overlay.show", &VerbShow);
        wf::testcmd::Register("overlay.hide", &VerbHide);
        wf::testcmd::Register("overlay.toggle", &VerbToggle);
        wf::testcmd::Register("overlay.capture", &VerbCapture);
        wf::testcmd::Register("overlay.stats", &VerbStats);
        wf::testcmd::Register("overlay.selftest", &VerbSelftest);

        wf::events::SetPresentHook(&OnPresent);
        g_installed = true;
        WF_INFO("[overlay] installed, hidden (ToggleKey=%s PassthroughKey=%s Demo=%d VerifyState=%d)",
                wf::render::HotkeyText(0).c_str(), wf::render::HotkeyText(1).c_str(), g_demo, g_verify);
        return true;
    }

    void Uninstall() override {
        wf::events::SetPresentHook(nullptr);
        g_installed = false;
    }

private:
    void ParseKey(const char* key, const char* def, uint8_t* dik, uint8_t* mods) const {
        wf::config::EnsureKey(Name(), key, def);
        std::string v = wf::config::GetString(Name(), key, def);  // inline '; comments' already stripped
        if (!wf::render::ParseHotkeyText(v.c_str(), dik, mods)) {
            WF_WARN("[overlay] %s=%s is not a valid hotkey, using %s", key, v.c_str(), def);
            wf::render::ParseHotkeyText(def, dik, mods);
        }
    }
};
}  // namespace

WUMFIX_MODULE(Overlay);

// ---------------------------------------------------------------- internal
namespace wf::render {
std::string HotkeyText(int which) {
    return which == 0 ? HotkeyLabel(g_toggleDik, g_toggleMods) : HotkeyLabel(g_passDik, g_passMods);
}
double Fps() { return g_fps.load(); }
bool VerifyStateOn() { return g_verify; }
void SetPanelDefaultRect(int handle, float x, float y, float w, float h) {
    std::lock_guard lk(g_regMx);
    for (Panel& p : g_panels) {
        if (p.handle != handle) continue;
        p.hasRect = true;
        p.x = x;
        p.y = y;
        p.w = w;
        p.h = h;
    }
}
}  // namespace wf::render

// ---------------------------------------------------------------- public API (wumfix/overlay.h)
namespace wf::overlay {
int AddPanel(const char* id, const char* title, DrawFn fn, void* user, uint32_t flags) {
    if (!id || !*id || !fn) return 0;
    std::lock_guard lk(g_regMx);
    for (const Panel& p : g_panels) {
        if (p.id == id) {
            WF_WARN("[overlay] AddPanel: id '%s' already registered", id);
            return 0;
        }
    }
    Panel p;
    p.handle = g_nextHandle++;
    p.id = id;
    p.title = title && *title ? title : id;
    p.label = p.title + "###" + p.id;
    p.fn = fn;
    p.user = user;
    p.open = (flags & kPanelOpenByDefault) != 0;
    g_panels.push_back(std::move(p));
    return g_panels.back().handle;
}

void RemovePanel(int handle) {
    {
        std::lock_guard lk(g_regMx);
        auto it = std::find_if(g_panels.begin(), g_panels.end(), [&](const Panel& p) { return p.handle == handle; });
        if (it == g_panels.end()) return;
        g_panels.erase(it);  // its open state stays in imgui.ini for a later registration with the same id
    }
    // Wait for a draw of this panel in progress on the main thread (recursive: a panel may remove itself).
    std::lock_guard draw(g_drawMx);
}

int AddMenuItem(const char* path, ActionFn fn, void* user, const char* shortcut) {
    if (!fn) return 0;
    MenuEntry e;
    if (!wf::render::SplitMenuPath(path, e.segs)) return 0;
    e.fn = fn;
    e.user = user;
    e.shortcut = shortcut ? shortcut : "";
    std::lock_guard lk(g_regMx);
    e.handle = g_nextHandle++;
    g_menu.push_back(std::move(e));
    return g_menu.back().handle;
}

bool Visible() { return g_visible.load(); }

void SetVisible(bool v) {
    if (v && g_disabled) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) WF_WARN("[overlay] disabled for this session (see the error above); not showing it");
        return;
    }
    bool was = g_visible.exchange(v);
    g_capture = v;  // showing turns capture on, hiding turns it off
    if (was != v) WF_INFO("[overlay] %s", v ? "shown (capturing input)" : "hidden");
}

bool Capturing() {
    return g_visible.load(std::memory_order_relaxed) && g_capture.load(std::memory_order_relaxed) &&
           !g_disabled.load(std::memory_order_relaxed);
}

void SetCapture(bool on) {
    bool was = g_capture.exchange(on);
    if (was != on && g_visible) WF_INFO("[overlay] capture %s", on ? "on" : "off (pass-through)");
}

GlInfo Gl() {
    std::lock_guard lk(g_glMx);
    return g_gl;
}

Stats GetStats() {
    std::lock_guard lk(g_statMx);
    return g_stats;
}

bool Installed() { return g_installed.load(); }
}  // namespace wf::overlay
