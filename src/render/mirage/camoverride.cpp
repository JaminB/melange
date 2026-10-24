// Module "MirageCamera": render::SetCameraOverride, a render-only camera.
//
// The engine draws every pass of a frame inside XOpenGLRenderManager::Render (vtable slot 8, 0x6f6b58, stdcall
// (this, dtMs)). Its main pass culls and draws from the render camera **(rm+0x60), an XCamera (vtable 0x893814):
// eye +0x120, target +0x12c, up +0x138, view +0x14, inverse view +0x54, projection +0x94, tangent frustum
// (l, r, b, t, n, f) +0x104, flags +0x144 (view stale), +0x145 (inverse stale), +0x146 (derived frustum stale).
// The game's logical camera (CameraManagerService 0x95c370, which Wormsign validates) is a different object that
// only feeds this one, so writing the XCamera changes the picture and nothing the simulation reads.
//
// While an override is set, the hook on Render saves the XCamera's block +0x14..+0x148, writes the override into it
// (eye/target/up plus the matching matrices, so a rebuild through the engine's own getters gives the same values),
// runs the original Render, and writes the saved block back. Code outside Render (game logic, input, the logical
// camera) never sees the override; everything drawn inside Render (the world, its culling, worms, particles, labels,
// Mirage's stage callbacks, post-FX and GLSL uniforms through GetCamera) sees one consistent camera.
#include <windows.h>

#include <safetyhook.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/compat.h"
#include "melange/jlog.h"
#include "melange/render.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "render/mirage/camoverride.h"
#include "render/mirage/camoverride_logic.h"
#include "render/mirage/engine.h"

namespace melange::mirage::camoverride {
namespace {
namespace logic = melange::mirage::camoverride::logic;

constexpr uintptr_t kRender = 0x6f6b58, kXCameraVt = 0x893814;
constexpr uintptr_t kBlockBegin = 0x14, kBlockEnd = 0x148;
constexpr size_t kBlockBytes = kBlockEnd - kBlockBegin;
constexpr uintptr_t kView = 0x14, kInvView = 0x54, kProj = 0x94, kFrustum = 0x104, kOrtho = 0x11c, kEye = 0x120,
                    kTarget = 0x12c, kUp = 0x138, kViewStale = 0x144, kInvStale = 0x145, kDerivedStale = 0x146;

std::recursive_mutex g_mx;
logic::State g_st;
bool g_checked = false, g_ok = false, g_wantHook = false;
safetyhook::InlineHook g_hook;

// Main thread only (set and cleared inside the Render hook).
bool g_applied = false;
uintptr_t g_cam = 0;
uint8_t g_saved[kBlockBytes];
float g_appliedEye[3] = {};
bool g_verified = false;
int g_foreignWrites = 0;

struct Counters {
    uint64_t applied = 0, sets = 0, expired = 0, matchEnds = 0, cleared = 0, busy = 0, skipped = 0;
} g_n;

bool Check() {
    if (g_checked) return g_ok;
    g_checked = true;
    // push ebp; mov ebp,esp; sub esp,0x20; push ebx; push esi; mov esi,[ebp+8]
    g_ok = game::IsKnownBuild() && engine::Check() &&
           mem::Expect(kRender, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x20, 0x53, 0x56, 0x8B, 0x75, 0x08});
    if (!g_ok) {
        LOG_WARN("[mirage] [MirageCamera] the engine's Render was not recognised; camera overrides are unavailable");
        compat::Report(compat::Kind::Feature, "camera-override", compat::Status::Failed, "engine Render not recognised", "builtin");
    }
    return g_ok;
}

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

// The main render camera, if it is the XCamera we know.
uintptr_t MainCamera() {
    const uintptr_t cam = engine::Cam();
    return cam && Rd<uintptr_t>(cam) == kXCameraVt ? cam : 0;
}

bool CopyGuarded(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using GetMatFn = float*(__stdcall*)(uintptr_t);  // slots 4 and 5 take `this` on the stack (0x6e1f00, 0x6e1a52)
// The engine's own rebuild (slot 4 view, slot 5 inverse view) from the eye/target/up just written, for a one-time
// check that the override's matrices are the ones the engine would build. Null on a fault.
const float* EngineRebuild(uintptr_t cam, int slot) {
    __try {
        auto fn = reinterpret_cast<GetMatFn>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(cam) + 4 * slot));
        return fn(cam);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

float MaxDiff(const float* a, const float* b) {
    float d = 0;
    for (int i = 0; i < 16; ++i) d = (std::max)(d, std::fabs(a[i] - b[i]));
    return d;
}

void Note(const char* msg, const char* why) {
    jlog::Rec("render", jlog::Level::Info, msg).Str("why", why).Emit();
}

// Ends a stale override (expired, match over). Caller holds g_mx.
void Expire() {
    switch (logic::Check(g_st, events::FrameCount(), sim::InMatch(), sim::MatchSerial())) {
        case logic::EndReason::Expired:
            ++g_n.expired;
            Note("camera override ended", "not refreshed");
            break;
        case logic::EndReason::MatchEnded:
            ++g_n.matchEnds;
            Note("camera override ended", "match ended");
            break;
        default: break;
    }
}

struct Matrices {
    float view[16], inv[16], proj[16];
    float k;  // tangent frustum scale (1 = fov unchanged)
};

void Build(const logic::Spec& s, const float gameProj[16], Matrices* m) {
    logic::LookAt(s, m->view, m->inv);
    m->k = logic::WithFov(gameProj, s.fovDeg, m->proj);
}

void Apply() {
    logic::Spec spec;
    {
        std::lock_guard lk(g_mx);
        Expire();
        if (!g_st.active) return;
        spec = g_st.spec;
    }
    const uintptr_t cam = MainCamera();
    if (!cam || !CopyGuarded(g_saved, reinterpret_cast<const void*>(cam + kBlockBegin), kBlockBytes)) {
        ++g_n.skipped;
        return;
    }
    const float* savedProj = reinterpret_cast<const float*>(g_saved + (kProj - kBlockBegin));
    const float* savedFrustum = reinterpret_cast<const float*>(g_saved + (kFrustum - kBlockBegin));
    const bool ortho = g_saved[kOrtho - kBlockBegin] != 0;
    Matrices m;
    Build(spec, savedProj, &m);
    if (ortho) {
        memcpy(m.proj, savedProj, sizeof m.proj);
        m.k = 1.f;
    }
    uint8_t block[kBlockBytes];
    memcpy(block, g_saved, kBlockBytes);
    auto put = [&](uintptr_t off, const void* src, size_t n) { memcpy(block + (off - kBlockBegin), src, n); };
    put(kEye, spec.pos, 12);
    put(kTarget, spec.target, 12);
    put(kUp, spec.up, 12);
    put(kView, m.view, 64);
    put(kInvView, m.inv, 64);
    put(kProj, m.proj, 64);
    if (m.k != 1.f) {
        float fr[4];
        for (int i = 0; i < 4; ++i) fr[i] = savedFrustum[i] * m.k;
        put(kFrustum, fr, 16);
    }
    block[kViewStale - kBlockBegin] = 0;
    block[kInvStale - kBlockBegin] = 0;
    block[kDerivedStale - kBlockBegin] = 1;  // the derived frustum (+0xd4) is rebuilt from the projection
    if (!CopyGuarded(reinterpret_cast<void*>(cam + kBlockBegin), block, kBlockBytes)) {
        ++g_n.skipped;
        return;
    }
    if (!g_verified) {
        // Once per session: let the engine rebuild both matrices from eye/target/up and compare.
        g_verified = true;
        reinterpret_cast<uint8_t*>(cam)[kViewStale] = 1;
        reinterpret_cast<uint8_t*>(cam)[kInvStale] = 1;
        const float* ev = EngineRebuild(cam, 4);
        float dv = ev ? MaxDiff(ev, m.view) : -1.f;
        const float* ei = EngineRebuild(cam, 5);
        float di = ei ? MaxDiff(ei, m.inv) : -1.f;
        LOG_INFO("[mirage] [MirageCamera] first override: engine rebuild differs from ours by view %.6f, inverse %.6f", dv, di);
        // Keep ours either way, so GetCamera outside Render and the drawn frame agree exactly.
        CopyGuarded(reinterpret_cast<void*>(cam + kView), m.view, 64);
        CopyGuarded(reinterpret_cast<void*>(cam + kInvView), m.inv, 64);
        reinterpret_cast<uint8_t*>(cam)[kViewStale] = 0;
        reinterpret_cast<uint8_t*>(cam)[kInvStale] = 0;
    }
    memcpy(g_appliedEye, spec.pos, sizeof g_appliedEye);
    g_cam = cam;
    g_applied = true;
    ++g_n.applied;
}

void Restore() {
    if (!g_applied) return;
    g_applied = false;
    float eye[3] = {};
    if (!CopyGuarded(eye, reinterpret_cast<const void*>(g_cam + kEye), sizeof eye)) return;
    if (memcmp(eye, g_appliedEye, sizeof eye) != 0) {
        // Something set the render camera during Render: keep what it wrote rather than our stale copy.
        if (g_foreignWrites++ < 5) LOG_WARN("[mirage] [MirageCamera] the render camera changed during Render; not restored");
        return;
    }
    CopyGuarded(reinterpret_cast<void*>(g_cam + kBlockBegin), g_saved, kBlockBytes);
}

uint32_t __stdcall HkRender(void* self, uint32_t dtMs) {
    Apply();
    const uint32_t r = g_hook.stdcall<uint32_t>(self, dtMs);
    Restore();
    return r;
}

// Main thread, outside Render.
void InstallHook() {
    if (g_hook || !Check()) return;
    g_hook = safetyhook::create_inline(kRender, &HkRender);
    if (!g_hook) {
        g_ok = false;
        LOG_WARN("[mirage] [MirageCamera] Render hook installation failed; camera overrides are unavailable");
        compat::Report(compat::Kind::Feature, "camera-override", compat::Status::Failed, "hook installation failed", "builtin");
        return;
    }
    LOG_INFO("[mirage] [MirageCamera] Render hook installed (0x%06x)", static_cast<unsigned>(kRender));
}

void OnFrame() {
    bool want;
    {
        std::lock_guard lk(g_mx);
        Expire();
        want = g_wantHook && !g_hook;
    }
    if (want) InstallHook();
}

bool VerbStats(std::string_view, void*) {
    std::lock_guard lk(g_mx);
    LOG_INFO("[mirage] camera.override: active=%d hook=%d applied=%llu sets=%llu expired=%llu matchEnds=%llu cleared=%llu "
             "busy=%llu skipped=%llu foreign=%d",
             g_st.active ? 1 : 0, g_hook ? 1 : 0, static_cast<unsigned long long>(g_n.applied),
             static_cast<unsigned long long>(g_n.sets), static_cast<unsigned long long>(g_n.expired),
             static_cast<unsigned long long>(g_n.matchEnds), static_cast<unsigned long long>(g_n.cleared),
             static_cast<unsigned long long>(g_n.busy), static_cast<unsigned long long>(g_n.skipped), g_foreignWrites);
    return true;
}

bool g_installed = false;
}  // namespace

void Adjust(float view[16], float invView[16], float proj[16]) {
    if (g_applied) return;  // inside Render the engine camera already holds it
    std::lock_guard lk(g_mx);
    if (!g_st.active || !g_hook) return;
    Matrices m;
    Build(g_st.spec, proj, &m);
    memcpy(view, m.view, sizeof m.view);
    memcpy(invView, m.inv, sizeof m.inv);
    if (proj[11] != 0.f) memcpy(proj, m.proj, sizeof m.proj);
}

void GameView(float view[16], float invView[16], float proj[16]) {
    if (!g_applied) return;
    memcpy(view, g_saved + (kView - kBlockBegin), 64);
    memcpy(invView, g_saved + (kInvView - kBlockBegin), 64);
    memcpy(proj, g_saved + (kProj - kBlockBegin), 64);
}

class MirageCamera final : public melange::Module {
  public:
    const char* Name() const override { return "MirageCamera"; }
    const char* Description() const override { return "render-only camera override for mods (wum.render.cameraOverride)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 45; }
    bool Install() override {
        if (!Check()) return false;
        events::Subscribe(events::Event::Frame, [] { OnFrame(); });
        testcmd::Register("camera.override", &VerbStats);
        g_installed = true;
        return true;
    }
};

MELANGE_MODULE(MirageCamera);
}  // namespace melange::mirage::camoverride

namespace melange::render {
using namespace melange::mirage::camoverride;

OverrideResult SetCameraOverride(const void* owner, const CameraOverride& o, int frames) {
    if (!g_installed || !g_ok) return OverrideResult::Unavailable;
    if (!sim::InMatch()) return OverrideResult::Unavailable;
    logic::Spec s{};
    memcpy(s.pos, o.pos, sizeof s.pos);
    memcpy(s.target, o.target, sizeof s.target);
    memcpy(s.up, o.up, sizeof s.up);
    s.fovDeg = o.fovDeg;
    bool installNow = false;
    {
        std::lock_guard lk(g_mx);
        const bool was = g_st.active;
        switch (logic::Set(g_st, owner, s, frames, events::FrameCount(), sim::MatchSerial())) {
            case logic::SetResult::Busy: ++g_n.busy; return OverrideResult::Busy;
            case logic::SetResult::Invalid: return OverrideResult::Invalid;
            default: break;
        }
        ++g_n.sets;
        if (!was) jlog::Rec("render", jlog::Level::Info, "camera override started").Emit();
        g_wantHook = true;
        // Between frames (not inside the engine's passes) the hook can go in right away.
        installNow = !g_hook && render::CurrentPass() == Pass::None && GetCurrentThreadId() == events::MainThreadId();
    }
    if (installNow) InstallHook();
    return OverrideResult::Ok;
}

bool ClearCameraOverride(const void* owner) {
    std::lock_guard lk(g_mx);
    if (!logic::Clear(g_st, owner)) return false;
    ++g_n.cleared;
    Note("camera override ended", "cleared");
    return true;
}

bool CameraOverridden() {
    std::lock_guard lk(g_mx);
    return g_st.active;
}
}  // namespace melange::render
