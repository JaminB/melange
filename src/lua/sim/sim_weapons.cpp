// wum.sim.weapons (Lua 5.0 trampolines) and the sim.weapon.* dispatch. Same rules as sim_api.cpp: trampolines keep
// only plain data alive across a raise.
#include "lua/sim/sim_weapons.h"

#include <cstdio>
#include <cstring>

#include "core/mem.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"

namespace melange::simweapons {
namespace {
namespace l5 = lua50;
using simcore::Upvalue;
using weapons::CloneInfo;
using weapons::kMaxClones;

const Api* g_api = nullptr;
char g_err[160];

constexpr const char* kFull[] = {"sim.weapon.fire", "sim.weapon.tick", "sim.weapon.impact", "sim.weapon.explosion"};
constexpr const char* kShort[] = {"fire", "tick", "impact", "explosion"};

int ModOf(l5::State* L) { return static_cast<int>(l5::A().tonumber(L, Upvalue(1))); }

int Raise(l5::State* L) {
    l5::A().pushstring(L, g_err);
    l5::A().error(L);
    return 0;
}

int Fail(l5::State* L, const char* text) {
    snprintf(g_err, sizeof g_err, "%s", text);
    return Raise(L);
}

int NilReason(l5::State* L, const char* reason) {
    l5::A().pushnil(L);
    l5::A().pushstring(L, reason);
    return 2;
}

int Declared(CloneInfo* out) {
    if (!g_api || !g_api->declared) return 0;
    const int n = g_api->declared(out, kMaxClones);
    return n < 0 ? 0 : (n > kMaxClones ? kMaxClones : n);
}

int FullIndex(const char* ev) {
    for (int i = 0; i < 4; ++i)
        if (std::strcmp(ev, kShort[i]) == 0) return i;
    return -1;
}

bool KnownClone(const char* name) {
    CloneInfo c[kMaxClones];
    const int n = Declared(c);
    for (int i = 0; i < n; ++i)
        if (std::strcmp(c[i].name, name) == 0) return true;
    return false;
}

// Subscriber of sim.weapon.<event>: upvalue 1 = the short event name, 2 = the name filter, 3 = the mod's function.
// Called with (full event, clone name, tick, ...); calls fn(event, clone name, tick, ...).
int __cdecl LTrampoline(l5::State* L) {
    const auto& a = l5::A();
    const char* filter = a.tostring(L, Upvalue(2));
    if (!filter || std::strcmp(filter, "*") != 0) {
        const char* name = a.type(L, 2) == l5::kTString ? a.tostring(L, 2) : nullptr;
        if (!name || !filter || std::strcmp(name, filter) != 0) return 0;
    }
    const int n = a.gettop(L);
    if (n < 1) return 0;
    a.pushvalue(L, Upvalue(1));
    a.replace(L, 1);
    a.pushvalue(L, Upvalue(3));
    a.insert(L, 1);
    if (a.pcall(L, n, 0, 0) != 0) a.error(L);
    return 0;
}

int __cdecl LOn(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTString || a.type(L, 2) != l5::kTString || a.type(L, 3) != l5::kTFunction)
        return Fail(L, "wum.sim.weapons.on: expected (event, name, function)");
    const int mod = ModOf(L);
    const int ev = FullIndex(a.tostring(L, 1));
    if (ev < 0) return NilReason(L, "unknown event");
    if (!simcore::WeaponsAllowed(mod)) return NilReason(L, "no weapons");
    const char* name = a.tostring(L, 2);
    if (std::strcmp(name, "*") != 0 && !KnownClone(name)) return NilReason(L, "unknown weapon");
    a.pushstring(L, kShort[ev]);
    a.pushvalue(L, 2);
    a.pushvalue(L, 3);
    a.pushcclosure(L, &LTrampoline, 3);
    int ref = simcore::TakeRef();
    const uint32_t h = simcore::AddSub(mod, kFull[ev], ref);
    if (!h) {
        simcore::DropRef(ref);
        return NilReason(L, "too many subscriptions");
    }
    a.pushnumber(L, static_cast<float>(h));
    return 1;
}

int __cdecl LOff(l5::State* L) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1)) return Fail(L, "wum.sim.weapons.off: expected a handle");
    a.pushboolean(L, simcore::RemoveSub(ModOf(L), static_cast<uint32_t>(a.tonumber(L, 1))));
    return 1;
}

int __cdecl LList(l5::State* L) {
    const auto& a = l5::A();
    CloneInfo c[kMaxClones];
    const int n = Declared(c);
    a.newtable(L);
    const int list = a.gettop(L);
    for (int i = 0; i < n; ++i) {
        a.newtable(L);
        const int t = a.gettop(L);
        a.pushstring(L, "name");
        a.pushstring(L, c[i].name);
        a.rawset(L, t);
        a.pushstring(L, "base");
        const char* base = g_api && g_api->baseName ? g_api->baseName(c[i].base) : nullptr;
        if (base)
            a.pushstring(L, base);
        else
            a.pushnumber(L, static_cast<float>(c[i].base));
        a.rawset(L, t);
        a.pushstring(L, "cell");
        a.pushnumber(L, static_cast<float>(c[i].cell));
        a.rawset(L, t);
        a.pushstring(L, "k");
        a.pushnumber(L, static_cast<float>(c[i].k));
        a.rawset(L, t);
        a.pushstring(L, "live");
        a.pushboolean(L, c[i].live);
        a.rawset(L, t);
        a.rawseti(L, list, i + 1);
    }
    return 1;
}

int __cdecl LActive(l5::State* L) {
    const auto& a = l5::A();
    const int k = g_api && g_api->active ? g_api->active() : -1;
    CloneInfo c[kMaxClones];
    const int n = Declared(c);
    for (int i = 0; k >= 0 && i < n; ++i)
        if (c[i].k == k) {
            a.pushstring(L, c[i].name);
            return 1;
        }
    a.pushnil(L);
    return 1;
}

int __cdecl LExplode(l5::State* L) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1) || !a.isnumber(L, 2) || !a.isnumber(L, 3))
        return Fail(L, "wum.sim.weapons.explode: expected (dx, dy, dz)");
    const float d[3] = {a.tonumber(L, 1), a.tonumber(L, 2), a.tonumber(L, 3)};
    const auto r = g_api && g_api->explode ? g_api->explode(d) : weapons::QueueResult::NotInExplosion;
    switch (r) {
        case weapons::QueueResult::Ok: a.pushboolean(L, 1); return 1;
        case weapons::QueueResult::Full: return NilReason(L, "full");
        case weapons::QueueResult::OutOfRange: return NilReason(L, "range");
        default: return NilReason(L, "not in explosion");
    }
}

void SetFn(l5::State* L, int t, const char* name, l5::CFunction fn, int mod) {
    const auto& a = l5::A();
    a.pushstring(L, name);
    a.pushnumber(L, static_cast<float>(mod));
    a.pushcclosure(L, fn, 1);
    a.rawset(L, t);
}
}  // namespace

void SetApi(const Api* api) { g_api = api; }

const char* EventName(weapons::Event e) {
    const int i = static_cast<int>(e);
    return i >= 0 && i < 4 ? kShort[i] : "?";
}

void Dispatch(const weapons::EventArgs& a, const char* cloneName, const char* msgName) {
    const int i = static_cast<int>(a.ev);
    if (i < 0 || i >= 4 || !cloneName) return;
    using simbridge::Arg;
    Arg args[5];
    int n = 0;
    args[n++] = {Arg::Str, 0, cloneName};
    args[n++] = {Arg::Num, static_cast<float>(a.tick), nullptr};
    if (a.ev == weapons::Event::Impact) {
        args[n++] = {Arg::Str, 0, msgName ? msgName : ""};
    } else if (a.ev == weapons::Event::Explosion || (a.ev == weapons::Event::Tick && a.hasPos)) {
        for (int j = 0; j < 3; ++j) args[n++] = {Arg::Num, a.pos[j], nullptr};
    }
    simbridge::DispatchArgs(kFull[i], args, n);
}
}  // namespace melange::simweapons

namespace melange::simcore {
bool WeaponsAllowed(int mod) noexcept {
    const char* id = simbridge::ModIdAt(mod);
    if (!id || !simweapons::g_api || !simweapons::g_api->allowed) return false;
    try {
        return simweapons::g_api->allowed(id);
    } catch (...) {
        mem::ReportOutOfMemory("sim: weapons allowed");
        return false;
    }
}

void PushWeapons(int mod) {
    l5::State* L = g.L;
    const auto& a = l5::A();
    a.newtable(L);
    const int t = a.gettop(L);
    simweapons::SetFn(L, t, "list", &simweapons::LList, mod);
    simweapons::SetFn(L, t, "on", &simweapons::LOn, mod);
    simweapons::SetFn(L, t, "off", &simweapons::LOff, mod);
    simweapons::SetFn(L, t, "explode", &simweapons::LExplode, mod);
    simweapons::SetFn(L, t, "active", &simweapons::LActive, mod);
}
}  // namespace melange::simcore
