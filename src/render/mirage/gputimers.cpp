#include "render/mirage/gputimers.h"

#include <windows.h>
#include <GL/gl.h>

#include <atomic>

#include "core/events.h"
#include "melange/gltrace.h"
#include "render/mirage/gputimers_logic.h"

namespace melange::mirage::gputimers {
namespace {
// Timestamp pairs rather than GL_TIME_ELAPSED: elapsed queries cannot nest, and the swap region spans every stage and
// the post-FX effect timers.
constexpr GLenum kTimestamp = 0x8E28, kQueryResult = 0x8866, kQueryResultAvailable = 0x8867;
constexpr int kRegions = static_cast<int>(melange::gltrace::GpuRegion::Count);

struct Procs {
    HGLRC ctx = nullptr;
    bool ok = false;
    void(APIENTRY* GenQueries)(GLsizei, GLuint*) = nullptr;
    void(APIENTRY* DeleteQueries)(GLsizei, const GLuint*) = nullptr;
    void(APIENTRY* QueryCounter)(GLuint, GLenum) = nullptr;
    void(APIENTRY* GetQueryObjectiv)(GLuint, GLenum, GLint*) = nullptr;
    void(APIENTRY* GetQueryObjectui64v)(GLuint, GLenum, unsigned long long*) = nullptr;
};
Procs g_p;

template <class T>
bool Proc(T& fn, const char* name) {
    PROC p = wglGetProcAddress(name);
    auto v = reinterpret_cast<intptr_t>(p);
    fn = (p && v != 1 && v != 2 && v != 3 && v != -1) ? reinterpret_cast<T>(p) : nullptr;
    return fn != nullptr;
}

const Procs& P() {
    HGLRC ctx = wglGetCurrentContext();
    if (ctx == g_p.ctx) return g_p;
    Procs p{};
    p.ctx = ctx;
    if (ctx) {
        p.ok = Proc(p.GenQueries, "glGenQueries") && Proc(p.DeleteQueries, "glDeleteQueries") &&
               Proc(p.QueryCounter, "glQueryCounter") &&
               Proc(p.GetQueryObjectiv, "glGetQueryObjectiv") && Proc(p.GetQueryObjectui64v, "glGetQueryObjectui64v");
    }
    g_p = p;
    return g_p;
}

struct RegionState {
    logic::Region state;
    GLuint queries[logic::kSlots][2] = {};
    bool allocated = false;
};
RegionState g_regions[kRegions];
std::atomic<bool> g_enabled{true};

void Poll(const Procs& p, RegionState& r) {
    if (!logic::ShouldPoll(r.state)) return;
    GLint avail = 0;
    p.GetQueryObjectiv(r.queries[r.state.head][1], kQueryResultAvailable, &avail);
    if (!avail) return;
    unsigned long long t0 = 0, t1 = 0;
    p.GetQueryObjectui64v(r.queries[r.state.head][0], kQueryResult, &t0);
    p.GetQueryObjectui64v(r.queries[r.state.head][1], kQueryResult, &t1);
    logic::Resolve(r.state, true, t1 > t0 ? t1 - t0 : 0);
}
}  // namespace

// Not in the anonymous namespace above: melange::gltrace::GetGpuTime (a sibling namespace block further down in
// this file) needs to reach it by its qualified name.
double LastMs(int region, bool* valid) {
    if (valid) *valid = false;
    if (region < 0 || region >= kRegions) return -1.0;
    const logic::Region& r = g_regions[region].state;
    const bool current = logic::Current(r, melange::events::FrameCount());
    if (valid) *valid = current;
    return current ? r.lastMs : -1.0;
}

bool Supported() { return P().ok; }
void SetEnabled(bool on) { g_enabled = on; }
bool Enabled() { return g_enabled; }

void Begin(int region) {
    if (!g_enabled || region < 0 || region >= kRegions) return;
    const Procs& p = P();
    if (!p.ok) return;
    RegionState& r = g_regions[region];
    if (!r.allocated) {
        p.GenQueries(logic::kSlots * 2, &r.queries[0][0]);
        r.allocated = true;
    }
    Poll(p, r);
    p.QueryCounter(r.queries[r.state.head][0], kTimestamp);
}

void End(int region) {
    if (!g_enabled || region < 0 || region >= kRegions) return;
    const Procs& p = P();
    if (!p.ok) return;
    RegionState& r = g_regions[region];
    if (!r.allocated) return;  // Begin() for this region never ran (no matching query is open)
    p.QueryCounter(r.queries[r.state.head][1], kTimestamp);
    logic::Advance(r.state, melange::events::FrameCount());
    // A timestamp records when the GPU reaches it, so a region also counts any time the GPU spends waiting for its
    // commands. Drivers hold recorded commands back and submit them in chunks (AMD's at a chunk boundary that can fall
    // anywhere in the frame), so without this the end of a stage could wait in the driver for everything the CPU does
    // until the next submission: Frame-event work, log writes, a Lua call, SwapBuffers. Measured on an RX 7800 XT, that
    // turned a 0.6 ms PostWorld into 5-10 ms readings (and 2263 ms across a turn change). Flushing right after the end
    // timestamp submits the stage's commands with it. The swap region needs none: SwapBuffers follows at once.
    if (region != 0) glFlush();
}
}  // namespace melange::mirage::gputimers

namespace melange::gltrace {
bool GpuTimerSupported() { return mirage::gputimers::Supported(); }

GpuTime GetGpuTime(GpuRegion r) {
    GpuTime t{-1.0, false};
    t.ms = mirage::gputimers::LastMs(static_cast<int>(r), &t.valid);
    return t;
}
}  // namespace melange::gltrace
