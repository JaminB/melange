// Module "MiragePostFX": post-processing effect stack at the PostWorld and Final stages.
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "render/mirage/engine.h"
#include "render/mirage/modfs.h"
#include "render/mirage/postfx_internal.h"
#include "render/mirage/stages.h"
#include "melange/gldebug.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/postfx.h"
#include "melange/render.h"
#include "melange/testcmd.h"

namespace pfx = melange::mirage::postfx;
using melange::render::Stage;

namespace {
constexpr const char* kSection = "MiragePostFX";
constexpr int kStageOrder = 1 << 20;  // after every other callback at the same stage
constexpr Stage kStages[2] = {Stage::PostWorld, Stage::Final};

std::recursive_mutex g_mx;
std::vector<std::shared_ptr<pfx::Effect>> g_effects;
std::vector<std::shared_ptr<pfx::Effect>> g_graveyard;  // removed code passes whose GL names wait for the main thread
std::vector<pfx::StackEntry> g_persist;
std::map<std::string, std::string> g_pendingParams;
bool g_stackDirty = false;
uint64_t g_lastFlush = 0;

bool g_installed = false, g_bypass = false, g_split = false, g_gpuTimers = true, g_unsupported = false;
bool g_noStagesLogged = false;
int g_handles[2] = {};
std::atomic<bool> g_rescan{false};
int g_nextHandle = 1;
LARGE_INTEGER g_t0{}, g_freq{};
std::string g_reason;
uint32_t g_passes[2] = {};
int g_errorLogs = 0;
struct FormatCache {
    unsigned tex = 0;
    int w = 0, h = 0;
    bool rgba8 = false;
} g_fmt;

int StageIndex(Stage s) { return s == Stage::Final ? 1 : 0; }

pfx::Effect* Find(const char* id) {
    if (!id) return nullptr;
    for (auto& e : g_effects)
        if (e->id == id) return e.get();
    return nullptr;
}

pfx::StackEntry* Persisted(const std::string& id) {
    for (pfx::StackEntry& s : g_persist)
        if (s.id == id) return &s;
    return nullptr;
}

void Persist(const pfx::Effect& e) {
    if (pfx::StackEntry* s = Persisted(e.id)) {
        s->order = e.order;
        s->enabled = e.enabled;
    } else {
        g_persist.push_back({e.id, e.order, e.enabled});
    }
    g_stackDirty = true;
}

std::string ReadStack() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetPrivateProfileStringW(L"MiragePostFX", L"Stack", L"", buf.data(), static_cast<DWORD>(buf.size()),
                                       melange::config::Path().c_str());
    buf.resize(n);
    std::string s = melange::game::Narrow(buf);
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == ';' && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            s.resize(i);
            break;
        }
    return s;
}

void FlushPersist(bool force) {
    uint64_t now = GetTickCount64();
    if (!force && now - g_lastFlush < 500) return;
    g_lastFlush = now;
    if (g_stackDirty) {
        melange::config::SetString(kSection, "Stack", pfx::FormatStack(g_persist).c_str());
        g_stackDirty = false;
    }
    for (auto& [key, value] : g_pendingParams) {
        melange::config::SetString(kSection, key.c_str(), value.c_str());
        LOG_INFO("[postfx] param %s = %s", key.c_str(), value.c_str());
    }
    g_pendingParams.clear();
}

void ApplyPersisted(pfx::Effect& e) {
    if (const pfx::StackEntry* s = Persisted(e.id)) {
        e.order = s->order;
        e.enabled = s->enabled;
    } else if (!e.code) {
        e.order = e.desc.order;
        e.enabled = e.desc.enabled;
    }
}

bool ReadText(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(h, nullptr), got = 0;
    bool ok = size != INVALID_FILE_SIZE && size < (1u << 20);
    if (ok) {
        out->resize(size);
        ok = ReadFile(h, out->data(), size, &got, nullptr) && got == size;
    }
    CloseHandle(h);
    return ok;
}

void Reparse(pfx::Effect& e) {
    e.reparse = false;
    std::string text, err;
    pfx::EffectDesc d;
    if (!ReadText(e.dir + L"\\effect.ini", &text)) {
        e.missing = true;
        e.error = "effect.ini not found";
        return;
    }
    if (!pfx::ParseEffect(text, &d, &err)) {
        e.failed = true;
        e.dirty = false;
        e.error = err;
        LOG_ERROR("[postfx] %s: %s", e.id.c_str(), err.c_str());
        melange::jlog::Rec("shader", melange::jlog::Level::Error, "postfx effect.ini error").Str("id", e.id).Str("error", err);
        return;
    }
    std::vector<std::array<float, 4>> values(d.params.size());
    for (size_t i = 0; i < d.params.size(); ++i) {
        const pfx::ParamDesc& p = d.params[i];
        std::copy(p.def, p.def + 4, values[i].begin());
        bool kept = false;
        for (size_t k = 0; k < e.desc.params.size() && k < e.values.size(); ++k)
            if (e.desc.params[k].name == p.name && e.desc.params[k].n == p.n) {
                values[i] = e.values[k];
                kept = true;
            }
        if (kept) continue;
        std::string v = melange::config::GetString(kSection, (e.id + "." + p.name).c_str(), "");
        float f[4];
        int n = pfx::ParseFloats(v, f, 4);
        for (int k = 0; k < n && k < p.n; ++k) values[i][k] = f[k];
    }
    e.desc = std::move(d);
    e.values = std::move(values);
    e.stage = e.desc.stage;
    e.missing = false;
    e.failed = false;
    e.error.clear();
    e.dirty = true;
    ApplyPersisted(e);
}

void Scan() {
    std::lock_guard lk(g_mx);
    std::vector<bool> seen(g_effects.size(), false);
    melange::mirage::modfs::Root roots[128];
    int n = melange::mirage::modfs::Roots(roots, 128);
    for (int r = 0; r < n; ++r) {
        std::wstring base = std::wstring(roots[r].dir) + L"\\postfx";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((base + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::wstring dir = base + L"\\" + fd.cFileName;
            if (GetFileAttributesW((dir + L"\\effect.ini").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            std::string folder = melange::game::Narrow(fd.cFileName);
            std::string id = std::string(roots[r].id) + "/" + folder;
            pfx::Effect* e = Find(id.c_str());
            if (!e) {
                auto ne = std::make_shared<pfx::Effect>();
                ne->id = id;
                ne->reparse = true;
                g_effects.push_back(ne);
                seen.push_back(true);
                e = ne.get();
            } else {
                for (size_t i = 0; i < g_effects.size(); ++i)
                    if (g_effects[i].get() == e) seen[i] = true;
                if (e->code) continue;
                if (e->missing) e->reparse = true;
            }
            e->owner = roots[r].id;
            e->folder = folder;
            e->dir = dir;
            if (e->reparse) Reparse(*e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (size_t i = 0; i < g_effects.size(); ++i) {
        pfx::Effect& e = *g_effects[i];
        if (seen[i] || e.code || e.missing) continue;
        e.missing = true;
        e.error = "effect folder not found";
        LOG_WARN("[postfx] %s: effect folder is gone (kept in Melange.ini)", e.id.c_str());
    }
}

void OnFileChange(const wchar_t* path, void*) {
    std::wstring p(path);
    std::wstring lower = p;
    for (wchar_t& c : lower) c = towlower(c);
    size_t at = lower.find(L"\\postfx\\");
    if (at == std::wstring::npos) return;
    size_t ownerStart = p.rfind(L'\\', at - 1);
    size_t effEnd = p.find(L'\\', at + 8);
    if (ownerStart == std::wstring::npos) return;
    std::string id = melange::game::Narrow(p.substr(ownerStart + 1, at - ownerStart - 1)) + "/" +
                     melange::game::Narrow(p.substr(at + 8, effEnd == std::wstring::npos ? std::wstring::npos : effEnd - at - 8));
    {
        std::lock_guard lk(g_mx);
        if (pfx::Effect* e = Find(id.c_str())) {
            e->reparse = true;
            LOG_INFO("[postfx] %s changed on disk; reloading", id.c_str());
        }
    }
    g_rescan = true;
}

bool Runnable(const pfx::Effect& e) {
    if (!e.enabled || e.missing) return false;
    if (e.code) return !e.failed;
    return !e.desc.passes.empty() && (e.dirty || !e.failed);
}

void OnStage(Stage s, void*);

void SyncStages() {
    for (int i = 0; i < 2; ++i) {
        bool want = false;
        if (!g_bypass && !g_unsupported) {
            std::lock_guard lk(g_mx);
            for (auto& e : g_effects) want |= e->stage == kStages[i] && Runnable(*e);
        }
        if (want && !g_handles[i]) {
            g_handles[i] = melange::render::AddStageCallback(kStages[i], &OnStage, nullptr, kStageOrder);
            if (g_handles[i]) {
                LOG_INFO("[postfx] %s stack active", pfx::StageName(kStages[i]));
            } else if (!g_noStagesLogged) {
                g_noStagesLogged = true;
                LOG_WARN("[postfx] scene stages unavailable (is [Mirage] enabled?); effects will not run");
            }
        } else if (!want && g_handles[i]) {
            melange::render::RemoveStageCallback(g_handles[i]);
            g_handles[i] = 0;
            g_passes[i] = 0;
            LOG_INFO("[postfx] %s stack idle", pfx::StageName(kStages[i]));
        }
    }
    if (!g_handles[0] && !g_handles[1]) g_reason.clear();
}

void OnFrame() {
    if (!g_installed) return;
    if (g_rescan.exchange(false)) Scan();
    SyncStages();
    FlushPersist(false);
    std::vector<std::shared_ptr<pfx::Effect>> dead;
    {
        std::lock_guard lk(g_mx);
        dead.swap(g_graveyard);
    }
    if (!dead.empty() && wglGetCurrentContext())
        for (auto& e : dead) pfx::Release(*e);
}

bool SceneIsRgba8(const melange::render::SceneTargets& t) {
    if (g_fmt.tex == t.colorTex && g_fmt.w == t.w && g_fmt.h == t.h) return g_fmt.rgba8;
    GLint internal = 0;
    glBindTexture(GL_TEXTURE_2D, t.colorTex);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &internal);
    g_fmt = {t.colorTex, t.w, t.h, internal == GL_RGBA8 || internal == GL_RGBA};
    return g_fmt.rgba8;
}

void SetReason(const char* why) {
    if (g_reason != why) {
        g_reason = why;
        LOG_WARN("[postfx] bypassed: %s", why);
    }
}

void CompileLogged(pfx::Effect& e) {
    if (pfx::Compile(e)) {
        if (!e.code) LOG_INFO("[postfx] %s compiled (%zu passes)", e.id.c_str(), e.passes.size());
        return;
    }
    LOG_ERROR("[postfx] %s failed: %s", e.id.c_str(), e.error.c_str());
    melange::jlog::Rec("shader", melange::jlog::Level::Error, "postfx compile failed")
        .Str("id", e.id)
        .Str("owner", e.owner)
        .Str("error", e.error);
}

void OnStage(Stage s, void*) {
    const int si = StageIndex(s);
    g_passes[si] = 0;
    if (g_bypass) return;
    std::vector<std::shared_ptr<pfx::Effect>> chain;
    {
        std::lock_guard lk(g_mx);
        for (auto& e : g_effects)
            if (e->stage == s && Runnable(*e)) chain.push_back(e);
    }
    if (chain.empty()) return;
    std::sort(chain.begin(), chain.end(), [](const auto& a, const auto& b) {
        return a->order != b->order ? a->order < b->order : a->id < b->id;
    });
    if (melange::mirage::engine::MsaaOn()) return SetReason("the scene target is multisampled");
    melange::render::SceneTargets t = melange::render::GetSceneTargets();
    if (!t.valid) return SetReason("no scene framebuffer at this stage");
    if (!pfx::OwnContext()) return;
    uint32_t token = melange::render::PushState();
    if (!token) return SetReason("GL state could not be saved");
    melange::gldebug::PushGroup("Mirage post-FX");
    std::string why;
    if (!pfx::GlReady(&why)) {
        g_unsupported = true;
        SetReason(why.c_str());
        std::lock_guard lk(g_mx);
        for (auto& e : g_effects) {
            e->failed = true;
            e->dirty = false;
            e->error = why;
        }
    } else if (!SceneIsRgba8(t)) {
        SetReason("the scene colour target is not RGBA8");
    } else {
        std::vector<pfx::Effect*> raw;
        std::vector<bool> failedBefore;
        for (auto& e : chain) {
            if (e->dirty || e->glGen != pfx::ContextGeneration()) CompileLogged(*e);
            raw.push_back(e.get());
            failedBefore.push_back(e->failed);
        }
        pfx::FrameInput in;
        in.sceneColor = t.colorTex;
        in.sceneDepth = t.depthTex;
        in.w = t.w;
        in.h = t.h;
        melange::render::Camera cam;
        if (melange::render::GetCamera(&cam)) {
            std::copy(cam.proj, cam.proj + 16, in.proj);
            std::copy(cam.view, cam.view + 16, in.view);
            in.nearFar[0] = cam.nearZ;
            in.nearFar[1] = cam.farZ;
        } else {
            for (int k = 0; k < 16; k += 5) in.proj[k] = in.view[k] = 1.f;
        }
        if (!pfx::Invert4(in.proj, in.invProj)) std::copy(in.proj, in.proj + 16, in.invProj);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        in.timeSec = static_cast<float>(static_cast<double>(now.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart));
        in.frame = melange::events::FrameCount();
        in.splitCompare = g_split;
        in.gpuTimers = g_gpuTimers;
        pfx::RunResult r = pfx::Run(raw, in);
        g_passes[si] = r.passes;
        if (r.effects) g_reason.clear();
        if (r.glErrors && g_errorLogs < 20) {
            ++g_errorLogs;
            LOG_WARN("[postfx] %s stack raised %d GL error(s)", pfx::StageName(s), r.glErrors);
        }
        for (size_t i = 0; i < raw.size(); ++i)
            if (raw[i]->failed && !failedBefore[i])
                LOG_ERROR("[postfx] %s %s; skipped until reloaded", raw[i]->id.c_str(), raw[i]->error.c_str());
    }
    melange::gldebug::PopGroup();
    melange::render::PopState(token);
}

void ToggleStack(void*) { pfx::SetBypassed(!g_bypass); }

// ---------------------------------------------------------------- verbs
std::vector<std::string> Words(std::string_view s) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p < s.size()) {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == ',')) ++p;
        size_t q = p;
        while (q < s.size() && s[q] != ' ' && s[q] != '\t' && s[q] != ',') ++q;
        if (q > p) out.emplace_back(s.substr(p, q - p));
        p = q;
    }
    return out;
}

bool VerbList(std::string_view, void*) {
    melange::postfx::EffectInfo info[128];
    size_t n = melange::postfx::ListEffects(info, 128);
    for (size_t i = 0; i < n; ++i) {
        const auto& e = info[i];
        LOG_INFO("[postfx] %-28s stage=%s order=%d enabled=%d failed=%d gpuMs=%.3f cpuMs=%.3f title=\"%s\"", e.id,
                 pfx::StageName(e.stage), e.order, e.enabled, e.failed, e.gpuMs, e.cpuMs, e.title);
        melange::jlog::Rec("mirage", melange::jlog::Level::Info, "postfx effect")
            .Str("id", e.id).Str("title", e.title).Str("stage", pfx::StageName(e.stage)).Int("order", e.order)
            .Bool("enabled", e.enabled).Bool("failed", e.failed).Float("gpuMs", e.gpuMs).Float("cpuMs", e.cpuMs);
    }
    melange::postfx::Stats st = melange::postfx::GetStats();
    LOG_INFO("[postfx] %u effects, %u passes last frame, gpuMs=%.3f cpuMs=%.3f bypassed=%d%s%s", st.effects,
             st.activePasses, st.gpuMs, st.cpuMs, st.bypassed, st.bypassReason ? " reason=" : "",
             st.bypassReason ? st.bypassReason : "");
    return true;
}

bool VerbEnable(std::string_view a, void*) {
    auto w = Words(a);
    return w.size() == 2 && (w[1] == "0" || w[1] == "1") && melange::postfx::SetEnabled(w[0].c_str(), w[1] == "1");
}

bool VerbOrder(std::string_view a, void*) {
    auto w = Words(a);
    return w.size() == 2 && melange::postfx::SetOrder(w[0].c_str(), atoi(w[1].c_str()));
}

bool VerbParam(std::string_view a, void*) {
    auto w = Words(a);
    if (w.size() < 3 || w.size() > 6) return false;
    float v[4];
    int n = 0;
    for (size_t i = 2; i < w.size(); ++i) n += pfx::ParseFloats(w[i], v + n, 1);
    if (n != static_cast<int>(w.size()) - 2 || !melange::postfx::SetParam(w[0].c_str(), w[1].c_str(), v, n)) return false;
    float back[4] = {};
    melange::postfx::GetParam(w[0].c_str(), w[1].c_str(), back, 4);
    LOG_INFO("[postfx] param %s.%s set to %s", w[0].c_str(), w[1].c_str(), pfx::FormatFloats(back, n).c_str());
    return true;
}

bool VerbReload(std::string_view a, void*) {
    auto w = Words(a);
    int n = melange::postfx::Reload(w.empty() ? nullptr : w[0].c_str());
    LOG_INFO("[postfx] reload %s: %d effect(s)", w.empty() ? "all" : w[0].c_str(), n);
    return n > 0 || w.empty();
}

bool VerbBypass(std::string_view a, void*) {
    auto w = Words(a);
    if (w.size() != 1 || (w[0] != "0" && w[0] != "1")) return false;
    pfx::SetBypassed(w[0] == "1");
    return true;
}

bool VerbCompare(std::string_view a, void*) {
    auto w = Words(a);
    if (w.size() != 1 || (w[0] != "0" && w[0] != "1")) return false;
    pfx::SetSplitCompare(w[0] == "1");
    return true;
}

class MiragePostFX final : public melange::Module {
public:
    const char* Name() const override { return "MiragePostFX"; }
    const char* Description() const override { return "post-FX stack at the PostWorld and Final stages"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 44; }

    bool Install() override {
        if (!melange::mirage::stages::CoreEnabled(Name())) return false;
        melange::config::EnsureKey(kSection, "ToggleKey", "Ctrl+Shift+F8");
        melange::config::EnsureKey(kSection, "Stack", "");
        std::string toggle = melange::config::GetString(kSection, "ToggleKey", "Ctrl+Shift+F8");
        g_gpuTimers = Bool("GpuTimers", true);
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
        {
            std::lock_guard lk(g_mx);
            g_persist = pfx::ParseStack(ReadStack());
            for (const pfx::StackEntry& s : g_persist)
                if (!Find(s.id.c_str())) {
                    auto e = std::make_shared<pfx::Effect>();
                    e->id = s.id;
                    e->desc.title = s.id;
                    e->missing = true;
                    e->error = "not found";
                    e->order = s.order;
                    e->enabled = s.enabled;
                    g_effects.push_back(e);
                }
        }
        Scan();
        melange::mirage::modfs::Watch(L"postfx", &OnFileChange, nullptr);

        melange::testcmd::Register("postfx.list", &VerbList);
        melange::testcmd::Register("postfx.enable", &VerbEnable);
        melange::testcmd::Register("postfx.order", &VerbOrder);
        melange::testcmd::Register("postfx.param", &VerbParam);
        melange::testcmd::Register("postfx.reload", &VerbReload);
        melange::testcmd::Register("postfx.bypass", &VerbBypass);
        melange::testcmd::Register("postfx.compare", &VerbCompare);
        melange::overlay::AddPanel("mirage.postfx", "Mirage/Post-FX", &pfx::DrawPanel, nullptr);
        melange::overlay::AddMenuItem("Mirage/Post-FX/Toggle stack", &ToggleStack, nullptr, toggle.c_str());
        uint8_t dik = 0, mods = 0;
        if (!toggle.empty() && melange::overlay::ParseHotkey(toggle.c_str(), &dik, &mods))
            melange::overlay::AddHotkey(dik, mods, &ToggleStack, nullptr);
        else if (!toggle.empty())
            LOG_WARN("[postfx] ToggleKey '%s' not understood", toggle.c_str());
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        melange::events::Subscribe(melange::events::Event::Shutdown, [] { FlushPersist(true); });
        g_installed = true;

        size_t enabled = 0;
        {
            std::lock_guard lk(g_mx);
            for (auto& e : g_effects) enabled += e->enabled && !e->missing;
        }
        LOG_INFO("[postfx] ready: %zu effects found, %zu enabled", g_effects.size(), enabled);
        return true;
    }
};
}  // namespace

MELANGE_MODULE(MiragePostFX);

namespace melange::mirage::postfx {
std::recursive_mutex& Mutex() { return g_mx; }
const std::vector<std::shared_ptr<Effect>>& Effects() { return g_effects; }
bool Bypassed() { return g_bypass; }
void SetBypassed(bool on) {
    if (g_bypass == on) return;
    g_bypass = on;
    LOG_INFO("[postfx] stack %s", on ? "bypassed" : "active");
}
bool SplitCompare() { return g_split; }
void SetSplitCompare(bool on) {
    if (g_split == on) return;
    g_split = on;
    LOG_INFO("[postfx] split compare %s", on ? "on (left half without effects)" : "off");
}
}  // namespace melange::mirage::postfx

namespace melange::postfx {
size_t ListEffects(EffectInfo* out, size_t max) {
    std::lock_guard lk(g_mx);
    std::vector<pfx::Effect*> v;
    for (auto& e : g_effects) v.push_back(e.get());
    std::sort(v.begin(), v.end(), [](const pfx::Effect* a, const pfx::Effect* b) {
        if (a->stage != b->stage) return a->stage < b->stage;
        return a->order != b->order ? a->order < b->order : a->id < b->id;
    });
    if (!out) return v.size();
    size_t n = std::min(max, v.size());
    for (size_t i = 0; i < n; ++i) {
        const pfx::Effect& e = *v[i];
        out[i] = {e.id.c_str(), e.desc.title.empty() ? e.id.c_str() : e.desc.title.c_str(), e.stage, e.order, e.enabled,
                  e.failed || e.missing, e.gpuMs, e.cpuMs};
    }
    return n;
}

bool SetEnabled(const char* id, bool on) {
    std::lock_guard lk(g_mx);
    pfx::Effect* e = Find(id);
    if (!e) return false;
    if (e->enabled != on) LOG_INFO("[postfx] %s %s", e->id.c_str(), on ? "enabled" : "disabled");
    e->enabled = on;
    if (on && e->failed && e->code) {
        e->failed = false;
        e->error.clear();
    } else if (on && e->failed) {
        e->reparse = true;
        g_rescan = true;
    }
    Persist(*e);
    return true;
}

bool SetOrder(const char* id, int order) {
    std::lock_guard lk(g_mx);
    pfx::Effect* e = Find(id);
    if (!e) return false;
    if (e->order != order) LOG_INFO("[postfx] %s order %d -> %d", e->id.c_str(), e->order, order);
    e->order = order;
    Persist(*e);
    return true;
}

bool SetParam(const char* id, const char* param, const float* v, int n) {
    if (!param || !v || n < 1) return false;
    std::lock_guard lk(g_mx);
    pfx::Effect* e = Find(id);
    if (!e) return false;
    for (size_t k = 0; k < e->desc.params.size() && k < e->values.size(); ++k) {
        const pfx::ParamDesc& p = e->desc.params[k];
        if (p.name != param) continue;
        for (int i = 0; i < n && i < p.n; ++i) e->values[k][i] = v[i];
        g_pendingParams[e->id + "." + p.name] = pfx::FormatFloats(e->values[k].data(), p.n);
        return true;
    }
    return false;
}

bool GetParam(const char* id, const char* param, float* v, int n) {
    if (!param || !v) return false;
    std::lock_guard lk(g_mx);
    pfx::Effect* e = Find(id);
    if (!e) return false;
    for (size_t k = 0; k < e->desc.params.size() && k < e->values.size(); ++k)
        if (e->desc.params[k].name == param) {
            for (int i = 0; i < n && i < 4; ++i) v[i] = i < e->desc.params[k].n ? e->values[k][i] : 0.f;
            return true;
        }
    return false;
}

int Reload(const char* id) {
    int n = 0;
    {
        std::lock_guard lk(g_mx);
        for (auto& e : g_effects)
            if (!e->code && (!id || e->id == id)) {
                e->reparse = true;
                ++n;
            }
    }
    g_rescan = true;
    return n;
}

int AddCodePass(const char* id, render::Stage stage, int order, PassFn fn, void* user) {
    if (!id || !*id || strchr(id, ',') || !fn || (stage != render::Stage::PostWorld && stage != render::Stage::Final))
        return 0;
    std::lock_guard lk(g_mx);
    pfx::Effect* e = Find(id);
    if (e && !(e->missing && !e->code && e->dir.empty())) return 0;
    if (!e) {
        g_effects.push_back(std::make_shared<pfx::Effect>());
        e = g_effects.back().get();
        e->id = id;
    }
    e->code = true;
    e->missing = false;
    e->failed = false;
    e->error.clear();
    e->desc = {};
    e->desc.title = id;
    e->fn = fn;
    e->user = user;
    e->stage = stage;
    e->order = order;
    e->enabled = true;
    e->dirty = true;
    ApplyPersisted(*e);
    e->handle = g_nextHandle++;
    LOG_INFO("[postfx] code pass %s added at %s (order %d, %s)", id, pfx::StageName(stage), e->order,
             e->enabled ? "enabled" : "disabled");
    return e->handle;
}

void RemoveCodePass(int handle) {
    if (!handle) return;
    bool found = false;
    {
        std::lock_guard lk(g_mx);
        for (auto it = g_effects.begin(); it != g_effects.end(); ++it)
            if ((*it)->code && (*it)->handle == handle) {
                LOG_INFO("[postfx] code pass %s removed", (*it)->id.c_str());
                (*it)->removed.store(true, std::memory_order_release);
                g_graveyard.push_back(*it);
                g_effects.erase(it);
                found = true;
                break;
            }
    }
    if (!found) return;
    // A chain already snapshotted this frame may still be about to call fn(..., user), or be calling it right now
    // on the main thread; wait so the caller can safely free `user` once this returns. Skip the wait if we ARE
    // that thread (a code pass removing itself, or another pass in the same chain, must not block on itself).
    unsigned long self = GetCurrentThreadId();
    while (pfx::g_activeCodeHandle.load(std::memory_order_acquire) == handle &&
           pfx::g_activeCodeThread.load(std::memory_order_acquire) != self)
        Sleep(0);
}

Stats GetStats() {
    std::lock_guard lk(g_mx);
    Stats s{};
    s.effects = static_cast<uint32_t>(g_effects.size());
    s.activePasses = g_passes[0] + g_passes[1];
    for (auto& e : g_effects)
        if (e->enabled && !e->failed && !e->missing) {
            if (e->gpuMs > 0) s.gpuMs += e->gpuMs;
            s.cpuMs += e->cpuMs;
        }
    s.bypassed = g_bypass || !g_reason.empty();
    s.bypassReason = g_bypass ? "bypass all" : (g_reason.empty() ? nullptr : g_reason.c_str());
    return s;
}
}  // namespace melange::postfx
