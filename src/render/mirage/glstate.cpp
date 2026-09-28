#include <windows.h>
#include <GL/gl.h>

#include <new>

#include "render/gl_guard.h"
#include "melange/gldebug.h"
#include "melange/render.h"

namespace melange::render {
namespace {
constexpr int kMaxDepth = 4;
constexpr uint32_t kTokenTag = 0x4D470000;
alignas(gl::Guard) unsigned char g_slots[kMaxDepth][sizeof(gl::Guard)];
int g_depth = 0;

gl::Guard* Slot(int i) { return std::launder(reinterpret_cast<gl::Guard*>(g_slots[i])); }
}  // namespace

uint32_t PushState() {
    if (g_depth >= kMaxDepth || !wglGetCurrentContext()) return 0;
    gl::Guard* g = new (g_slots[g_depth]) gl::Guard(gl::Flavor::Stage);
    if (!g->Ok()) {
        g->~Guard();
        return 0;
    }
    return kTokenTag | static_cast<uint32_t>(++g_depth);
}

void PopState(uint32_t token) {
    if ((token & 0xFFFF0000u) != kTokenTag) return;
    int level = static_cast<int>(token & 0xFFFF);
    if (level < 1 || level > g_depth) return;
    while (g_depth >= level) Slot(--g_depth)->~Guard();
}

int StateDepth() { return g_depth; }

void ForceStateDepth(int depth) {
    if (depth < 0) depth = 0;
    while (g_depth > depth) Slot(--g_depth)->~Guard();
}
}  // namespace melange::render

namespace melange::gldebug {
namespace {
struct Procs {
    HGLRC ctx = nullptr;
    void(APIENTRY* push)(GLenum, GLuint, GLsizei, const char*) = nullptr;
    void(APIENTRY* pop)() = nullptr;
    void(APIENTRY* label)(GLenum, GLuint, GLsizei, const char*) = nullptr;
};
Procs g_procs;

const Procs& Get() {
    HGLRC ctx = wglGetCurrentContext();
    if (ctx == g_procs.ctx) return g_procs;
    Procs p;
    p.ctx = ctx;
    if (ctx) {
        auto proc = [](const char* a, const char* b) -> PROC {
            PROC f = wglGetProcAddress(a);
            auto v = reinterpret_cast<intptr_t>(f);
            if (!f || v == 1 || v == 2 || v == 3 || v == -1) f = wglGetProcAddress(b);
            v = reinterpret_cast<intptr_t>(f);
            return (!f || v == 1 || v == 2 || v == 3 || v == -1) ? nullptr : f;
        };
        p.push = reinterpret_cast<decltype(p.push)>(proc("glPushDebugGroup", "glPushDebugGroupKHR"));
        p.pop = reinterpret_cast<decltype(p.pop)>(proc("glPopDebugGroup", "glPopDebugGroupKHR"));
        p.label = reinterpret_cast<decltype(p.label)>(proc("glObjectLabel", "glObjectLabelKHR"));
        if (!p.push || !p.pop) {
            p.push = nullptr;
            p.pop = nullptr;
        }
    }
    g_procs = p;
    return g_procs;
}
}  // namespace

void PushGroup(const char* label) {
    const Procs& p = Get();
    if (p.push && label) p.push(0x824A /*GL_DEBUG_SOURCE_APPLICATION*/, 0, -1, label);
}

void PopGroup() {
    const Procs& p = Get();
    if (p.pop) p.pop();
}

void Label(unsigned identifier, unsigned name, const char* label) {
    const Procs& p = Get();
    if (p.label && label) p.label(identifier, name, -1, label);
}
}  // namespace melange::gldebug
