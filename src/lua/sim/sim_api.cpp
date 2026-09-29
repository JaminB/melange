// The wum.* functions of sim mods (Lua 5.0 trampolines) and the pre-checked sends.
// Trampolines keep only plain data alive: every raise goes through Raise() after the helpers have returned.
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"

namespace melange::simcore {
namespace {
char g_err[512];

int Raise(l5::State* L) {
    l5::A().pushstring(L, g_err);
    l5::A().error(L);
    return 0;
}

int Fail(l5::State* L, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    return Raise(L);
}

int NilReason(l5::State* L, const char* reason) {
    l5::A().pushnil(L);
    l5::A().pushstring(L, reason);
    return 2;
}

int ModOf(l5::State* L) { return static_cast<int>(l5::A().tonumber(L, Upvalue(1))); }

bool IsType(l5::State* L, int i, int t) { return l5::A().type(L, i) == t; }

bool Integral(float f) { return std::floor(f) == f; }

struct Extra {
    char name[32];
    l5::CFunction fn;
};
Extra g_extra[16];
int g_extraCount = 0;

// wum.log.*(...) and print: upvalue 1 = mod, upvalue 2 = level.
int __cdecl LLog(l5::State* L) {
    const auto& a = l5::A();
    static char buf[4096];
    size_t len = 0;
    buf[0] = 0;
    const int mod = ModOf(L);
    const int level = static_cast<int>(a.tonumber(L, Upvalue(2)));
    const int n = a.gettop(L);
    for (int i = 1; i <= n; ++i) {
        const int t = a.type(L, i);
        const char* s = t == l5::kTString || t == l5::kTNumber ? a.tostring(L, i)
                        : t == l5::kTBoolean                   ? (a.toboolean(L, i) ? "true" : "false")
                        : t == l5::kTNil                       ? "nil"
                        : t == l5::kTTable                     ? "table"
                        : t == l5::kTFunction                  ? "function"
                                                               : "userdata";
        const int w = snprintf(buf + len, sizeof buf - len, "%s%s", i > 1 ? " " : "", s);
        if (w < 0) break;
        len = std::min(sizeof buf - 1, len + static_cast<size_t>(w));
    }
    ModLog(mod, level, buf);
    return 0;
}

int __cdecl LEventsOn(l5::State* L) {
    const auto& a = l5::A();
    if (!IsType(L, 1, l5::kTString) || !IsType(L, 2, l5::kTFunction))
        return Fail(L, "wum.events.on: expected (name, function)");
    const char* name = a.tostring(L, 1);
    if (!KnownEvent(name)) return NilReason(L, "unknown event");
    a.pushvalue(L, 2);
    int ref = TakeRef();
    const uint32_t h = AddSub(ModOf(L), name, ref);
    if (!h) {
        DropRef(ref);
        return NilReason(L, "too many subscriptions");
    }
    a.pushnumber(L, static_cast<float>(h));
    return 1;
}

int __cdecl LEventsOff(l5::State* L) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1)) return Fail(L, "wum.events.off: expected a handle");
    a.pushboolean(L, RemoveSub(ModOf(L), static_cast<uint32_t>(a.tonumber(L, 1))));
    return 1;
}

int Schedule(l5::State* L, bool repeat) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1) || !IsType(L, 2, l5::kTFunction))
        return Fail(L, "wum.sim.%s: expected (ticks, function)", repeat ? "every" : "after");
    const float t = std::floor(a.tonumber(L, 1));
    if (!(t >= 1 && t <= 16777216.0f)) return Fail(L, "wum.sim.%s: ticks must be 1 or more", repeat ? "every" : "after");
    const uint32_t ticks = static_cast<uint32_t>(t);
    a.pushvalue(L, 2);
    int ref = TakeRef();
    const uint32_t h = AddTimer(ModOf(L), ticks, repeat ? ticks : 0, ref);
    if (!h) {
        DropRef(ref);
        return NilReason(L, "too many timers");
    }
    a.pushnumber(L, static_cast<float>(h));
    return 1;
}
int __cdecl LAfter(l5::State* L) { return Schedule(L, false); }
int __cdecl LEvery(l5::State* L) { return Schedule(L, true); }

int __cdecl LCancel(l5::State* L) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1)) return Fail(L, "wum.sim.cancel: expected a handle");
    a.pushboolean(L, CancelTimer(ModOf(L), static_cast<uint32_t>(a.tonumber(L, 1))));
    return 1;
}

int __cdecl LTick(l5::State* L) {
    l5::A().pushnumber(L, static_cast<float>(g.tick));
    return 1;
}

int __cdecl LRandom(l5::State* L) {
    const auto& a = l5::A();
    const int mod = ModOf(L);
    if (mod < 0 || mod >= static_cast<int>(g.mods.size())) return Fail(L, "wum.sim.random: no mod");
    uint32_t& st = g.mods[mod].rng;
    const int n = a.gettop(L);
    if (n == 0) {
        a.pushnumber(L, static_cast<float>(Draw24(st)) * (1.0f / 16777216.0f));
        return 1;
    }
    if (!a.isnumber(L, 1) || (n >= 2 && !a.isnumber(L, 2))) return Fail(L, "wum.sim.random: expected numbers");
    const double lo = n >= 2 ? a.tonumber(L, 1) : 1.0;
    const double hi = n >= 2 ? a.tonumber(L, 2) : a.tonumber(L, 1);
    double r = 0;
    const char* err = nullptr;
    if (!RandomRange(st, lo, hi, &r, &err)) return Fail(L, "wum.sim.random: %s", err);
    a.pushnumber(L, static_cast<float>(r));
    return 1;
}

int __cdecl LRandomFloat(l5::State* L) {
    const int mod = ModOf(L);
    if (mod < 0 || mod >= static_cast<int>(g.mods.size())) return Fail(L, "wum.sim.randomFloat: no mod");
    l5::A().pushnumber(L, static_cast<float>(Draw24(g.mods[mod].rng)) * (1.0f / 16777216.0f));
    return 1;
}

int SendReturn(l5::State* L, sim::SendResult r) {
    if (r == sim::SendResult::Ok) {
        l5::A().pushboolean(L, 1);
        return 1;
    }
    return NilReason(L, SendResultText(r));
}

// upvalue 2 = kind (-1: pick from the value's type)
int __cdecl LSend(l5::State* L) {
    const auto& a = l5::A();
    if (!IsType(L, 1, l5::kTString)) return Fail(L, "wum.sim.send: expected a message name");
    int kind = static_cast<int>(a.tonumber(L, Upvalue(2)));
    const int vt = a.type(L, 2);
    if (kind < 0) {
        kind = vt == l5::kTNone || vt == l5::kTNil ? 0
               : vt == l5::kTNumber               ? (Integral(a.tonumber(L, 2)) ? 1 : 2)
               : vt == l5::kTString               ? 3
                                                  : -1;
        if (kind < 0) return Fail(L, "wum.sim.send: the value must be a number or a string");
    }
    SendArgs s{static_cast<SendKind>(kind), a.tostring(L, 1), 0, 0, nullptr};
    if (s.kind == SendKind::Int || s.kind == SendKind::Float) {
        if (!a.isnumber(L, 2)) return Fail(L, "wum.sim.send: expected a number");
        s.f = a.tonumber(L, 2);
        if (s.kind == SendKind::Int) {
            if (!Integral(s.f) || std::fabs(s.f) > 16777216.0f) return Fail(L, "wum.sim.sendInt: not an integer below 2^24");
            s.i = static_cast<int32_t>(s.f);
        }
    } else if (s.kind == SendKind::String) {
        if (!a.isstring(L, 2)) return Fail(L, "wum.sim.sendString: expected a string");
        s.s = a.tostring(L, 2);
    }
    return SendReturn(L, DoSend(s));
}

// Engine data containers: type check, deny list, then the engine's own GetData/SetData through our refs.
const char* DataPrecheck(const char* name, int* type) {
    if (!g.L || l5::RunState() == 0) return "not in a match";
    if (l5::RunState() == 2) return "halted";
    *type = DataType(name);
    if (*type < 0) return "unknown data id";
    if (*type != 0 && *type != 1 && *type != 2 && *type != 4) return "not a number or string";
    if (!l5::AllowAll() && l5::EngineWouldDenyData(name)) return "denied";
    return nullptr;
}

bool EnsureEngineRef(int which);

int __cdecl LGetData(l5::State* L) {
    const auto& a = l5::A();
    if (!IsType(L, 1, l5::kTString)) return Fail(L, "wum.sim.getData: expected a data id");
    int type = 0;
    if (const char* why = DataPrecheck(a.tostring(L, 1), &type)) return NilReason(L, why);
    if (!EnsureEngineRef(kGetData)) return NilReason(L, "GetData is unavailable");
    const int top = a.gettop(L);
    SuspendBudget();
    a.rawgeti(L, l5::kRegistry, g.engineRef[kGetData]);
    a.pushvalue(L, 1);
    const int rc = a.pcall(L, 1, 1, 0);
    ResumeBudget();
    if (rc) {
        a.settop(L, top);
        return NilReason(L, "GetData failed");
    }
    return 1;
}

int __cdecl LSetData(l5::State* L) {
    const auto& a = l5::A();
    if (!IsType(L, 1, l5::kTString) || a.gettop(L) < 2) return Fail(L, "wum.sim.setData: expected (data id, value)");
    int type = 0;
    if (const char* why = DataPrecheck(a.tostring(L, 1), &type)) return NilReason(L, why);
    if (type == 4 ? !a.isstring(L, 2) : !a.isnumber(L, 2))
        return NilReason(L, type == 4 ? "the value must be a string" : "the value must be a number");
    if (!EnsureEngineRef(kSetData)) return NilReason(L, "SetData is unavailable");
    const int top = a.gettop(L);
    const int run0 = l5::RunState();
    SuspendBudget();
    a.rawgeti(L, l5::kRegistry, g.engineRef[kSetData]);
    a.pushvalue(L, 1);
    a.pushvalue(L, 2);
    const int rc = a.pcall(L, 2, 0, 0);
    ResumeBudget();
    a.settop(L, top);
    if (run0 == 1 && l5::RunState() == 2) {
        LOG_ERROR("[sim] setData halted the level script despite the pre-check (left halted)");
        return NilReason(L, "halted");
    }
    if (rc) return NilReason(L, "SetData failed");
    a.pushboolean(L, 1);
    return 1;
}

int __cdecl LWeaponsRaise(l5::State* L) { return Fail(L, "wum.sim.weapons is available in M5"); }

void SetFn(l5::State* L, int t, const char* name, l5::CFunction fn, int mod, int up2 = -2) {
    const auto& a = l5::A();
    a.pushstring(L, name);
    a.pushnumber(L, static_cast<float>(mod));
    if (up2 != -2) a.pushnumber(L, static_cast<float>(up2));
    a.pushcclosure(L, fn, up2 != -2 ? 2 : 1);
    a.rawset(L, t);
}

void SetStr(l5::State* L, int t, const char* k, const char* v) {
    l5::A().pushstring(L, k);
    l5::A().pushstring(L, v);
    l5::A().rawset(L, t);
}

const char* kEngineNames[] = {"SendMessage", "SendIntMessage", "SendFloatMessage", "SendStringMessage", "GetData",
                              "SetData"};

bool EnsureEngineRef(int which) {
    if (g.engineRef[which] >= 0) return true;
    const auto& a = l5::A();
    a.pushstring(g.L, kEngineNames[which]);
    a.rawget(g.L, l5::kGlobals);
    if (a.type(g.L, -1) != l5::kTFunction) {
        a.settop(g.L, -2);
        return false;
    }
    g.engineRef[which] = TakeRef();
    return g.engineRef[which] >= 0;
}


int PushSend(l5::State* L, const void* ctx) {
    const auto& s = *static_cast<const SendArgs*>(ctx);
    const auto& a = l5::A();
    a.pushstring(L, s.name);
    switch (s.kind) {
    case SendKind::Plain: return 1;
    case SendKind::Int: a.pushnumber(L, static_cast<float>(s.i)); return 2;
    case SendKind::Float: a.pushnumber(L, s.f); return 2;
    case SendKind::String: a.pushstring(L, s.s); return 2;
    }
    return 1;
}
}  // namespace

void CaptureEngineRefs() {
    for (int i = 0; i < 6; ++i) EnsureEngineRef(i);
}

void PushWum(int mod) {
    l5::State* L = g.L;
    const auto& a = l5::A();
    const Mod& m = g.mods[mod];
    a.newtable(L);
    const int wum = a.gettop(L);
    a.pushstring(L, "mod");
    a.newtable(L);
    SetStr(L, wum + 2, "id", m.id.c_str());
    SetStr(L, wum + 2, "version", m.version.c_str());
    a.rawset(L, wum);

    a.pushstring(L, "log");
    a.newtable(L);
    SetFn(L, wum + 2, "debug", &LLog, mod, 0);
    SetFn(L, wum + 2, "info", &LLog, mod, 1);
    SetFn(L, wum + 2, "warn", &LLog, mod, 2);
    SetFn(L, wum + 2, "error", &LLog, mod, 3);
    a.rawset(L, wum);

    a.pushstring(L, "events");
    a.newtable(L);
    SetFn(L, wum + 2, "on", &LEventsOn, mod);
    SetFn(L, wum + 2, "off", &LEventsOff, mod);
    a.rawset(L, wum);

    a.pushstring(L, "sim");
    a.newtable(L);
    const int sim = wum + 2;
    SetFn(L, sim, "after", &LAfter, mod);
    SetFn(L, sim, "every", &LEvery, mod);
    SetFn(L, sim, "cancel", &LCancel, mod);
    SetFn(L, sim, "tick", &LTick, mod);
    SetFn(L, sim, "random", &LRandom, mod);
    SetFn(L, sim, "randomFloat", &LRandomFloat, mod);
    SetFn(L, sim, "send", &LSend, mod, -1);
    SetFn(L, sim, "sendInt", &LSend, mod, 1);
    SetFn(L, sim, "sendFloat", &LSend, mod, 2);
    SetFn(L, sim, "sendString", &LSend, mod, 3);
    SetFn(L, sim, "getData", &LGetData, mod);
    SetFn(L, sim, "setData", &LSetData, mod);
    SetFn(L, sim, "hash", &LHash, mod);
    a.pushstring(L, "storage");
    a.newtable(L);
    a.rawset(L, sim);
    a.pushstring(L, "weapons");
    a.newtable(L);
    a.newtable(L);
    a.pushstring(L, "__index");
    a.pushcclosure(L, &LWeaponsRaise, 0);
    a.rawset(L, -3);
    a.pushstring(L, "__newindex");
    a.pushcclosure(L, &LWeaponsRaise, 0);
    a.rawset(L, -3);
    a.setmetatable(L, -2);
    a.rawset(L, sim);
    for (int i = 0; i < g_extraCount; ++i) SetFn(L, sim, g_extra[i].name, g_extra[i].fn, mod);
    a.rawset(L, wum);
}

const char* SendResultText(sim::SendResult r) {
    switch (r) {
    case sim::SendResult::Ok: return "ok";
    case sim::SendResult::NotRegistered: return "not registered";
    case sim::SendResult::Denied: return "denied";
    case sim::SendResult::NotInMatch: return "not in a match";
    case sim::SendResult::Halted: return "halted";
    case sim::SendResult::BadArgs: return "bad arguments";
    }
    return "?";
}

sim::SendResult DoSend(const SendArgs& s) {
    using R = sim::SendResult;
    l5::State* L = g.L;
    if (!L || !l5::Check()) return R::NotInMatch;
    if (!s.name || !*s.name || (s.kind == SendKind::String && !s.s)) return R::BadArgs;
    const int run0 = l5::RunState();
    if (run0 == 0) return R::NotInMatch;
    if (run0 == 2) return R::Halted;
    if (l5::Lookup(s.name) == 0xffff) return R::NotRegistered;
    const auto& a = l5::A();
    const int top = a.gettop(L);
    if (!a.checkstack(L, 8)) return R::BadArgs;
    char param[64] = {};
    const char* p = nullptr;
    if (s.kind == SendKind::Int || s.kind == SendKind::Float) {
        a.pushnumber(L, s.kind == SendKind::Int ? static_cast<float>(s.i) : s.f);
        snprintf(param, sizeof param, "%s", a.tostring(L, -1));
        a.settop(L, top);
        p = param;
    } else if (s.kind == SendKind::String) {
        p = s.s;
    }
    if (!l5::AllowAll() && l5::EngineWouldDenySend(s.name, p)) return R::Denied;
    const int which = static_cast<int>(s.kind);
    if (!EnsureEngineRef(which)) return R::NotInMatch;
    SuspendBudget();
    a.rawgeti(L, l5::kRegistry, g.engineRef[which]);
    const int nargs = PushSend(L, &s);
    const int rc = a.pcall(L, nargs, 0, 0);
    if (rc) snprintf(g_err, sizeof g_err, "%s", a.type(L, -1) == l5::kTString ? a.tostring(L, -1) : "?");
    a.settop(L, top);
    ResumeBudget();
    if (run0 == 1 && l5::RunState() == 2) {
        LOG_ERROR("[sim] sending '%s' halted the level script despite the pre-check (left halted)", s.name);
        return R::Halted;
    }
    if (rc) {
        LOG_WARN("[sim] sending '%s' raised: %s", s.name, g_err);
        return R::BadArgs;
    }
    if (IsModMessage(s.name)) DeliverEvent(s.name, &PushSend, &s);
    return R::Ok;
}
}  // namespace melange::simcore

namespace melange::sim {
using simcore::DoSend;
using simcore::SendArgs;
using simcore::SendKind;
SendResult Send(const char* name) { return DoSend(SendArgs{SendKind::Plain, name, 0, 0, nullptr}); }
SendResult SendInt(const char* name, int32_t v) {
    if (v > 16777216 || v < -16777216) return SendResult::BadArgs;
    return DoSend(SendArgs{SendKind::Int, name, 0, v, nullptr});
}
SendResult SendFloat(const char* name, float v) { return DoSend(SendArgs{SendKind::Float, name, v, 0, nullptr}); }
SendResult SendString(const char* name, const char* v) {
    return DoSend(SendArgs{SendKind::String, name, 0, 0, v});
}
}  // namespace melange::sim

namespace melange::simbridge {
bool AddSimFunction(const char* name, lua50::CFunction fn) {
    using namespace simcore;
    if (!name || !fn || std::strlen(name) >= sizeof g_extra[0].name || g_extraCount >= 16) return false;
    for (int i = 0; i < g_extraCount; ++i)
        if (std::strcmp(g_extra[i].name, name) == 0) return false;
    snprintf(g_extra[g_extraCount].name, sizeof g_extra[0].name, "%s", name);
    g_extra[g_extraCount++].fn = fn;
    return true;
}
}  // namespace melange::simbridge
