// Module "Mirage": the shared graphics scaffold (engine access, scene stages, frame timing, mod folders).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "render/mirage/compat.h"
#include "render/mirage/engine.h"
#include "render/mirage/hub.h"
#include "render/mirage/modfs.h"
#include "render/mirage/pe.h"
#include "render/mirage/stages.h"
#include "melange/jlog.h"
#include "melange/render.h"
#include "melange/testcmd.h"

namespace {
using melange::render::Stage;
namespace engine = melange::mirage::engine;
namespace stages = melange::mirage::stages;

// ---------------------------------------------------------------- timing
constexpr int kWindow = 240;
std::mutex g_timeMx;
double g_busy[kWindow] = {}, g_interval[kWindow] = {};
int g_count = 0, g_idx = 0;
uint64_t g_frames = 0;
LARGE_INTEGER g_freq{}, g_lastReturn{}, g_lastEntry{};
BOOL(WINAPI* g_swapBuffers)(HDC) = nullptr;

double Ms(LONGLONG ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(g_freq.QuadPart); }

// Wraps core's SwapBuffers hook: entry is the Frame event, return is the end of the frame.
BOOL WINAPI TimedSwapBuffers(HDC dc) {
    LARGE_INTEGER entry;
    QueryPerformanceCounter(&entry);
    {
        std::lock_guard lk(g_timeMx);
        if (g_lastReturn.QuadPart && g_lastEntry.QuadPart) {
            g_busy[g_idx] = Ms(entry.QuadPart - g_lastReturn.QuadPart);
            g_interval[g_idx] = Ms(entry.QuadPart - g_lastEntry.QuadPart);
            g_idx = (g_idx + 1) % kWindow;
            if (g_count < kWindow) ++g_count;
        }
        g_lastEntry = entry;
        ++g_frames;
    }
    BOOL r = g_swapBuffers(dc);
    QueryPerformanceCounter(&g_lastReturn);
    return r;
}

double Percentile(double* v, int n, double q) {
    if (n <= 0) return 0;
    int k = std::clamp(static_cast<int>(q * (n - 1) + 0.5), 0, n - 1);
    std::nth_element(v, v + k, v + n);
    return v[k];
}

// ---------------------------------------------------------------- audit
struct SlotCount {
    int total = 0, target = 0, thunk = 0, other = 0;
};

SlotCount CountSlots(HMODULE mod, const char* dll, HMODULE target) {
    SlotCount c;
    melange::mirage::pe::ForEachImport(mod, dll, [&](const char*, void** slot) {
        ++c.total;
        if (melange::mirage::pe::InModule(target, *slot)) ++c.target;
        else if (melange::mirage::hub::IsThunk(*slot)) ++c.thunk;
        else ++c.other;
    });
    return c;
}

bool VerbAudit(std::string_view, void*) {
    HMODULE exe = GetModuleHandleW(nullptr), ogl = GetModuleHandleW(L"opengl32.dll"), cggl = GetModuleHandleW(L"cgGL.dll"),
            cg = GetModuleHandleW(L"cg.dll");
    SlotCount exeGl = CountSlots(exe, "OPENGL32.dll", ogl), cgGl = CountSlots(cggl, "OPENGL32.dll", ogl),
              exeCg = CountSlots(exe, "cg.dll", cg), exeCgGl = CountSlots(exe, "cgGL.dll", cggl);

    std::string objects;
    int nObjects = 0;
    for (int id = 0; id < engine::BucketCount(); ++id)
        for (bool post : {false, true}) {
            uintptr_t obj = engine::BucketFunc(id, post);
            if (!obj) continue;
            uintptr_t fn = 0;
            melange::mem::SafeRead(obj + 0x14, &fn, 4);
            bool own = stages::IsOwnObject(obj);
            if (!own && melange::mirage::pe::InModule(exe, reinterpret_cast<void*>(fn))) continue;
            char b[64];
            snprintf(b, sizeof b, "%s%s[%d]=%s", nObjects ? " " : "", post ? "post" : "pre", id, own ? "mirage" : "foreign");
            objects += b;
            ++nObjects;
        }

    std::string table;
    for (int s = 0; s < static_cast<int>(Stage::Count); ++s) {
        auto st = static_cast<Stage>(s);
        melange::render::StageInfo i = melange::render::GetStageInfo(st);
        char b[128];
        snprintf(b, sizeof b, "%s%s=%s%d(installed=%d calls=%llu callbacks=%d)", s ? " " : "", stages::Name(st),
                 i.post ? "post" : "pre", i.bucket, i.installed, static_cast<unsigned long long>(i.calls), stages::Callbacks(st));
        table += b;
    }
    stages::Stats ss = stages::GetStats();
    LOG_INFO("[mirage] audit: hub=%d thunks=%d | exe OPENGL32 %d/%d opengl32 (%d hub, %d other) | cgGL OPENGL32 %d/%d "
             "opengl32 (%d hub, %d other) | exe cg.dll %d/%d original, cgGL.dll %d/%d original | bucket objects: %d%s%s",
             melange::mirage::hub::Installed(), melange::mirage::hub::Count(), exeGl.target, exeGl.total, exeGl.thunk,
             exeGl.other, cgGl.target, cgGl.total, cgGl.thunk, cgGl.other, exeCg.target, exeCg.total, exeCgGl.target,
             exeCgGl.total, nObjects, nObjects ? " " : "", objects.c_str());
    LOG_INFO("[mirage] audit stages: %s | reasserts=%llu faults=%llu removed=%llu", table.c_str(),
             static_cast<unsigned long long>(ss.reasserts), static_cast<unsigned long long>(ss.faults),
             static_cast<unsigned long long>(ss.removed));
    melange::jlog::Rec("mirage", melange::jlog::Level::Info, "audit")
        .Bool("hub", melange::mirage::hub::Installed())
        .Int("thunks", melange::mirage::hub::Count())
        .Int("exeGlTotal", exeGl.total).Int("exeGlOpengl32", exeGl.target).Int("exeGlHub", exeGl.thunk)
        .Int("cgGlTotal", cgGl.total).Int("cgGlOpengl32", cgGl.target).Int("cgGlHub", cgGl.thunk)
        .Int("exeCgTotal", exeCg.total).Int("exeCgOriginal", exeCg.target)
        .Int("exeCgGlTotal", exeCgGl.total).Int("exeCgGlOriginal", exeCgGl.target)
        .Int("bucketObjects", nObjects).Str("objects", objects).Str("stages", table)
        .Uint("reasserts", ss.reasserts);
    return true;
}

class Mirage final : public melange::Module {
public:
    const char* Name() const override { return "Mirage"; }
    const char* Description() const override { return "graphics layer scaffold: renderer access, scene stages, mod folders"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 40; }

    bool Install() override {
        if (!engine::Check()) return false;
        std::string modsDir = String("ModsDir", "Mods"), disabled = String("DisabledMods", "mirage-landscape"), ids = String("StageIds", "");
        std::wstring dir(modsDir.begin(), modsDir.end());
        if (dir.size() < 2 || (dir[1] != L':' && dir[0] != L'\\')) dir = melange::game::GameDir() + L"\\" + dir;
        melange::mirage::modfs::Configure(dir, disabled);
        if (!stages::Configure(ids)) LOG_WARN("[mirage] StageIds='%s' ignored; using the built-in table", ids.c_str());
        stages::Enable();
        // Independent of MirageTrace, so a Save-logs export still gets a real gpu/compat.* with it disabled.
        melange::mirage::compat::Install();

        QueryPerformanceFrequency(&g_freq);
        if (!melange::mem::HookIAT("GDI32.dll", "SwapBuffers", reinterpret_cast<void*>(&TimedSwapBuffers),
                                   reinterpret_cast<void**>(&g_swapBuffers)))
            LOG_WARN("[mirage] SwapBuffers not hooked: frame timing unavailable");
        melange::events::Subscribe(melange::events::Event::Frame, [] {
            stages::OnFrame();
            melange::mirage::modfs::OnFrame();
        });
        melange::testcmd::Register("mirage.audit", &VerbAudit);

        std::string table;
        for (int s = 0; s < static_cast<int>(Stage::Count); ++s) {
            stages::Slot sl = stages::Get(static_cast<Stage>(s));
            char b[48];
            snprintf(b, sizeof b, "%s%s=%s%d", s ? "," : "", stages::Name(static_cast<Stage>(s)), sl.post ? "post" : "pre", sl.bucket);
            table += b;
        }
        LOG_INFO("[mirage] ready: stages %s, mods folder %s", table.c_str(), melange::game::Narrow(dir).c_str());
        return true;
    }

private:
    std::string String(const char* key, const char* def) const {
        melange::config::EnsureKey(Name(), key, def);
        return melange::config::GetString(Name(), key, def);
    }
};
}  // namespace

MELANGE_MODULE(Mirage);

namespace melange::render {
Timing GetTiming() {
    Timing t{};
    double busy[kWindow], iv[kWindow];
    int n;
    {
        std::lock_guard lk(g_timeMx);
        n = g_count;
        std::copy(g_busy, g_busy + n, busy);
        std::copy(g_interval, g_interval + n, iv);
        t.frames = g_frames;
    }
    if (n == 0) return t;
    double sum = 0;
    for (int i = 0; i < n; ++i) sum += iv[i];
    t.busyMsP50 = Percentile(busy, n, 0.5);
    t.busyMsP95 = Percentile(busy, n, 0.95);
    t.frameMsP50 = Percentile(iv, n, 0.5);
    t.fps = sum > 0 ? 1000.0 * n / sum : 0;
    return t;
}
}  // namespace melange::render
