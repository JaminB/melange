#include "render/mirage/stages.h"

#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "render/gl_guard.h"
#include "render/mirage/engine.h"

namespace melange::mirage::stages {
namespace {
using render::Stage;
constexpr int kStages = static_cast<int>(Stage::Count);
constexpr int kMainPass = 3;
// Main-pass buckets run in id order: sky 2-4, landscape 8, objects and worms 16, sea 17, foliage 32/34, the active
// worm between the engine's pre[38]/post[43] pair, particles 46-48, worm labels 59, HUD 69-84, Composite post[164].
constexpr Slot kDefault[kStages] = {
    {44, false},   // World: after the pre[38]..post[43] pair, before particles
    {50, false},   // WorldLate: after particles, before labels and HUD
    {50, false},   // PostWorld: same slot, runs after WorldLate
    {163, true},   // Hud: after all HUD buckets
    {164, true},   // Final: wraps the engine's Composite
};
const char* kNames[kStages] = {"World", "WorldLate", "PostWorld", "Hud", "Final"};

Slot g_table[kStages] = {kDefault[0], kDefault[1], kDefault[2], kDefault[3], kDefault[4]};

struct SlotState {
    int bucket = -1;
    bool post = false;
    uintptr_t obj = 0;
    uintptr_t saved = 0;  // the object that held the slot before us (AddRef'd), called after our stages
    bool installed = false;
    int failures = 0;
};
SlotState g_slots[kStages];
int g_nslots = 0;
int g_slotOf[kStages] = {};

struct Cb {
    int handle;
    Stage stage;
    render::StageFn fn;
    void* user;
    int order;
    int faults;
};
std::mutex g_mx;
std::vector<Cb> g_cbs;  // sorted by (order, handle)
int g_nextHandle = 1;
std::atomic<uint64_t> g_calls[kStages];
Stats g_stats{};
std::atomic<bool> g_enabled{false};
int g_reassertLogs = 0;

// The handle/thread of the callback currently executing inside Invoke, so RemoveStageCallback (any thread) can
// wait for a call already in flight instead of returning while the callback's `user` is still being read.
std::atomic<int> g_activeHandle{0};
std::atomic<unsigned long> g_activeThread{0};

uintptr_t Rd(uintptr_t a) {
    uintptr_t v = 0;
    mem::SafeRead(a, &v, 4);
    return v;
}
void AddRef(uintptr_t o) {
    if (auto f = reinterpret_cast<unsigned long(__stdcall*)(uintptr_t)>(Rd(Rd(o) + 4))) f(o);
}
void Release(uintptr_t o) {
    if (auto f = reinterpret_cast<unsigned long(__stdcall*)(uintptr_t)>(Rd(Rd(o) + 8))) f(o);
}

const char* SlotText(const SlotState& s) {
    static char b[4][24];
    static int k = 0;
    char* out = b[k++ & 3];
    snprintf(out, 24, "%s[%d]", s.post ? "post" : "pre", s.bucket);
    return out;
}

void BuildSlots() {
    g_nslots = 0;
    for (int i = 0; i < kStages; ++i) {
        int found = -1;
        for (int k = 0; k < g_nslots; ++k)
            if (g_slots[k].bucket == g_table[i].bucket && g_slots[k].post == g_table[i].post) found = k;
        if (found < 0) {
            found = g_nslots++;
            g_slots[found] = SlotState{};
            g_slots[found].bucket = g_table[i].bucket;
            g_slots[found].post = g_table[i].post;
        }
        g_slotOf[i] = found;
    }
}

bool Invoke(render::StageFn fn, Stage s, void* user, unsigned long* code, int handle) {
    g_activeHandle.store(handle, std::memory_order_release);
    g_activeThread.store(GetCurrentThreadId(), std::memory_order_release);
    int depth = render::StateDepth();
    bool ok = true;
    __try {
        fn(s, user);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        ok = false;
    }
    if (!ok) render::ForceStateDepth(depth);  // unwind a PushState() the callback never matched with PopState()
    g_activeHandle.store(0, std::memory_order_release);
    return ok;
}

bool CallSceneFunc(uintptr_t obj, void* a1, unsigned long* code) {
    __try {
        auto fn = *reinterpret_cast<void(__cdecl**)(void*, void*)>(obj + 0x14);
        if (fn) fn(a1, reinterpret_cast<void*>(obj));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

void Fault(int handle, Stage s, unsigned long code) {
    std::lock_guard lk(g_mx);
    ++g_stats.faults;
    auto it = std::find_if(g_cbs.begin(), g_cbs.end(), [&](const Cb& c) { return c.handle == handle; });
    if (it == g_cbs.end()) return;
    ++it->faults;
    LOG_ERROR("[mirage] %s callback %d raised exception 0x%08lx (%d/3)", kNames[static_cast<int>(s)], handle, code, it->faults);
    if (it->faults >= 3) {
        LOG_ERROR("[mirage] %s callback %d removed after 3 faults", kNames[static_cast<int>(s)], handle);
        g_cbs.erase(it);
        ++g_stats.removed;
    }
}

void RunStage(Stage s) {
    Cb local[64];
    int n = 0;
    {
        std::lock_guard lk(g_mx);
        for (const Cb& c : g_cbs)
            if (c.stage == s && n < 64) local[n++] = c;
    }
    g_calls[static_cast<int>(s)].fetch_add(1, std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        unsigned long code = 0;
        if (!Invoke(local[i].fn, s, local[i].user, &code, local[i].handle)) Fault(local[i].handle, s, code);
    }
}

void __cdecl SlotFn(void* a1, void* a2) {
    auto self = reinterpret_cast<uintptr_t>(a2);
    int si = -1;
    for (int i = 0; i < g_nslots; ++i)
        if (g_slots[i].obj == self) si = i;
    if (si < 0) return;
    if (engine::Pass() == kMainPass) {
        ++g_stats.mainPasses;
        for (int st = 0; st < kStages; ++st)
            if (g_slotOf[st] == si) RunStage(static_cast<Stage>(st));
    } else {
        ++g_stats.otherPasses;
    }
    if (uintptr_t saved = g_slots[si].saved) {
        unsigned long code = 0;
        if (!CallSceneFunc(saved, a1, &code))
            LOG_ERROR("[mirage] chained scene func %08x at %s raised 0x%08lx", static_cast<unsigned>(saved),
                      SlotText(g_slots[si]), code);
    }
}

void Install(SlotState& s) {
    if (s.failures >= 3) return;
    if (!s.obj) s.obj = engine::CreateSceneFunc(&SlotFn);
    uintptr_t cur = s.obj ? engine::BucketFunc(s.bucket, s.post) : 0;
    if (cur && cur != s.obj) {
        AddRef(cur);
        s.saved = cur;
    }
    if (!s.obj || !engine::SetBucketFunc(s.bucket, s.post, s.obj)) {
        if (s.saved) Release(s.saved);
        s.saved = 0;
        if (++s.failures >= 3) LOG_ERROR("[mirage] could not install the stage slot %s; giving up", SlotText(s));
        return;
    }
    s.installed = true;
    LOG_INFO("[mirage] stage slot %s installed (object %08x, chains %08x)", SlotText(s), static_cast<unsigned>(s.obj),
             static_cast<unsigned>(s.saved));
}

void Reassert(SlotState& s) {
    uintptr_t cur = engine::BucketFunc(s.bucket, s.post);
    if (cur == s.obj) return;
    ++g_stats.reasserts;
    if (cur != s.saved) {
        if (s.saved) Release(s.saved);
        s.saved = cur;
        if (cur) AddRef(cur);
    }
    bool ok = engine::SetBucketFunc(s.bucket, s.post, s.obj);
    if (g_reassertLogs++ < 50 || g_stats.reasserts % 100 == 0)
        LOG_INFO("[mirage] re-assert %s: slot held %08x, now chains %08x (%s, reasserts=%llu)", SlotText(s),
                 static_cast<unsigned>(cur), static_cast<unsigned>(s.saved), ok ? "ok" : "failed",
                 static_cast<unsigned long long>(g_stats.reasserts));
}

void Restore(SlotState& s) {
    // The engine may have already rewritten this slot to something other than our object (e.g. a scene rebuild
    // between the last Reassert and this call); overwriting that with our stale `saved` chain would lose it.
    if (engine::BucketFunc(s.bucket, s.post) == s.obj) {
        engine::SetBucketFunc(s.bucket, s.post, s.saved);
        LOG_INFO("[mirage] stage slot %s restored to %08x", SlotText(s), static_cast<unsigned>(s.saved));
    } else {
        LOG_WARN("[mirage] stage slot %s not restored: the engine already holds it", SlotText(s));
    }
    Release(s.saved);
    s.saved = 0;
    s.installed = false;
}
}  // namespace

Slot Default(Stage s) { return kDefault[static_cast<int>(s)]; }
Slot Get(Stage s) { return g_table[static_cast<int>(s)]; }
const char* Name(Stage s) { return static_cast<int>(s) < kStages ? kNames[static_cast<int>(s)] : "?"; }

bool Configure(const std::string& overrides) {
    Slot t[kStages];
    std::copy(kDefault, kDefault + kStages, t);
    bool ok = true;
    size_t p = 0;
    while (p < overrides.size()) {
        size_t q = overrides.find(',', p);
        if (q == std::string::npos) q = overrides.size();
        std::string item = overrides.substr(p, q - p);
        p = q + 1;
        item.erase(std::remove_if(item.begin(), item.end(), [](char c) { return c == ' ' || c == '\t'; }), item.end());
        if (item.empty()) continue;
        size_t eq = item.find('=');
        int st = -1;
        for (int i = 0; i < kStages && eq != std::string::npos; ++i)
            if (_stricmp(item.substr(0, eq).c_str(), kNames[i]) == 0) st = i;
        std::string v = eq == std::string::npos ? "" : item.substr(eq + 1);
        bool post = _strnicmp(v.c_str(), "post", 4) == 0;
        bool pre = _strnicmp(v.c_str(), "pre", 3) == 0;
        int id = (pre || post) ? atoi(v.c_str() + (post ? 4 : 3)) : -1;
        if (st < 0 || id < 0 || id > 1023) {
            LOG_WARN("[mirage] StageIds: cannot parse '%s'", item.c_str());
            ok = false;
            continue;
        }
        t[st] = {id, post};
    }
    if (!ok) std::copy(kDefault, kDefault + kStages, t);
    std::copy(t, t + kStages, g_table);
    BuildSlots();
    return ok;
}

void Enable() { g_enabled = true; }

bool Enabled() { return g_enabled; }

bool CoreEnabled(const char* who) {
    if (config::GetBool("Mirage", "Enabled", true)) return true;
    LOG_INFO("[mirage] %s off: [Mirage] Enabled=0", who);
    return false;
}

void OnFrame() {
    if (!g_enabled || !engine::Sort() || engine::BucketCount() == 0) return;
    bool want[kStages] = {};
    {
        std::lock_guard lk(g_mx);
        for (const Cb& c : g_cbs) want[g_slotOf[static_cast<int>(c.stage)]] = true;
    }
    for (int i = 0; i < g_nslots; ++i) {
        SlotState& s = g_slots[i];
        if (want[i] && !s.installed)
            Install(s);
        else if (s.installed && !want[i] && s.saved)
            Restore(s);
        else if (s.installed)
            Reassert(s);
    }
}

Stats GetStats() { return g_stats; }

bool IsOwnObject(uintptr_t obj) {
    for (int i = 0; i < g_nslots; ++i)
        if (obj && g_slots[i].obj == obj) return true;
    return false;
}

int Callbacks(Stage s) {
    std::lock_guard lk(g_mx);
    return static_cast<int>(std::count_if(g_cbs.begin(), g_cbs.end(), [&](const Cb& c) { return c.stage == s; }));
}
}  // namespace melange::mirage::stages

namespace melange::render {
using mirage::stages::g_enabled;
namespace {
constexpr GLenum kFRAMEBUFFER = 0x8D40, kFRAMEBUFFER_BINDING = 0x8CA6, kCOLOR_ATTACHMENT0 = 0x8CE0,
                 kDEPTH_ATTACHMENT = 0x8D00, kATTACHMENT_OBJECT_TYPE = 0x8CD0, kATTACHMENT_OBJECT_NAME = 0x8CD1;

void Mul(const float* a, const float* b, float* out) {  // out = a * b, column-major (glLoadMatrixf) layout
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    memcpy(out, r, sizeof(r));
}

struct FboProcs {
    HGLRC ctx = nullptr;
    void(APIENTRY* attachParam)(GLenum, GLenum, GLenum, GLint*) = nullptr;
    void(APIENTRY* texLevelParamExt)(GLuint, GLenum, GLint, GLenum, GLint*) = nullptr;
};
FboProcs g_fbo;
const FboProcs& Fbo() {
    HGLRC ctx = wglGetCurrentContext();
    if (ctx == g_fbo.ctx) return g_fbo;
    FboProcs p;
    p.ctx = ctx;
    auto get = [](const char* n) -> PROC {
        PROC f = wglGetProcAddress(n);
        auto v = reinterpret_cast<intptr_t>(f);
        return (f && v != 1 && v != 2 && v != 3 && v != -1) ? f : nullptr;
    };
    if (ctx) {
        p.attachParam = reinterpret_cast<decltype(p.attachParam)>(get("glGetFramebufferAttachmentParameteriv"));
        if (!p.attachParam)
            p.attachParam = reinterpret_cast<decltype(p.attachParam)>(get("glGetFramebufferAttachmentParameterivEXT"));
        p.texLevelParamExt = reinterpret_cast<decltype(p.texLevelParamExt)>(get("glGetTextureLevelParameterivEXT"));
    }
    g_fbo = p;
    return g_fbo;
}

unsigned AttachedTexture(const FboProcs& p, GLenum attachment) {
    GLint type = 0, name = 0;
    p.attachParam(kFRAMEBUFFER, attachment, kATTACHMENT_OBJECT_TYPE, &type);
    if (type != GL_TEXTURE) return 0;
    p.attachParam(kFRAMEBUFFER, attachment, kATTACHMENT_OBJECT_NAME, &name);
    return static_cast<unsigned>(name);
}

SceneTargets g_targets{};
uint64_t g_targetsFrame = ~0ull;
}  // namespace

Pass CurrentPass() { return g_enabled ? static_cast<Pass>(mirage::engine::Pass()) : Pass::None; }

bool Ready() { return g_enabled && mirage::engine::Rm() && wglGetCurrentContext(); }

bool GetCamera(Camera* out) {
    if (!out) return false;
    *out = Camera{};
    uintptr_t cam = g_enabled ? mirage::engine::Cam() : 0;
    if (!cam || !mem::SafeRead(cam + 0x14, out->view, 64) || !mem::SafeRead(cam + 0x54, out->invView, 64) ||
        !mem::SafeRead(cam + 0x94, out->proj, 64))
        return false;
    Mul(out->proj, out->view, out->viewProj);
    for (int k = 0; k < 3; ++k) {
        out->pos[k] = out->invView[12 + k];
        out->fwd[k] = -out->invView[8 + k];
        out->up[k] = out->invView[4 + k];
    }
    // GL perspective: proj[10] = -(f+n)/(f-n), proj[14] = -2fn/(f-n)
    const float a = out->proj[10], b = out->proj[14];
    if (a != 1.f && a != -1.f) {
        out->nearZ = b / (a - 1.f);
        out->farZ = b / (a + 1.f);
    }
    out->valid = true;
    return true;
}

bool WorldToScreen(const float world[3], float* x, float* y, float* depth) {
    Camera c;
    if (!world || !GetCamera(&c)) return false;
    float clip[4];
    for (int r = 0; r < 4; ++r)
        clip[r] = c.viewProj[r] * world[0] + c.viewProj[4 + r] * world[1] + c.viewProj[8 + r] * world[2] + c.viewProj[12 + r];
    if (clip[3] <= 1e-6f) return false;
    int w = 0, h = 0;
    WindowSize(&w, &h);
    const float nx = clip[0] / clip[3], ny = clip[1] / clip[3], nz = clip[2] / clip[3];
    if (x) *x = (nx * 0.5f + 0.5f) * static_cast<float>(w);
    if (y) *y = (0.5f - ny * 0.5f) * static_cast<float>(h);
    if (depth) *depth = nz * 0.5f + 0.5f;
    return true;
}

void WindowSize(int* w, int* h) {
    RECT r{};
    if (HWND wnd = static_cast<HWND>(events::GameWindow())) GetClientRect(wnd, &r);
    if (w) *w = r.right - r.left;
    if (h) *h = r.bottom - r.top;
}

int AddStageCallback(Stage stage, StageFn fn, void* user, int order) {
    using namespace mirage::stages;
    if (!g_enabled || !fn || static_cast<int>(stage) >= kStages) return 0;
    std::lock_guard lk(g_mx);
    Cb c{g_nextHandle++, stage, fn, user, order, 0};
    auto at = std::upper_bound(g_cbs.begin(), g_cbs.end(), c,
                               [](const Cb& a, const Cb& b) { return a.order < b.order || (a.order == b.order && a.handle < b.handle); });
    g_cbs.insert(at, c);
    return c.handle;
}

void RemoveStageCallback(int handle) {
    using namespace mirage::stages;
    {
        std::lock_guard lk(g_mx);
        g_cbs.erase(std::remove_if(g_cbs.begin(), g_cbs.end(), [&](const Cb& c) { return c.handle == handle; }), g_cbs.end());
    }
    // RunStage copies callbacks out before invoking them, so the one being removed may already be running (or
    // about to run from that copy) on the main thread. Wait for it so the caller can safely free `user` right
    // after this returns; skip the wait if we ARE that thread (a callback removing a handle from within itself
    // must not block on itself).
    unsigned long self = GetCurrentThreadId();
    while (g_activeHandle.load(std::memory_order_acquire) == handle && g_activeThread.load(std::memory_order_acquire) != self)
        Sleep(0);
}

StageInfo GetStageInfo(Stage stage) {
    using namespace mirage::stages;
    StageInfo i{-1, false, false, 0};
    if (static_cast<int>(stage) >= kStages) return i;
    const SlotState& s = g_slots[g_slotOf[static_cast<int>(stage)]];
    i.bucket = s.bucket;
    i.post = s.post;
    i.installed = s.installed;
    i.calls = g_calls[static_cast<int>(stage)].load();
    return i;
}

SceneTargets GetSceneTargets() {
    SceneTargets t{};
    const FboProcs& p = Fbo();
    if (!g_enabled || !p.ctx || !p.attachParam) return t;
    GLint fbo = 0;
    glGetIntegerv(kFRAMEBUFFER_BINDING, &fbo);
    uint64_t frame = events::FrameCount();
    if (frame == g_targetsFrame && g_targets.fbo == static_cast<unsigned>(fbo)) return g_targets;
    t.fbo = static_cast<unsigned>(fbo);
    if (fbo) {
        t.colorTex = AttachedTexture(p, kCOLOR_ATTACHMENT0);
        t.depthTex = AttachedTexture(p, kDEPTH_ATTACHMENT);
        if (t.colorTex && p.texLevelParamExt) {
            p.texLevelParamExt(t.colorTex, GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &t.w);
            p.texLevelParamExt(t.colorTex, GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &t.h);
        } else if (t.colorTex) {
            GLint prev = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
            glBindTexture(GL_TEXTURE_2D, t.colorTex);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &t.w);
            glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &t.h);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
        }
        t.valid = t.colorTex != 0 && t.w > 0 && t.h > 0;
    }
    g_targets = t;
    g_targetsFrame = frame;
    return t;
}
}  // namespace melange::render
