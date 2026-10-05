#include <windows.h>
#include <GL/gl.h>
#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "melange/export.h"
#include "melange/gldebug.h"
#include "melange/gltrace.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/postfx.h"
#include "melange/render.h"
#include "melange/shaders.h"
#include "melange/testcmd.h"
#include "render/gl_guard.h"
#include "render/mirage/compat.h"
#include "render/mirage/engine.h"
#include "render/mirage/hub.h"

namespace melange::mirage::compat {
namespace {
constexpr GLenum kNUM_EXTENSIONS = 0x821D, kCONTEXT_FLAGS = 0x821E, kCONTEXT_PROFILE_MASK = 0x9126,
                 kMAX_TEXTURE_UNITS = 0x84E2, kMAX_TEXTURE_IMAGE_UNITS = 0x8872, kSHADING_LANGUAGE_VERSION = 0x8B8C;
constexpr int kCG_GL_VERTEX = 8, kCG_GL_FRAGMENT = 9;
constexpr const char* kProfiles[] = {"arbvp1", "arbfp1", "vp20", "fp20", "vp30", "fp30", "vp40", "fp40", "gp4vp",
                                     "gp4fp", "gp4gp", "gp5vp", "gp5fp", "gp5gp", "glslv", "glslf", "glslg"};

HGLRC g_ctx = nullptr;
Snapshot g_base;  // the per-context part, collected once
bool g_refresh = true;
uint64_t g_lastRefresh = 0;
std::vector<char> g_extFilter(64, 0);
bool g_failedOnly = false;

const char* Str(const GLubyte* s) { return s ? reinterpret_cast<const char*>(s) : ""; }

template <class T>
T CgProc(const wchar_t* dll, const char* name) {
    HMODULE m = GetModuleHandleW(dll);
    return m ? reinterpret_cast<T>(GetProcAddress(m, name)) : nullptr;
}

// Every Cg call drains cgGetError so the engine does not log our errors as its own.
void DrainCg() {
    if (auto err = CgProc<int(__cdecl*)()>(L"cg.dll", "cgGetError")) err();
}

std::string ProfileName(int p) {
    auto str = CgProc<const char*(__cdecl*)(int)>(L"cg.dll", "cgGetProfileString");
    if (!str || !p) return p ? std::to_string(p) : "";
    const char* s = str(p);
    DrainCg();
    return s && *s ? s : std::to_string(p);
}

std::string FileVersion(const wchar_t* dll) {
    HMODULE m = GetModuleHandleW(dll);
    wchar_t path[MAX_PATH];
    if (!m || !GetModuleFileNameW(m, path, MAX_PATH)) return {};
    DWORD h = 0, n = GetFileVersionInfoSizeW(path, &h);
    if (!n) return {};
    std::vector<uint8_t> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path, 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) || !fi)
        return {};
    char s[64];
    snprintf(s, sizeof s, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS),
             LOWORD(fi->dwFileVersionLS));
    return s;
}

std::string RegString(const std::wstring& key, const wchar_t* value) {
    wchar_t buf[256];
    DWORD n = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return {};
    char out[256];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    return out;
}

void CollectDriver(Snapshot& s) {
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i, dd.cb = sizeof dd) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE)) continue;
        char name[128];
        WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, name, sizeof name, nullptr, nullptr);
        s.adapter = name;
        std::wstring key = dd.DeviceKey;
        const std::wstring prefix = L"\\Registry\\Machine\\";
        if (_wcsnicmp(key.c_str(), prefix.c_str(), prefix.size()) != 0) return;
        key = key.substr(prefix.size());
        strncpy_s(s.gpu.driver, RegString(key, L"DriverVersion").c_str(), _TRUNCATE);
        s.driverDate = RegString(key, L"DriverDate");
        s.driverProvider = RegString(key, L"ProviderName");
        return;
    }
}

void Split(const char* list, std::vector<std::string>& out) {
    out.clear();
    for (const char* p = list; p && *p;) {
        while (*p == ' ') ++p;
        const char* e = strchr(p, ' ');
        size_t n = e ? static_cast<size_t>(e - p) : strlen(p);
        if (n) out.emplace_back(p, n);
        p += n;
    }
    std::sort(out.begin(), out.end());
}

void CollectContext(Snapshot& s) {
    s = Snapshot{};
    const render::gl::Caps& caps = render::gl::Load();
    strncpy_s(s.gpu.vendor, Str(glGetString(GL_VENDOR)), _TRUNCATE);
    strncpy_s(s.gpu.renderer, Str(glGetString(GL_RENDERER)), _TRUNCATE);
    strncpy_s(s.gpu.version, Str(glGetString(GL_VERSION)), _TRUNCATE);
    strncpy_s(s.gpu.glsl, Str(glGetString(kSHADING_LANGUAGE_VERSION)), _TRUNCATE);
    const char* ext = Str(glGetString(GL_EXTENSIONS));
    if (*ext) {
        Split(ext, s.extensions);
    } else if (auto gsi = reinterpret_cast<const GLubyte*(WINAPI*)(GLenum, GLuint)>(wglGetProcAddress("glGetStringi"))) {
        GLint n = 0;
        glGetIntegerv(kNUM_EXTENSIONS, &n);
        for (GLint i = 0; i < n; ++i) s.extensions.emplace_back(Str(gsi(GL_EXTENSIONS, static_cast<GLuint>(i))));
        std::sort(s.extensions.begin(), s.extensions.end());
    }
    if (auto wext = reinterpret_cast<const char*(WINAPI*)(HDC)>(wglGetProcAddress("wglGetExtensionsStringARB")))
        Split(wext(wglGetCurrentDC()), s.wglExtensions);
    s.gpu.extensions = static_cast<int>(s.extensions.size());
    if (caps.major >= 3) {
        glGetIntegerv(kCONTEXT_FLAGS, &s.contextFlags);
        GLint mask = 0;
        if (caps.major > 3 || caps.minor >= 2) glGetIntegerv(kCONTEXT_PROFILE_MASK, &mask);
        s.contextProfile = mask & 1 ? "core" : mask & 2 ? "compatibility" : "legacy";
    } else {
        s.contextProfile = "legacy";
    }
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &s.maxTextureSize);
    glGetIntegerv(kMAX_TEXTURE_UNITS, &s.maxTextureUnits);
    if (caps.arbFp || caps.glsl) glGetIntegerv(kMAX_TEXTURE_IMAGE_UNITS, &s.maxTextureImageUnits);
    render::gl::DrainErrors();

    s.cgVersion = FileVersion(L"cg.dll");
    auto latest = CgProc<int(__cdecl*)(int)>(L"cgGL.dll", "cgGLGetLatestProfile");
    auto supported = CgProc<int(__cdecl*)(int)>(L"cgGL.dll", "cgGLIsProfileSupported");
    auto byName = CgProc<int(__cdecl*)(const char*)>(L"cg.dll", "cgGetProfile");
    if (latest) {
        s.cgLatestVertex = ProfileName(latest(kCG_GL_VERTEX));
        DrainCg();
        s.cgLatestFragment = ProfileName(latest(kCG_GL_FRAGMENT));
        DrainCg();
    }
    if (supported && byName)
        for (const char* p : kProfiles) {
            int id = byName(p);
            DrainCg();
            if (id <= 0 || id == 6145) continue;  // CG_PROFILE_UNKNOWN
            bool ok = supported(id) != 0;
            DrainCg();
            s.cgProfiles.emplace_back(p, ok);
        }
    CollectDriver(s);
    s.gpu.valid = true;
}

void CollectDynamic(Snapshot& s) {
    s.frame = events::FrameCount();
    // engine::Check() logs an error on other exe builds, so it is only asked on the known one
    s.engineKnown = game::IsKnownBuild() && engine::Check();
    s.debugContext = gldebug::DebugContext();
    s.hub = hub::Installed();
    gltrace::Mode m = gltrace::GetMode();
    s.traceMode = !s.hub ? "off (no hub)" : m == gltrace::Mode::Log ? "log" : m == gltrace::Mode::Count ? "count" : "off";
    s.programs.clear();
    if (s.engineKnown) {
        s.fxaa = engine::FxaaOn();
        s.msaa = engine::MsaaOn();
        strncpy_s(s.gpu.cgVertex, ProfileName(engine::CgProfile(0)).c_str(), _TRUNCATE);
        strncpy_s(s.gpu.cgFragment, ProfileName(engine::CgProfile(1)).c_str(), _TRUNCATE);
        s.programsLoaded = engine::ShaderMgr() != 0;
    }
    if (s.engineKnown) engine::ForEachCgProg(
        [](const engine::CgProg& c, void* u) {
            auto* rows = static_cast<std::vector<ProgramRow>*>(u);
            ProgramRow r;
            char path[260] = {}, entry[128] = {};
            if (c.path) strncpy_s(path, c.path, _TRUNCATE);
            if (c.entry) strncpy_s(entry, c.entry, _TRUNCATE);
            const char* slash = std::max(strrchr(path, '/'), strrchr(path, '\\'));
            r.file = slash ? slash + 1 : path;
            r.entry = entry;
            r.stage = c.type;
            r.failed = c.failed;
            r.pending = c.reload;
            r.binds = c.binds;
            rows->push_back(std::move(r));
        },
        &s.programs);

    std::vector<shaders::ProgramInfo> infos(512);
    infos.resize(shaders::ListPrograms(infos.data(), infos.size()));
    std::vector<shaders::CompileError> errs(64);
    errs.resize(shaders::LastErrors(errs.data(), errs.size()));
    shaders::Stats ss = shaders::GetStats();
    s.passthroughLoads = ss.passthroughLoads;
    s.overriddenPrograms = ss.overridden;
    s.compileErrors = ss.compileErrors;
    for (ProgramRow& r : s.programs) {
        for (const shaders::ProgramInfo& i : infos)
            if (i.file && i.entry && r.file == i.file && r.entry == i.entry) {
                r.owner = i.owner ? i.owner : "";
                r.overridden = i.overridden;
                r.glsl = i.glsl;
            }
        if (!r.failed) continue;
        for (const shaders::CompileError& e : errs)
            if (e.file && r.file == e.file) {
                char b[192];
                snprintf(b, sizeof b, "%s(%d): %s", e.file, e.line, e.text ? e.text : "");
                r.reason = b;
                break;
            }
        if (r.reason.empty())
            r.reason = "the engine could not compile it for " + std::string(r.stage == 0 ? s.gpu.cgVertex : s.gpu.cgFragment);
    }

    std::vector<postfx::EffectInfo> fx(128);
    fx.resize(postfx::ListEffects(fx.data(), fx.size()));
    s.effects.clear();
    for (const postfx::EffectInfo& e : fx) {
        EffectRow r;
        r.id = e.id ? e.id : "";
        r.title = e.title ? e.title : "";
        r.stage = e.stage == render::Stage::Final ? "Final" : "PostWorld";
        r.enabled = e.enabled;
        r.failed = e.failed;
        s.effects.push_back(std::move(r));
    }
    postfx::Stats ps = postfx::GetStats();
    s.postfxBypassed = ps.bypassed;
    s.bypassReason = ps.bypassReason ? ps.bypassReason : "";

    static const char* kStage[] = {"World", "WorldLate", "PostWorld", "Hud", "Final"};
    s.stages.clear();
    for (int i = 0; i < static_cast<int>(render::Stage::Count); ++i) {
        render::StageInfo si = render::GetStageInfo(static_cast<render::Stage>(i));
        s.stages.push_back({kStage[i], si.bucket, si.post, si.installed, si.calls});
    }
}

void Refresh() {
    HGLRC ctx = wglGetCurrentContext();
    if (ctx && ctx != g_ctx) {
        g_ctx = ctx;
        CollectContext(g_base);
        LOG_INFO("[mirage] GPU: %s | %s | GL %s | driver %s | Cg %s/%s | %d extensions", g_base.gpu.renderer, g_base.gpu.vendor,
                 g_base.gpu.version, g_base.gpu.driver, g_base.cgLatestVertex.c_str(), g_base.cgLatestFragment.c_str(),
                 g_base.gpu.extensions);
    }
    Snapshot s = g_base;
    CollectDynamic(s);
    g_lastRefresh = s.frame;
    g_refresh = false;
    SetSnapshot(std::move(s));
}

void OnFrame() {
    if (g_refresh || events::FrameCount() - g_lastRefresh >= 300) Refresh();
}

const ImVec4 kRed(1.f, 0.45f, 0.4f, 1.f), kGrey(0.6f, 0.6f, 0.6f, 1.f), kGreen(0.5f, 0.85f, 0.5f, 1.f);

void StatusText(const char* status) {
    ImVec4 c = !strcmp(status, "failed") ? kRed : !strcmp(status, "skipped") ? kGrey : kGreen;
    ImGui::TextColored(c, "%s", status);
}

void DrawPanel(void*) {
    if (ImGui::IsWindowAppearing()) g_refresh = true;
    Snapshot s = GetSnapshot();
    std::vector<Entry> reps = Entries();
    if (ImGui::Button("Refresh")) g_refresh = true;
    ImGui::SameLine();
    if (ImGui::Button("Copy report")) ImGui::SetClipboardText(BuildText(s, reps).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Export last game's logs")) exporter::RequestExportLastGame();
    ImGui::SameLine();
    ImGui::TextDisabled("the report is part of every logs zip");

    if (ImGui::CollapsingHeader("GPU and driver", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!s.gpu.valid) ImGui::TextColored(kGrey, "No GL context seen yet.");
        ImGui::Text("Renderer: %s", s.gpu.renderer);
        ImGui::Text("Vendor:   %s", s.gpu.vendor);
        ImGui::Text("Adapter:  %s", s.adapter.c_str());
        ImGui::Text("Driver:   %s  %s", s.gpu.driver, s.driverDate.c_str());
        ImGui::Text("OpenGL:   %s (%s, flags 0x%x)", s.gpu.version, s.contextProfile.c_str(), s.contextFlags);
        ImGui::Text("GLSL:     %s", s.gpu.glsl);
        ImGui::Text("Limits:   max texture %d, units %d, image units %d", s.maxTextureSize, s.maxTextureUnits, s.maxTextureImageUnits);
    }
    if (ImGui::CollapsingHeader("Cg", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("cg.dll %s; the engine compiles for %s / %s (latest %s / %s)", s.cgVersion.c_str(), s.gpu.cgVertex,
                    s.gpu.cgFragment, s.cgLatestVertex.c_str(), s.cgLatestFragment.c_str());
        std::string sup, unsup;
        for (const auto& [name, ok] : s.cgProfiles) (ok ? sup : unsup) += name + " ";
        ImGui::TextWrapped("Supported: %s", sup.empty() ? "-" : sup.c_str());
        ImGui::TextWrapped("Not supported: %s", unsup.empty() ? "-" : unsup.c_str());
        ImGui::Text("FXAA flag %s, MSAA %s, debug context %s, GL hub %s (%s)", s.fxaa ? "on" : "off", s.msaa ? "on" : "off",
                    s.debugContext ? "yes" : "no", s.hub ? "yes" : "no", s.traceMode.c_str());
    }
    uint32_t failed = 0;
    for (const ProgramRow& p : s.programs) failed += p.failed;
    char head[96];
    snprintf(head, sizeof head, "Shaders: %zu programs, %u failed###shaders", s.programs.size(), failed);
    if (ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Failed only", &g_failedOnly);
        ImGui::SameLine();
        ImGui::TextDisabled("%u overridden, %u passthrough loads, %u compile errors", s.overriddenPrograms, s.passthroughLoads,
                            s.compileErrors);
        if (ImGui::BeginTable("progs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit |
                                              ImGuiTableFlags_Resizable, ImVec2(0, 220))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Status");
            ImGui::TableSetupColumn("Stage");
            ImGui::TableSetupColumn("Program");
            ImGui::TableSetupColumn("Owner");
            ImGui::TableSetupColumn("Reason", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const ProgramRow& p : s.programs) {
                if (g_failedOnly && !p.failed) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                StatusText(p.failed ? "failed" : "loaded");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.stage == 0 ? "vertex" : "fragment");
                ImGui::TableNextColumn();
                ImGui::Text("%s:%s", p.file.c_str(), p.entry.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.owner.empty() ? (p.overridden ? "override" : "") : p.owner.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p.reason.c_str());
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Passes and effects", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (s.postfxBypassed) ImGui::TextColored(kRed, "Post-FX bypassed: %s", s.bypassReason.c_str());
        if (s.effects.empty()) ImGui::TextColored(kGrey, "No post-FX effects.");
        for (const EffectRow& e : s.effects) {
            StatusText(e.failed ? "failed" : e.enabled ? "loaded" : "skipped");
            ImGui::SameLine();
            ImGui::Text("%s (%s, %s)", e.id.c_str(), e.title.c_str(), e.stage.c_str());
        }
        for (const StageRow& st : s.stages)
            ImGui::Text("Stage %-9s %s[%d] %s, %llu calls", st.name.c_str(), st.post ? "post" : "pre", st.bucket,
                        st.installed ? "installed" : "not installed", static_cast<unsigned long long>(st.calls));
    }
    snprintf(head, sizeof head, "Reports (%zu)###reports", reps.size());
    if (ImGui::CollapsingHeader(head, ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const Entry& e : reps) {
            StatusText(StatusName(e.status));
            ImGui::SameLine();
            ImGui::Text("%s %s%s%s%s", KindName(e.kind), e.id, *e.owner ? " [" : "", e.owner, *e.owner ? "]" : "");
            if (*e.reason) {
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "- %s", e.reason);
            }
        }
    }
    snprintf(head, sizeof head, "Extensions (%zu GL, %zu WGL)###ext", s.extensions.size(), s.wglExtensions.size());
    if (ImGui::CollapsingHeader(head)) {
        ImGui::InputText("Filter", g_extFilter.data(), g_extFilter.size());
        if (ImGui::BeginChild("extlist", ImVec2(0, 200), ImGuiChildFlags_Borders)) {
            for (const auto* list : {&s.extensions, &s.wglExtensions})
                for (const std::string& e : *list)
                    if (!g_extFilter[0] || e.find(g_extFilter.data()) != std::string::npos) ImGui::TextUnformatted(e.c_str());
        }
        ImGui::EndChild();
    }
}

bool VerbReport(std::string_view, void*) {
    Refresh();
    std::string text = Text(), json = Json();
    size_t start = 0;
    for (int n = 0; n < 12 && start < text.size(); ++n) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) LOG_INFO("[mirage] compat: %s", text.substr(start, end - start).c_str());
        start = end + 1;
    }
    Snapshot s = GetSnapshot();
    uint32_t failed = 0;
    for (const ProgramRow& p : s.programs) failed += p.failed;
    std::wstring path = jlog::CurrentSession().dir + L"\\gpu_compat.json";
    bool ok = false;
    if (FILE* f = _wfopen(path.c_str(), L"wb")) {
        ok = fwrite(json.data(), 1, json.size(), f) == json.size();
        fclose(f);
    }
    jlog::Rec("mirage", jlog::Level::Info, "compat.report")
        .Bool("gl", s.gpu.valid).Str("renderer", s.gpu.renderer).Str("version", s.gpu.version).Str("driver", s.gpu.driver)
        .Str("cgVertex", s.gpu.cgVertex).Str("cgFragment", s.gpu.cgFragment).Int("extensions", s.gpu.extensions)
        .Int("programs", static_cast<int64_t>(s.programs.size())).Uint("failed", failed)
        .Int("effects", static_cast<int64_t>(s.effects.size())).Int("reports", static_cast<int64_t>(Entries().size()))
        .Bool("written", ok);
    return true;
}
}  // namespace

void Install() {
    events::Subscribe(events::Event::Frame, [] { OnFrame(); });
    overlay::AddPanel("mirage.gpu", "Mirage/GPU", &DrawPanel, nullptr);
    testcmd::Register("compat.report", &VerbReport);
}
}  // namespace melange::mirage::compat
