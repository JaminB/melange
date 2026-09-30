// wum.level: the table a level script gets (its level, its knots, and trigger and crate spawns at a knot).
// Trampolines keep only plain data alive: every raise happens after the helpers have returned.
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lua/sim/sim_internal.h"

namespace melange::simcore {
namespace {
char g_err[256];

int Raise(l5::State* L) {
    l5::A().pushstring(L, g_err);
    l5::A().error(L);
    return 0;
}

int NilReason(l5::State* L, const char* why) {
    l5::A().pushnil(L);
    l5::A().pushstring(L, why);
    return 2;
}

int ModOf(l5::State* L) { return static_cast<int>(l5::A().tonumber(L, Upvalue(1))); }

const char* KnotKind(int mod, const char* knot) noexcept {
    if (mod < 0 || mod >= static_cast<int>(g.mods.size())) return nullptr;
    for (const auto& [name, kind] : g.mods[mod].knots)
        if (name == knot) return kind.c_str();
    return nullptr;
}

// Reads opts[key] (a number in [lo, hi], an integer when `integral`; a boolean counts as 0/1). False: bad value.
bool OptNum(l5::State* L, const char* key, float def, float lo, float hi, bool integral, float* out) noexcept {
    const auto& a = l5::A();
    *out = def;
    if (a.type(L, 2) != l5::kTTable) return true;
    a.pushstring(L, key);
    a.rawget(L, 2);
    const int t = a.type(L, -1);
    bool ok = true;
    if (t == l5::kTBoolean) *out = a.toboolean(L, -1) ? 1.0f : 0.0f;
    else if (t == l5::kTNumber) *out = a.tonumber(L, -1);
    else if (t != l5::kTNil) ok = false;
    a.settop(L, -2);
    if (ok && (!(*out >= lo && *out <= hi) || (integral && std::floor(*out) != *out))) ok = false;
    if (!ok) snprintf(g_err, sizeof g_err, "%s must be %s %g-%g", key, integral ? "an integer" : "a number",
                      static_cast<double>(lo), static_cast<double>(hi));
    return ok;
}

// Copies opts[key] into buf when it is a string of 1-63 of [A-Za-z0-9_]. False: bad value.
bool OptName(l5::State* L, const char* key, char* buf, size_t len) noexcept {
    const auto& a = l5::A();
    buf[0] = 0;
    if (a.type(L, 2) != l5::kTTable) return true;
    a.pushstring(L, key);
    a.rawget(L, 2);
    bool ok = true;
    if (a.type(L, -1) == l5::kTString) {
        const char* s = a.tostring(L, -1);
        const size_t n = std::strlen(s);
        ok = n >= 1 && n < len;
        for (size_t i = 0; ok && i < n; ++i)
            ok = (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_';
        if (ok) memcpy(buf, s, n + 1);
    } else if (a.type(L, -1) != l5::kTNil) {
        ok = false;
    }
    a.settop(L, -2);
    if (!ok) snprintf(g_err, sizeof g_err, "%s must be a name of 1-%u letters, digits or _", key, static_cast<unsigned>(len - 1));
    return ok;
}

struct Field {
    const char* name;
    const char* str;  // nullptr: num
    float num;
};

// Resets, sets the fields in order and creates, as the generated chunk does. nullptr or the reason.
const char* Spawn(l5::State* L, const char* reset, const Field* f, int n, const char* create) noexcept {
    const auto& a = l5::A();
    sim::SendResult r = DoSend(SendArgs{SendKind::Plain, reset, 0, 0, nullptr});
    if (r != sim::SendResult::Ok) return SendResultText(r);
    for (int i = 0; i < n; ++i) {
        if (f[i].str) a.pushstring(L, f[i].str);
        else a.pushnumber(L, f[i].num);
        const char* why = SetDataAt(L, f[i].name, a.gettop(L));
        a.settop(L, -2);
        if (why) {
            snprintf(g_err, sizeof g_err, "%s: %s", f[i].name, why);
            return g_err;
        }
    }
    r = DoSend(SendArgs{SendKind::Plain, create, 0, 0, nullptr});
    return r == sim::SendResult::Ok ? nullptr : SendResultText(r);
}

// wum.level.trigger(knot [, {index, radius, teamCollect, teamDestroy, hitpoints, wormCollect}])
int __cdecl LTrigger(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTString || (a.gettop(L) >= 2 && a.type(L, 2) != l5::kTTable && a.type(L, 2) != l5::kTNil)) {
        snprintf(g_err, sizeof g_err, "wum.level.trigger: expected (knot [, options])");
        return Raise(L);
    }
    const char* knot = a.tostring(L, 1);
    if (!KnotKind(ModOf(L), knot)) return NilReason(L, "unknown knot");
    float index, radius, collect, destroy, hp, worm;
    if (!OptNum(L, "index", 0, 0, 255, true, &index) || !OptNum(L, "radius", 60, 1, 1000, false, &radius) ||
        !OptNum(L, "teamCollect", 0, 0, 8, true, &collect) || !OptNum(L, "teamDestroy", 4, 0, 8, true, &destroy) ||
        !OptNum(L, "hitpoints", 1, 0, 1000, true, &hp) || !OptNum(L, "wormCollect", 0, 0, 1, true, &worm))
        return Raise(L);
    const Field f[] = {{"Trigger.Spawn", knot, 0},         {"Trigger.Radius", nullptr, radius},
                       {"Trigger.Index", nullptr, index},  {"Trigger.TeamCollect", nullptr, collect},
                       {"Trigger.TeamDestroy", nullptr, destroy}, {"Trigger.HitPoints", nullptr, hp},
                       {"Trigger.WormCollect", nullptr, worm}};
    if (const char* why = Spawn(L, "GameLogic.ResetTriggerParams", f, 7, "GameLogic.CreateTrigger")) return NilReason(L, why);
    a.pushboolean(L, 1);
    return 1;
}

// wum.level.crate(knot [, {kind = "weapon"|"health"|"utility", contents, count, amount, hitpoints, parachute}])
int __cdecl LCrate(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTString || (a.gettop(L) >= 2 && a.type(L, 2) != l5::kTTable && a.type(L, 2) != l5::kTNil)) {
        snprintf(g_err, sizeof g_err, "wum.level.crate: expected (knot [, options])");
        return Raise(L);
    }
    const char* knot = a.tostring(L, 1);
    if (!KnotKind(ModOf(L), knot)) return NilReason(L, "unknown knot");
    char kind[16], contents[64];
    float count, amount, hp, chute;
    if (!OptName(L, "kind", kind, sizeof kind) || !OptName(L, "contents", contents, sizeof contents) ||
        !OptNum(L, "count", 1, 1, 99, true, &count) || !OptNum(L, "amount", 25, 1, 500, true, &amount) ||
        !OptNum(L, "hitpoints", 25, 1, 1000, true, &hp) || !OptNum(L, "parachute", 0, 0, 1, true, &chute))
        return Raise(L);
    if (!kind[0]) memcpy(kind, "weapon", 7);
    const bool health = std::strcmp(kind, "health") == 0;
    if (!health && std::strcmp(kind, "weapon") != 0 && std::strcmp(kind, "utility") != 0) {
        snprintf(g_err, sizeof g_err, "wum.level.crate: kind must be weapon, health or utility");
        return Raise(L);
    }
    if (!health && !contents[0]) {
        snprintf(g_err, sizeof g_err, "wum.level.crate: a %s crate needs contents", kind);
        return Raise(L);
    }
    const Field f[] = {{"Crate.Type", kind, 0},
                       {"Crate.Contents", health ? "health" : contents, 0},
                       {"Crate.NumContents", nullptr, health ? amount : count},
                       {"Crate.Spawn", knot, 0},
                       {"Crate.GroundSnap", nullptr, 1},
                       {"Crate.Parachute", nullptr, chute},
                       {"Crate.RandomSpawnPos", nullptr, 0},
                       {"Crate.Hitpoints", nullptr, hp},
                       {"Crate.Scale", nullptr, 1}};
    if (const char* why = Spawn(L, "GameLogic.ResetCrateParameters", f, 9, "GameLogic.CreateCrate")) return NilReason(L, why);
    a.pushboolean(L, 1);
    return 1;
}

void SetStr(l5::State* L, int t, const char* k, const char* v) {
    l5::A().pushstring(L, k);
    l5::A().pushstring(L, v);
    l5::A().rawset(L, t);
}

void SetFn(l5::State* L, int t, const char* name, l5::CFunction fn, int mod) {
    const auto& a = l5::A();
    a.pushstring(L, name);
    a.pushnumber(L, static_cast<float>(mod));
    a.pushcclosure(L, fn, 1);
    a.rawset(L, t);
}
}  // namespace

void PushLevel(int mod) {
    l5::State* L = g.L;
    const auto& a = l5::A();
    const Mod& m = g.mods[mod];
    a.newtable(L);
    const int t = a.gettop(L);
    SetStr(L, t, "key", m.levelKey.c_str());
    SetStr(L, t, "stem", m.stem.c_str());
    a.pushstring(L, "knots");
    a.newtable(L);
    for (const auto& [name, kind] : m.knots) SetStr(L, t + 2, name.c_str(), kind.c_str());
    a.rawset(L, t);
    SetFn(L, t, "trigger", &LTrigger, mod);
    SetFn(L, t, "crate", &LCrate, mod);
}
}  // namespace melange::simcore
