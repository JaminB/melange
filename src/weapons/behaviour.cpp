// The clone behaviour hooks: LaunchPayload (fire), the payload Update entries (tick), the payload HandleMessage
// entries (impact) and CreateExplosion (explosion and the queued extras).
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/mem.h"
#include "lua/sim/sim_weapons.h"
#include "melange/bus.h"
#include "melange/sim.h"
#include "mods/thumper_internal.h"
#include "weapons/behaviour.h"
#include "weapons/engine.h"
#include "weapons/manifest.h"
#include "weapons/registry.h"

namespace melange::weapons::behaviour {
namespace {
namespace eng = engine;
SafetyHookInline g_hLaunch, g_hUpdB, g_hUpdP, g_hMsgB, g_hMsgP, g_hExpl;
bool g_installed = false, g_enabled = false, g_log = false, g_exploding = false;
uintptr_t g_inUpdate = 0, g_inMsg = 0;

constexpr uint32_t kLaunchDesc = 0x20, kPayloadDesc = 0xc4;

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

bool CloneAt(uintptr_t descField, CloneInfo* out) {
    const uintptr_t desc = Rd<uintptr_t>(descField);
    const CloneInfo* c = desc ? registry::ByDesc(desc) : nullptr;
    if (!c) return false;
    *out = *c;
    return true;
}

EventArgs Args(Event ev, const CloneInfo& c, uintptr_t entity) {
    EventArgs a{};
    a.ev = ev;
    a.k = c.k;
    a.tick = sim::Tick();
    a.entity = entity;
    return a;
}

struct Samples {
    float us[256];
    uint32_t n;
};
Samples g_cost[4] = {};

double UsPerTick() {
    static const double v = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / static_cast<double>(f.QuadPart);
    }();
    return v;
}

void Emit(const EventArgs& a, const CloneInfo& c, const char* msgName) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    CountEvent(a.ev);
    if (g_log) {
        if (a.ev == Event::Impact)
            LOG_INFO("[weapons] %s %s tick %u %s", simweapons::EventName(a.ev), c.name, a.tick, msgName ? msgName : "");
        else if (a.hasPos)
            LOG_INFO("[weapons] %s %s tick %u at (%.2f,%.2f,%.2f)", simweapons::EventName(a.ev), c.name, a.tick, a.pos[0],
                     a.pos[1], a.pos[2]);
        else
            LOG_INFO("[weapons] %s %s tick %u", simweapons::EventName(a.ev), c.name, a.tick);
    }
    try {
        Raise(a);
        simweapons::Dispatch(a, c.name, msgName);
    } catch (...) {
        LOG_ERROR("[weapons] the %s event of %s threw at tick %u", simweapons::EventName(a.ev), c.name, a.tick);
    }
    QueryPerformanceCounter(&t1);
    Samples& s = g_cost[static_cast<int>(a.ev) & 3];
    s.us[s.n++ % 256] = static_cast<float>((t1.QuadPart - t0.QuadPart) * UsPerTick());
}

struct Nest {
    uintptr_t& slot;
    uintptr_t prev;
    Nest(uintptr_t& s, uintptr_t v) : slot(s), prev(s) { slot = v; }
    ~Nest() { slot = prev; }
    Nest(const Nest&) = delete;
    Nest& operator=(const Nest&) = delete;
};

uintptr_t __fastcall HkLaunch(uintptr_t e, void*) {
    CloneInfo c;
    if (CloneAt(e + kLaunchDesc, &c)) Emit(Args(Event::Fire, c, e), c, nullptr);
    return g_hLaunch.thiscall<uintptr_t>(e);
}

// A Parabolic entry may reach the base entry for the same payload or message: one event per outermost call.
uint32_t Update(SafetyHookInline& h, uintptr_t p, uint32_t t) {
    if (g_inUpdate != p) {
        CloneInfo c;
        if (CloneAt(p + kPayloadDesc, &c)) Emit(Args(Event::Tick, c, p), c, nullptr);
    }
    Nest n(g_inUpdate, p);
    return h.stdcall<uint32_t>(p, t);
}
uint32_t __stdcall HkUpdB(uintptr_t p, uint32_t t) { return Update(g_hUpdB, p, t); }
uint32_t __stdcall HkUpdP(uintptr_t p, uint32_t t) { return Update(g_hUpdP, p, t); }

uintptr_t Message(SafetyHookInline& h, uintptr_t p, uintptr_t m) {
    if (m && g_inMsg != m) {
        CloneInfo c;
        if (CloneAt(p + kPayloadDesc, &c)) {
            const uint16_t id = Rd<uint16_t>(m + 4);
            const char* name = bus::NameOf(id);
            if (name && std::strncmp(name, "Payload.", 8) == 0) {
                EventArgs a = Args(Event::Impact, c, p);
                a.msgId = id;
                Emit(a, c, name);
            }
        }
    }
    Nest n(g_inMsg, m);
    return h.stdcall<uintptr_t>(p, m);
}
uintptr_t __stdcall HkMsgB(uintptr_t p, uintptr_t m) { return Message(g_hMsgB, p, m); }
uintptr_t __stdcall HkMsgP(uintptr_t p, uintptr_t m) { return Message(g_hMsgP, p, m); }

uintptr_t __fastcall HkExplode(uintptr_t p, void*, float* pos, uint32_t a2) {
    CloneInfo c;
    float origin[3] = {};
    if (g_exploding || !pos || !mem::SafeRead(reinterpret_cast<uintptr_t>(pos), origin, sizeof origin) ||
        !CloneAt(p + kPayloadDesc, &c))
        return g_hExpl.thiscall<uintptr_t>(p, pos, a2);
    g_exploding = true;
    EventArgs a = Args(Event::Explosion, c, p);
    std::memcpy(a.pos, origin, sizeof origin);
    a.hasPos = true;
    OpenExplosion(origin);
    Emit(a, c, nullptr);
    float extra[kMaxExtraCap][3];
    const int n = CloseExplosion(extra, kMaxExtraCap);
    const uintptr_t r = g_hExpl.thiscall<uintptr_t>(p, pos, a2);
    for (int i = 0; i < n; ++i) {
        CountExtra();
        if (g_log)
            LOG_INFO("[weapons] extra explosion %d of %s at (%.2f,%.2f,%.2f) tick %u", i + 1, c.name, extra[i][0],
                     extra[i][1], extra[i][2], a.tick);
        g_hExpl.thiscall<uintptr_t>(p, extra[i], a2);
    }
    g_exploding = false;
    return r;
}

void SetAll(bool on) {
    for (SafetyHookInline* h : {&g_hLaunch, &g_hUpdB, &g_hUpdP, &g_hMsgB, &g_hMsgP, &g_hExpl}) eng::Enable(*h, on);
    g_enabled = on;
}

bool Declares(const std::string& mod) {
    for (auto& d : manifest::Frozen())
        if (d.mod == mod) return true;
    return false;
}

bool Allowed(const char* id) {
    if (!id) return false;
    if (Declares(id)) return true;
    thumper::Entry e;
    if (!thumper::FindEntry(id, &e)) return false;
    for (auto& d : e.manifest.dependencies)
        if (Declares(d.id)) return true;
    for (auto& d : e.manifest.optional)
        if (Declares(d.id)) return true;
    return false;
}

const simweapons::Api kApi = {&Declared, &ActiveClone, &QueueExplosion, &Allowed, &manifest::BaseName};
}  // namespace

void InstallLua() { simweapons::SetApi(&kApi); }

bool Install(int extraLimit, bool logEvents) {
    SetExtraLimit(extraLimit);
    g_log = logEvents;
    if (g_installed) return true;
    bool ok = eng::Inline(g_hLaunch, eng::kLaunch, &HkLaunch, "LaunchPayload");
    ok = eng::Inline(g_hUpdB, eng::kUpdBase, &HkUpdB, "payload update") && ok;
    ok = eng::Inline(g_hUpdP, eng::kUpdPara, &HkUpdP, "parabolic update") && ok;
    ok = eng::Inline(g_hMsgB, eng::kMsgBase, &HkMsgB, "payload message") && ok;
    ok = eng::Inline(g_hMsgP, eng::kMsgPara, &HkMsgP, "parabolic message") && ok;
    ok = eng::Inline(g_hExpl, eng::kExplode, &HkExplode, "CreateExplosion") && ok;
    g_installed = ok;
    if (!ok) LOG_ERROR("[weapons] behaviour hooks unavailable: clone events and extra explosions are off");
    return ok;
}

void OnMatchBegin() {
    ResetCounters();
    g_exploding = false;
    g_inUpdate = g_inMsg = 0;
    const bool on = g_installed && Live();
    SetAll(on);
    LOG_INFO("[weapons] behaviour hooks %s for this match (extras per explosion %d)", on ? "on" : "off", ExtraLimit());
}

void OnMatchEnd() {
    if (g_enabled) {
        const Counters n = GetCounters();
        LOG_INFO("[weapons] match end: fires=%u ticks=%u impacts=%u explosions=%u extras=%u", n.fires, n.ticks, n.impacts,
                 n.explosions, n.extras);
    }
    SetAll(false);
}

bool HooksEnabled() { return g_enabled; }
Cost EventCost(Event e) {
    const Samples& s = g_cost[static_cast<int>(e) & 3];
    const uint32_t n = std::min<uint32_t>(s.n, 256);
    Cost c{s.n, 0, 0, 0};
    if (!n) return c;
    float v[256];
    std::copy(s.us, s.us + n, v);
    std::sort(v, v + n);
    c.p50Us = v[n / 2];
    c.p95Us = v[std::min<uint32_t>(n - 1, n * 95 / 100)];
    c.maxUs = v[n - 1];
    return c;
}
}  // namespace melange::weapons::behaviour
