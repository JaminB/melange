// Module "MirageSupersample": the engine's supersampling (/SSAA) from enabled mods' runtime requests.
// Off by default: with no request and [MirageSupersample] Samples=auto the engine keeps its own /SSAA setting.
#include <windows.h>
#include <GL/gl.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/compat.h"
#include "melange/graphics.h"
#include "melange/mods.h"
#include "melange/render.h"
#include "melange/testcmd.h"
#include "render/mirage/engine.h"
#include "render/mirage/stages.h"
#include "render/mirage/supersample_logic.h"

namespace melange::mirage::supersample {
namespace {
namespace logic = melange::mirage::supersample::logic;
// Build #1077: when pp+0x78 is set, BeginScene steps the /SSAA factors (options +0x6c/+0x70, FXAA flag +0x74) one
// notch and rebuilds the scene targets (the engine's DEBUG.ChangeSSAA). The rebuild uses multisampled
// renderbuffers while pp+0x79 is set (it then sets pp+0x7a), and targets the factors' multiple of the window while
// pp+0x7a is clear.
constexpr uintptr_t kBeginSceneDirty = 0x61f819, kCreateTargets = 0x61f190;
constexpr int kMaxAttempts = 3;

std::mutex g_mx;
int g_ini = -1;
logic::Effective g_eff;
std::map<std::string, int> g_runtime;
logic::EngineAa g_vanilla;
bool g_haveVanilla = false, g_checked = false, g_ok = false, g_dirty = false, g_touched = false, g_gaveUp = false;
int g_attempts = 0;
uint64_t g_nextCheck = 0;

bool Check() {
    if (g_checked) return g_ok;
    g_checked = true;
    g_ok = game::IsKnownBuild() && engine::Check() && mem::Expect(kBeginSceneDirty, {0x80, 0x7E, 0x78, 0x00, 0x0F, 0x84}) &&
           mem::Expect(kCreateTargets, {0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF1});
    if (!g_ok) {
        LOG_WARN("[mirage] [MirageSupersample] the engine's /SSAA switch was not recognised; supersampling stays the engine's");
        compat::Report(compat::Kind::Feature, "supersample", compat::Status::Failed, "engine /SSAA switch not recognised", "builtin");
    }
    return g_ok;
}

uint8_t Rd8(uintptr_t a) {
    uint8_t v = 0;
    mem::SafeRead(a, &v, 1);
    return v;
}

bool ReadAa(logic::EngineAa* s) {
    uintptr_t pp = engine::PostProcess();
    if (!pp || !engine::Supersample(&s->x, &s->y)) return false;
    s->fxaa = engine::FxaaOn();
    s->hardware = Rd8(pp + 0x79) != 0;
    return true;
}

// Queues the engine's own rebuild so that its one-notch step lands on `t`.
void Queue(const logic::EngineAa& t) {
    uintptr_t o = engine::AppOptions(), pp = engine::PostProcess();
    if (!o || !pp) return;
    const logic::EngineAa b = logic::Before(t);
    *reinterpret_cast<volatile int*>(o + 0x6c) = b.x;
    *reinterpret_cast<volatile int*>(o + 0x70) = b.y;
    *reinterpret_cast<volatile uint8_t*>(o + 0x74) = b.fxaa ? 1 : 0;
    *reinterpret_cast<volatile uint8_t*>(pp + 0x79) = t.hardware ? 1 : 0;
    // The rebuild sets pp+0x7a only while pp+0x79 is set, so a hardware-AA scene would stay multisampled.
    if (!t.hardware) *reinterpret_cast<volatile uint8_t*>(pp + 0x7a) = 0;
    *reinterpret_cast<volatile uint8_t*>(pp + 0x78) = 1;
}

bool Fits(const logic::EngineAa& t) {
    if (t.x * t.y <= 1 || !wglGetCurrentContext()) return true;
    GLint max = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
    int w = 0, h = 0;
    render::WindowSize(&w, &h);
    return max <= 0 || (w * t.x <= max && h * t.y <= max);
}

std::string Describe() {
    logic::EngineAa cur;
    int w = 0, h = 0;
    bool have = ReadAa(&cur);
    engine::SceneSize(&w, &h);
    char b[200];
    snprintf(b, sizeof b, "effective=%d (%s) engine=%dx%d%s%s scene=%dx%d vanilla=%dx%d%s", g_eff.samples,
             g_eff.samples == 0 ? "vanilla" : g_ini >= 0 ? "ini override" : "mod-requested", have ? cur.x : 0,
             have ? cur.y : 0, have && cur.fxaa ? " fxaa" : "", engine::MsaaOn() ? " multisampled" : "", w, h,
             g_vanilla.x, g_vanilla.y, g_vanilla.fxaa ? " fxaa" : "");
    return b;
}

void Recompute() {
    std::vector<int> reqs;
    mods::ModInfo info[256];
    int n = mods::List(info, 256);
    for (int i = 0; i < n; ++i) {
        if (info[i].state != mods::State::Enabled) continue;
        auto rt = g_runtime.find(info[i].id);
        if (rt != g_runtime.end()) reqs.push_back(rt->second);
    }
    logic::Effective before = g_eff;
    g_eff = logic::Merge(g_ini, reqs);
    if (before.samples != g_eff.samples) {
        g_dirty = true;
        g_attempts = 0;
        g_gaveUp = false;
    }
}

void OnModsChanged(void*) {
    std::lock_guard lk(g_mx);
    Recompute();
}

// Main thread, between frames. Re-checked once a second so a display reset that rebuilds the targets is undone.
void OnFrame() {
    std::lock_guard lk(g_mx);
    const uint64_t frame = events::FrameCount();
    if (!g_ok || frame < 30 || !engine::Rm()) return;
    uintptr_t pp = engine::PostProcess();
    logic::EngineAa cur;
    if (!pp || !ReadAa(&cur)) return;
    if (!g_haveVanilla) {
        g_vanilla = cur;
        g_haveVanilla = true;
    }
    if (Rd8(pp + 0x78)) return;
    if (!g_dirty && frame < g_nextCheck) return;
    g_nextCheck = frame + 60;
    g_dirty = false;
    if (g_eff.samples == 0 && !g_touched) return;
    const logic::EngineAa want = logic::Target(g_eff, g_vanilla);
    if (logic::Same(cur, want)) {
        if (g_attempts) {
            int w = 0, h = 0;
            engine::SceneSize(&w, &h);
            LOG_INFO("[mirage] supersampling %dx%d: scene %dx%d%s", cur.x, cur.y, w, h, engine::MsaaOn() ? " (multisampled)" : "");
        }
        g_attempts = 0;
        if (g_eff.samples == 0) g_touched = false;
        return;
    }
    if (g_attempts >= kMaxAttempts) {
        if (!g_gaveUp) LOG_WARN("[mirage] supersampling did not take after %d tries: %s", kMaxAttempts, Describe().c_str());
        g_gaveUp = true;
        return;
    }
    if (!Fits(want)) {
        LOG_WARN("[mirage] supersampling %dx%d skipped: the scene would exceed the GPU's largest texture", want.x, want.y);
        g_attempts = kMaxAttempts;
        g_gaveUp = true;
        return;
    }
    ++g_attempts;
    g_touched = true;
    Queue(want);
}

bool VerbInfo(std::string_view, void*) {
    std::lock_guard lk(g_mx);
    LOG_INFO("[mirage] supersample: %s", Describe().c_str());
    return true;
}

class MirageSupersample final : public melange::Module {
public:
    const char* Name() const override { return "MirageSupersample"; }
    const char* Description() const override { return "supersampling requested by mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 48; }

    bool Install() override {
        if (!melange::mirage::stages::CoreEnabled(Name())) return false;
        melange::config::EnsureKey(Name(), "Samples", "auto");
        std::string s = melange::config::GetString(Name(), "Samples", "auto");
        if (!logic::ParseIni(s, &g_ini)) {
            LOG_WARN("[mirage] [MirageSupersample] Samples=%s not understood (auto, vanilla, off, 2, 4, 8 or 16); using auto", s.c_str());
            g_ini = -1;
        }
        Check();
        {
            std::lock_guard lk(g_mx);
            Recompute();
        }
        melange::testcmd::Register("mirage.supersample", &VerbInfo);
        melange::mods::OnChange(&OnModsChanged, nullptr);
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        compat::Report(compat::Kind::Feature, "supersample", g_ok ? compat::Status::Loaded : compat::Status::Failed,
                       g_ini >= 0 ? s.c_str() : "on mod request", "builtin");
        return true;
    }
};
}  // namespace

MELANGE_MODULE(MirageSupersample);
}  // namespace melange::mirage::supersample

namespace melange::graphics {
namespace ss = melange::mirage::supersample;

bool SetSupersampleRequest(const char* mod, int samples) {
    if (!mod || !*mod || (samples != 0 && !ss::logic::ValidModSamples(samples))) return false;
    if (!ss::Check()) return false;
    std::lock_guard lk(ss::g_mx);
    if (samples == 0) ss::g_runtime.erase(mod);
    else ss::g_runtime[mod] = samples;
    ss::Recompute();
    return true;
}

SupersampleInfo GetSupersampleInfo() {
    SupersampleInfo i{1, 1, 0, 0, 0, false, false, ss::Check()};
    std::lock_guard lk(ss::g_mx);
    mirage::engine::Supersample(&i.x, &i.y);
    mirage::engine::SceneSize(&i.sceneW, &i.sceneH);
    i.effective = ss::g_eff.samples;
    i.multisampled = mirage::engine::MsaaOn();
    i.modRequest = ss::g_eff.anyModRequest;
    return i;
}
}  // namespace melange::graphics
