// Module "MirageDebug": optional KHR_debug context, messages routed to the logs.
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "core/config.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "melange/gldebug.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"
#include "render/mirage/gldebug_logic.h"
#include "render/mirage/hub.h"

namespace melange::gldebug {
namespace {
namespace hub = melange::mirage::hub;

constexpr GLenum kSevHigh = 0x9146, kSevMedium = 0x9147, kSevLow = 0x9148, kSevNotif = 0x826B;
constexpr GLenum kDebugOutput = 0x92E0, kDebugOutputSync = 0x8242, kContextFlags = 0x821E;
constexpr int kWglProfileMask = 0x9126, kWglProfileCompat = 0x2, kWglContextFlags = 0x2094, kWglDebugBit = 0x1;

using PFN_CreateAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFN_DebugCb = void(APIENTRY*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const char*, const void*);
using PFN_DebugMessageCallback = void(APIENTRY*)(PFN_DebugCb, const void*);
using PFN_DebugMessageControl = void(APIENTRY*)(GLenum, GLenum, GLenum, GLsizei, const GLuint*, GLboolean);

HGLRC(WINAPI* g_nextCreateContext)(HDC) = nullptr;
BOOL(WINAPI* g_nextMakeCurrent)(HDC, HGLRC) = nullptr;

bool g_debugContext = false;
bool g_synchronous = true;
GLenum g_minSeverity = kSevLow;
std::mutex g_setupMx;
std::vector<HGLRC> g_debugCtxs, g_armed;

template <class T>
T Ext(const char* name) {
    PROC p = wglGetProcAddress(name);
    auto v = reinterpret_cast<intptr_t>(p);
    return (p && v != 1 && v != 2 && v != 3 && v != -1) ? reinterpret_cast<T>(p) : nullptr;
}

const char* SeverityName(GLenum s) {
    switch (s) {
        case kSevHigh: return "high";
        case kSevMedium: return "medium";
        case kSevLow: return "low";
        default: return "notification";
    }
}

melange::jlog::Level SeverityLevel(GLenum s) {
    switch (s) {
        case kSevHigh: return melange::jlog::Level::Error;
        case kSevMedium: return melange::jlog::Level::Warn;
        case kSevLow: return melange::jlog::Level::Info;
        default: return melange::jlog::Level::Debug;
    }
}

GLenum ParseSeverity(const std::string& s) {
    if (s == "notification") return kSevNotif;
    if (s == "medium") return kSevMedium;
    if (s == "high") return kSevHigh;
    return kSevLow;
}

struct Key {
    GLuint id;
    uint32_t caller;
    bool operator==(const Key& o) const { return id == o.id && caller == o.caller; }
};
struct KeyHash {
    size_t operator()(const Key& k) const { return (static_cast<size_t>(k.id) * 0x9E3779B1u) ^ k.caller; }
};

std::mutex g_mx;
std::unordered_map<Key, logic::Entry, KeyHash> g_seen;
Stats g_stats;
int g_budget = 200;
ULONGLONG g_budgetSecond = 0;

// The first return address in the exe, cgGL.dll or an .asi module; driver frames are skipped.
bool IsCallerModule(const void* p) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(p), &m))
        return false;
    if (m == GetModuleHandleW(nullptr) || m == GetModuleHandleW(L"cgGL.dll")) return true;
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    return n > 4 && _wcsicmp(path + n - 4, L".asi") == 0;
}

uintptr_t FindCaller() {
    void* frames[16] = {};
    USHORT n = CaptureStackBackTrace(2, 16, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
        if (IsCallerModule(frames[i])) return reinterpret_cast<uintptr_t>(frames[i]);
    return 0;
}

void APIENTRY OnDebugMessage(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const char* msg, const void*) {
    uintptr_t caller = g_synchronous ? FindCaller() : 0;
    std::string module = caller ? melange::game::DescribeAddress(caller) : "?";

    std::lock_guard lk(g_mx);
    ++g_stats.messages;
    switch (severity) {
        case kSevHigh: ++g_stats.high; break;
        case kSevMedium: ++g_stats.medium; break;
        case kSevLow: ++g_stats.low; break;
        default: ++g_stats.notification; break;
    }

    ULONGLONG now = GetTickCount64();
    if (now / 1000 != g_budgetSecond) {
        g_budgetSecond = now / 1000;
        g_budget = 200;
    }
    logic::Entry& e = g_seen[{id, static_cast<uint32_t>(caller)}];
    logic::Decision d = logic::Decide(e, g_budget, now);
    if (!d.log) {
        ++g_stats.suppressed;
        return;
    }
    uint64_t since = d.since;
    std::string suffix = since ? (" (+" + std::to_string(since) + " since last)") : std::string();
    LOG_INFO("[gl] %s id=%u src=0x%x type=0x%x caller=%s%s: %s", SeverityName(severity), id, source, type, module.c_str(),
             suffix.c_str(), msg ? msg : "");
    melange::jlog::Rec("gl", SeverityLevel(severity), msg ? msg : "")
        .Int("id", static_cast<int64_t>(id))
        .Hex("source", source)
        .Hex("type", type)
        .Str("severity", SeverityName(severity))
        .Hex("caller", caller)
        .Str("module", module)
        .Uint("sincePeriodic", since);
}

HGLRC WINAPI HookCreateContext(HDC dc) {
    HGLRC legacy = g_nextCreateContext(dc);
    if (!legacy) return legacy;
    HGLRC prevCtx = wglGetCurrentContext();
    HDC prevDc = wglGetCurrentDC();
    PFN_CreateAttribs create = nullptr;
    if (wglMakeCurrent(dc, legacy)) create = Ext<PFN_CreateAttribs>("wglCreateContextAttribsARB");
    wglMakeCurrent(prevDc, prevCtx);
    if (!create) {
        LOG_WARN("[gldebug] wglCreateContextAttribsARB unavailable; keeping the non-debug context");
        return legacy;
    }
    const int attribs[] = {kWglProfileMask, kWglProfileCompat, kWglContextFlags, kWglDebugBit, 0};
    HGLRC dbg = create(dc, nullptr, attribs);
    if (!dbg) {
        LOG_WARN("[gldebug] wglCreateContextAttribsARB(DEBUG) failed (GetLastError=0x%lx); keeping the non-debug context",
                 GetLastError());
        return legacy;
    }
    wglDeleteContext(legacy);
    g_debugContext = true;
    {
        std::lock_guard lk(g_setupMx);
        g_debugCtxs.push_back(dbg);
    }
    return dbg;
}

BOOL WINAPI HookMakeCurrent(HDC dc, HGLRC ctx) {
    BOOL ok = g_nextMakeCurrent(dc, ctx);
    if (!ok || !ctx || !g_debugContext) return ok;
    std::lock_guard lk(g_setupMx);
    if (std::find(g_debugCtxs.begin(), g_debugCtxs.end(), ctx) == g_debugCtxs.end()) return ok;
    if (std::find(g_armed.begin(), g_armed.end(), ctx) != g_armed.end()) return ok;
    g_armed.push_back(ctx);
    GLint flags = 0;
    glGetIntegerv(kContextFlags, &flags);
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    LOG_INFO("[gldebug] debug context: %s, flags 0x%x", ver ? ver : "?", flags);

    auto cb = Ext<PFN_DebugMessageCallback>("glDebugMessageCallback");
    auto ctl = Ext<PFN_DebugMessageControl>("glDebugMessageControl");
    if (!cb) cb = Ext<PFN_DebugMessageCallback>("glDebugMessageCallbackARB");
    if (!ctl) ctl = Ext<PFN_DebugMessageControl>("glDebugMessageControlARB");
    if (!cb) {
        LOG_WARN("[gldebug] glDebugMessageCallback unavailable on this driver (ctx %p)", static_cast<void*>(ctx));
        return ok;
    }
    cb(&OnDebugMessage, nullptr);
    if (ctl) {
        ctl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr, GL_TRUE);
        static const GLenum kOrder[] = {kSevNotif, kSevLow, kSevMedium, kSevHigh};
        for (GLenum s : kOrder) {
            if (s == g_minSeverity) break;
            ctl(GL_DONT_CARE, GL_DONT_CARE, s, 0, nullptr, GL_FALSE);
        }
    }
    glEnable(kDebugOutput);
    if (g_synchronous) glEnable(kDebugOutputSync);
    else glDisable(kDebugOutputSync);
    while (glGetError() != GL_NO_ERROR) {
    }
    LOG_INFO("[gldebug] KHR_debug armed on ctx %p (synchronous=%d minSeverity=%s)", static_cast<void*>(ctx), g_synchronous,
             SeverityName(g_minSeverity));
    return ok;
}

bool VerbStats(std::string_view, void*) {
    Stats s = GetStats();
    LOG_INFO("[gldebug] stats: debugContext=%d messages=%llu suppressed=%llu high=%u medium=%u low=%u notification=%u",
             DebugContext(), static_cast<unsigned long long>(s.messages), static_cast<unsigned long long>(s.suppressed),
             s.high, s.medium, s.low, s.notification);
    return true;
}

void DrawPanel(void*) {
    Stats s = GetStats();
    ImGui::Text("debug context: %s", DebugContext() ? "on" : "off");
    ImGui::Text("messages=%llu suppressed=%llu  high=%u medium=%u low=%u notification=%u",
                static_cast<unsigned long long>(s.messages), static_cast<unsigned long long>(s.suppressed), s.high, s.medium,
                s.low, s.notification);
    ImGui::Separator();
    std::vector<std::pair<Key, logic::Entry>> top;
    {
        std::lock_guard lk(g_mx);
        top.assign(g_seen.begin(), g_seen.end());
    }
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second.total > b.second.total; });
    ImGui::Text("%-8s %-10s %s", "id", "caller", "count");
    for (size_t i = 0; i < top.size() && i < 30; ++i)
        ImGui::Text("%-8u %08x %llu", top[i].first.id, top[i].first.caller,
                    static_cast<unsigned long long>(top[i].second.total));
}

class MirageDebug final : public melange::Module {
public:
    const char* Name() const override { return "MirageDebug"; }
    const char* Description() const override { return "optional GL debug context, KHR_debug messages to the logs"; }
    bool DefaultEnabled() const override { return false; }
    int Order() const override { return 42; }

    bool Install() override {
        g_synchronous = Bool("Synchronous", true);
        g_minSeverity = ParseSeverity(String("MinSeverity", "low"));

        if (!hub::Require(Name())) return false;
        if (!hub::Interpose("wglCreateContext", reinterpret_cast<void*>(&HookCreateContext),
                            reinterpret_cast<void**>(&g_nextCreateContext)))
            return false;
        if (!hub::Interpose("wglMakeCurrent", reinterpret_cast<void*>(&HookMakeCurrent),
                            reinterpret_cast<void**>(&g_nextMakeCurrent))) {
            LOG_WARN("[gldebug] wglMakeCurrent not interposed; KHR_debug will not be armed automatically");
        }
        melange::testcmd::Register("gldebug.stats", &VerbStats);
        melange::overlay::AddPanel("mirage.gldebug", "Mirage/GL debug", &DrawPanel, nullptr);
        LOG_INFO("[gldebug] ready: synchronous=%d minSeverity=%s", g_synchronous, SeverityName(g_minSeverity));
        return true;
    }

private:
    std::string String(const char* key, const char* def) const {
        melange::config::EnsureKey(Name(), key, def);
        return melange::config::GetString(Name(), key, def);
    }
};
}  // namespace

MELANGE_MODULE(MirageDebug);

bool DebugContext() { return g_debugContext; }
Stats GetStats() {
    std::lock_guard lk(g_mx);
    return g_stats;
}
}  // namespace melange::gldebug
