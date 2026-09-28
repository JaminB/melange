#pragma once
#include <cstdint>
#include "melange/render.h"
namespace melange::postfx {
struct EffectInfo {
    const char* id;        // "<owner>/<effect>", e.g. "mirage-samples/bloom"
    const char* title;
    render::Stage stage;   // PostWorld or Final
    int order;
    bool enabled, failed;
    double gpuMs, cpuMs;   // last frame (GPU from timer queries when available, else -1)
};
size_t ListEffects(EffectInfo* out, size_t max);
bool SetEnabled(const char* id, bool on);     // persisted to Melange.ini [MiragePostFX]
bool SetOrder(const char* id, int order);     // persisted
bool SetParam(const char* id, const char* param, const float* v, int n);  // persisted
bool GetParam(const char* id, const char* param, float* v, int n);
int Reload(const char* id = nullptr);         // recompile from disk; nullptr = all

// Pass from C++. Draw a full-screen pass reading ctx.src* into the bound target (dstFbo, viewport already set).
struct PassContext {
    unsigned srcColor, srcDepth, dstFbo;
    int w, h;
    const float* proj;
    const float* invProj;   // float[16], glLoadMatrixf layout
    float timeSec;
    uint64_t frame;
};
using PassFn = void (*)(const PassContext& ctx, void* user);
int AddCodePass(const char* id, render::Stage stage, int order, PassFn fn, void* user);  // any thread
void RemoveCodePass(int handle);

struct Stats { uint32_t effects, activePasses; double gpuMs, cpuMs; bool bypassed; const char* bypassReason; };
Stats GetStats();
}
