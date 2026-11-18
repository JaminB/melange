// wum.sim.melee (Lua 5.0 trampolines), the fire gate and the melee events. No engine addresses here: the Backend is
// the game (Melee module) or the self-test's fake. Same rules as sim_api.cpp: trampolines keep only plain data alive
// across a raise.
#include "lua/sim/sim_melee.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"

namespace melange::simmelee {
namespace {
namespace l5 = lua50;
using simcore::Upvalue;

Backend* g_backend = nullptr;
ClaimFn g_claimObserver = nullptr;
void* g_claimUser = nullptr;
char g_err[160];

struct Claim {
    bool used = false;
    char name[48] = "";
    int index = -1, mod = -1;
    bool dig = false;
    Mode mode = Mode::Auto;
};
Claim g_claims[kMaxClaims];

struct Held {
    bool on = false;
    int slot = -1, claim = -1;
    uint32_t pressTick = 0;
    Mode mode = Mode::Dig;  // Dig or Strike: what the gate did with the press
} g_held;

enum class Ev : uint8_t { Press, Release, Strike, Done };
constexpr const char* kEvFull[] = {"sim.melee.press", "sim.melee.release", "sim.melee.strike", "sim.melee.done"};
constexpr const char* kHitEvent = "sim.melee.hit";
struct Queued {
    Ev ev;
    int claim, slot;
    uint32_t tick, held;
    const char* mode;
};
constexpr int kQueueCap = 32;
Queued g_queue[kQueueCap];
int g_queued = 0;
uint32_t g_hits = 0, g_dropped = 0;

bool g_inHit = false;
HitResult g_result;

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

int ClaimByName(const char* name) {
    for (int i = 0; i < kMaxClaims; ++i)
        if (g_claims[i].used && std::strcmp(g_claims[i].name, name) == 0) return i;
    return -1;
}

int ClaimByIndex(int weaponIndex) {
    if (weaponIndex < 0) return -1;
    for (int i = 0; i < kMaxClaims; ++i)
        if (g_claims[i].used && g_claims[i].index == weaponIndex) return i;
    return -1;
}

void Queue(Ev ev, int claim, int slot, uint32_t held, const char* mode) {
    if (g_queued >= kQueueCap) {
        ++g_dropped;
        return;
    }
    g_queue[g_queued++] = {ev, claim, slot, sim::Tick(), held, mode};
}

// The claim the active worm wields, -1.
int WieldedClaim(int* slotOut) {
    if (!g_backend) return -1;
    const int slot = g_backend->ActiveSlot();
    if (slotOut) *slotOut = slot;
    return slot < 0 ? -1 : ClaimByIndex(g_backend->WeaponOf(slot));
}

void Release(const char* why) {
    if (!g_held.on) return;
    const uint32_t ticks = sim::Tick() - g_held.pressTick;
    Queue(Ev::Release, g_held.claim, g_held.slot, ticks, why);
    g_held = Held{};
}

// Subscriber of sim.melee.<event>: upvalue 1 = the short event name, 2 = the weapon filter, 3 = the mod's function.
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

constexpr const char* kShortEvents[] = {"press", "release", "strike", "done", "hit"};
constexpr const char* kFullEvents[] = {"sim.melee.press", "sim.melee.release", "sim.melee.strike", "sim.melee.done",
                                       "sim.melee.hit"};

bool ParseMode(const char* s, Mode* out) {
    if (!s) return false;
    if (!std::strcmp(s, "auto")) return *out = Mode::Auto, true;
    if (!std::strcmp(s, "dig")) return *out = Mode::Dig, true;
    if (!std::strcmp(s, "strike")) return *out = Mode::Strike, true;
    return false;
}

// wum.sim.melee.claim(weapon [, {dig = bool, mode = "auto"|"dig"|"strike"}]) -> true | nil, reason
int __cdecl LClaim(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTString) return Fail(L, "wum.sim.melee.claim(weapon [, opts]): weapon must be a string");
    const int ot = a.type(L, 2);
    bool dig = false;
    Mode mode = Mode::Auto;
    if (ot == l5::kTTable) {
        a.pushstring(L, "dig");
        a.rawget(L, 2);
        const int td = a.type(L, -1);
        if (td == l5::kTBoolean) dig = a.toboolean(L, -1) != 0;
        a.settop(L, 2);
        if (td != l5::kTNil && td != l5::kTBoolean) return Fail(L, "wum.sim.melee.claim: opts.dig must be a boolean");
        a.pushstring(L, "mode");
        a.rawget(L, 2);
        const int tm = a.type(L, -1);
        bool okMode = tm == l5::kTNil || (tm == l5::kTString && ParseMode(a.tostring(L, -1), &mode));
        a.settop(L, 2);
        if (!okMode) return Fail(L, "wum.sim.melee.claim: opts.mode must be \"auto\", \"dig\" or \"strike\"");
    } else if (ot != l5::kTNone && ot != l5::kTNil) {
        return Fail(L, "wum.sim.melee.claim: opts must be a table");
    }
    const int mod = ModOf(L);
    if (mod < 0 || mod >= static_cast<int>(simcore::g.mods.size()) || simcore::g.mods[mod].level)
        return NilReason(L, "denied");
    if (!simbridge::InTopLevelChunk()) return NilReason(L, "too late");
    const char* name = a.tostring(L, 1);
    if (!g_backend) return NilReason(L, "unavailable");
    const int index = g_backend->WeaponIndex(name);
    if (index < 0) return NilReason(L, "not melee");
    const int have = ClaimByName(name);
    if (have >= 0) {
        if (g_claims[have].mod != mod) {
            snprintf(g_err, sizeof g_err, "claimed by %s", simbridge::ModIdAt(g_claims[have].mod));
            return NilReason(L, g_err);
        }
        g_claims[have].dig = dig;
        g_claims[have].mode = mode;
        a.pushboolean(L, 1);
        return 1;
    }
    int slot = -1;
    for (int i = 0; i < kMaxClaims && slot < 0; ++i)
        if (!g_claims[i].used) slot = i;
    if (slot < 0) return NilReason(L, "too many claims");
    Claim& c = g_claims[slot];
    c.used = true;
    snprintf(c.name, sizeof c.name, "%s", name);
    c.index = index;
    c.mod = mod;
    c.dig = dig;
    c.mode = mode;
    int n = 0;
    for (const Claim& k : g_claims) n += k.used;
    if (g_claimObserver) g_claimObserver(n, g_claimUser);
    a.pushboolean(L, 1);
    return 1;
}

int __cdecl LOn(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTString || a.type(L, 2) != l5::kTString || a.type(L, 3) != l5::kTFunction)
        return Fail(L, "wum.sim.melee.on: expected (event, weapon, function)");
    const char* ev = a.tostring(L, 1);
    int e = -1;
    for (int i = 0; i < 5; ++i)
        if (!std::strcmp(ev, kShortEvents[i])) e = i;
    if (e < 0) return NilReason(L, "unknown event");
    const char* name = a.tostring(L, 2);
    if (std::strcmp(name, "*") != 0 && ClaimByName(name) < 0) return NilReason(L, "not claimed");
    a.pushstring(L, kShortEvents[e]);
    a.pushvalue(L, 2);
    a.pushvalue(L, 3);
    a.pushcclosure(L, &LTrampoline, 3);
    int ref = simcore::TakeRef();
    const uint32_t h = simcore::AddSub(ModOf(L), kFullEvents[e], ref);
    if (!h) {
        simcore::DropRef(ref);
        return NilReason(L, "too many subscriptions");
    }
    a.pushnumber(L, static_cast<float>(h));
    return 1;
}

int __cdecl LOff(l5::State* L) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1)) return Fail(L, "wum.sim.melee.off: expected a handle");
    a.pushboolean(L, simcore::RemoveSub(ModOf(L), static_cast<uint32_t>(a.tonumber(L, 1))));
    return 1;
}

int CheckName(l5::State* L, const char* fn) {
    if (l5::A().type(L, 1) != l5::kTString) {
        snprintf(g_err, sizeof g_err, "wum.sim.melee.%s(weapon): weapon must be a string", fn);
        return -2;
    }
    return ClaimByName(l5::A().tostring(L, 1));
}

// held(weapon) -> slot, heldTicks, pressTick, mode | nil
int __cdecl LHeld(l5::State* L) {
    const int c = CheckName(L, "held");
    if (c == -2) return Raise(L);
    const auto& a = l5::A();
    if (c < 0 || !g_held.on || g_held.claim != c) {
        a.pushnil(L);
        return 1;
    }
    a.pushnumber(L, static_cast<float>(g_held.slot));
    a.pushnumber(L, static_cast<float>(sim::Tick() - g_held.pressTick));
    a.pushnumber(L, static_cast<float>(g_held.pressTick));
    a.pushstring(L, ModeName(g_held.mode));
    return 4;
}

// reach(weapon) -> boolean: the active worm wields it and its last idle sweep had a worm in the swing
int __cdecl LReach(l5::State* L) {
    const int c = CheckName(L, "reach");
    if (c == -2) return Raise(L);
    l5::A().pushboolean(L, c >= 0 && WieldedClaim(nullptr) == c && g_backend->WormInRange());
    return 1;
}

// mode(weapon [, mode]) -> mode | true | nil, reason
int __cdecl LMode(l5::State* L) {
    const int c = CheckName(L, "mode");
    if (c == -2) return Raise(L);
    const auto& a = l5::A();
    if (c < 0) return NilReason(L, "not claimed");
    const int t = a.type(L, 2);
    if (t == l5::kTNone || t == l5::kTNil) {
        a.pushstring(L, ModeName(g_claims[c].mode));
        return 1;
    }
    Mode m;
    if (t != l5::kTString || !ParseMode(a.tostring(L, 2), &m))
        return Fail(L, "wum.sim.melee.mode(weapon, mode): mode must be \"auto\", \"dig\" or \"strike\"");
    if (g_claims[c].mod != ModOf(L)) return NilReason(L, "not the claimant");
    g_claims[c].mode = m;
    a.pushboolean(L, 1);
    return 1;
}

// strike(weapon) -> true | nil, reason
int __cdecl LStrike(l5::State* L) {
    const int c = CheckName(L, "strike");
    if (c == -2) return Raise(L);
    if (c < 0) return NilReason(L, "not claimed");
    if (g_inHit) return NilReason(L, "in hit");
    if (!g_backend) return NilReason(L, "unavailable");
    int slot = -1;
    if (WieldedClaim(&slot) != c) return NilReason(L, "not wielded");
    const char* why = g_backend->Strike();
    if (why) return NilReason(L, why);
    // A dig hold in progress becomes a strike: its release now passes to the engine.
    if (g_held.on && g_held.claim == c) g_held.mode = Mode::Strike;
    l5::A().pushboolean(L, 1);
    return 1;
}

// result{cancel=, kill=, damage=, impulse=, level=} -> true | nil, "not in hit"
int __cdecl LResult(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTTable) return Fail(L, "wum.sim.melee.result(t): t must be a table");
    if (!g_inHit) return NilReason(L, "not in hit");
    HitResult r;
    a.pushstring(L, "cancel");
    a.rawget(L, 1);
    r.cancel = a.type(L, -1) == l5::kTBoolean && a.toboolean(L, -1) != 0;
    a.settop(L, 1);
    a.pushstring(L, "kill");
    a.rawget(L, 1);
    r.kill = a.type(L, -1) == l5::kTBoolean && a.toboolean(L, -1) != 0;
    a.settop(L, 1);
    a.pushstring(L, "level");
    a.rawget(L, 1);
    r.level = a.type(L, -1) == l5::kTBoolean && a.toboolean(L, -1) != 0;
    a.settop(L, 1);
    a.pushstring(L, "damage");
    a.rawget(L, 1);
    int td = a.type(L, -1);
    if (td == l5::kTNumber) {
        r.setDamage = true;
        r.damage = a.tonumber(L, -1);
    }
    a.settop(L, 1);
    a.pushstring(L, "impulse");
    a.rawget(L, 1);
    int ti = a.type(L, -1);
    if (ti == l5::kTNumber) {
        r.setImpulse = true;
        r.impulse = a.tonumber(L, -1);
    }
    a.settop(L, 1);
    if ((td != l5::kTNil && td != l5::kTNumber) || (ti != l5::kTNil && ti != l5::kTNumber) ||
        (r.setDamage && (!std::isfinite(r.damage) || r.damage < 0 || r.damage > 1000)) ||
        (r.setImpulse && (!std::isfinite(r.impulse) || r.impulse < 0 || r.impulse > 100)))
        return Fail(L, "wum.sim.melee.result: damage is 0..1000, impulse 0..100");
    g_result = r;
    a.pushboolean(L, 1);
    return 1;
}

// list() -> {{name=, mod=, dig=, mode=}, ...}
int __cdecl LList(l5::State* L) {
    const auto& a = l5::A();
    a.newtable(L);
    const int list = a.gettop(L);
    int n = 0;
    for (const Claim& c : g_claims) {
        if (!c.used) continue;
        a.newtable(L);
        const int t = a.gettop(L);
        a.pushstring(L, "name");
        a.pushstring(L, c.name);
        a.rawset(L, t);
        a.pushstring(L, "mod");
        a.pushstring(L, simbridge::ModIdAt(c.mod) ? simbridge::ModIdAt(c.mod) : "");
        a.rawset(L, t);
        a.pushstring(L, "dig");
        a.pushboolean(L, c.dig);
        a.rawset(L, t);
        a.pushstring(L, "mode");
        a.pushstring(L, ModeName(c.mode));
        a.rawset(L, t);
        a.rawseti(L, list, ++n);
    }
    return 1;
}

void SetFn(l5::State* L, int t, const char* name, l5::CFunction fn, int mod) {
    const auto& a = l5::A();
    a.pushstring(L, name);
    a.pushnumber(L, static_cast<float>(mod));
    a.pushcclosure(L, fn, 1);
    a.rawset(L, t);
}

void Dispatch(const Queued& q) {
    using simbridge::Arg;
    const Claim& c = g_claims[q.claim];
    if (!c.used) return;
    Arg args[5];
    int n = 0;
    args[n++] = {Arg::Str, 0, c.name};
    args[n++] = {Arg::Num, static_cast<float>(q.tick), nullptr};
    args[n++] = {Arg::Num, static_cast<float>(q.slot), nullptr};
    if (q.ev == Ev::Release) args[n++] = {Arg::Num, static_cast<float>(q.held), nullptr};
    if (q.ev == Ev::Press || q.ev == Ev::Release) args[n++] = {Arg::Str, 0, q.mode ? q.mode : ""};
    simbridge::DispatchArgs(kEvFull[static_cast<int>(q.ev)], args, n);
}
}  // namespace

const char* ModeName(Mode m) {
    switch (m) {
    case Mode::Dig: return "dig";
    case Mode::Strike: return "strike";
    default: return "auto";
    }
}

void SetBackend(Backend* b) { g_backend = b; }
void SetClaimObserver(ClaimFn fn, void* user) {
    g_claimObserver = fn;
    g_claimUser = user;
}

int Claims(ClaimInfo* out, int max) {
    int n = 0;
    for (const Claim& c : g_claims) {
        if (!c.used || n >= max) continue;
        ClaimInfo& o = out[n++];
        snprintf(o.name, sizeof o.name, "%s", c.name);
        o.index = c.index;
        o.mod = c.mod;
        o.dig = c.dig;
        o.mode = c.mode;
    }
    return n;
}

bool Claimed(int weaponIndex) { return ClaimByIndex(weaponIndex) >= 0; }
bool InHit() { return g_inHit; }

bool OnFire(int slot, bool pressed) {
    if (!g_backend || slot < 0) return false;
    if (!pressed) {
        if (!g_held.on || g_held.slot != slot) return false;
        const bool swallow = g_held.mode == Mode::Dig;
        Release(ModeName(g_held.mode));
        return swallow;
    }
    if (g_held.on) return g_held.slot == slot && g_held.mode == Mode::Dig;  // a repeat of the held press
    const int c = ClaimByIndex(g_backend->WeaponOf(slot));
    if (c < 0) return false;
    if (g_backend->EntityState() > 0) return false;  // mid-swing: the engine's own business, no event
    // dig = false: the gate is off, every press goes to the engine; press and release are still reported.
    Mode m = g_claims[c].dig ? g_claims[c].mode : Mode::Strike;
    if (m == Mode::Auto) m = g_backend->WormInRange() ? Mode::Strike : Mode::Dig;
    g_held.on = true;
    g_held.slot = slot;
    g_held.claim = c;
    g_held.pressTick = sim::Tick();
    g_held.mode = m;
    Queue(Ev::Press, c, slot, 0, ModeName(m));
    return m == Mode::Dig;
}

void OnStrike(int slot) {
    const int c = g_backend && slot >= 0 ? ClaimByIndex(g_backend->WeaponOf(slot)) : -1;
    if (c >= 0) Queue(Ev::Strike, c, slot, 0, nullptr);
}

void OnDone(int slot) {
    const int c = g_backend && slot >= 0 ? ClaimByIndex(g_backend->WeaponOf(slot)) : -1;
    if (c >= 0) Queue(Ev::Done, c, slot, 0, nullptr);
}

HitResult OnHit(int attacker, int victim, const float pos[3], float damage, float impulse) {
    HitResult none;
    if (!g_backend || g_inHit || attacker < 0) return none;
    const int c = ClaimByIndex(g_backend->WeaponOf(attacker));
    if (c < 0) return none;
    ++g_hits;
    using simbridge::Arg;
    Arg args[9] = {{Arg::Str, 0, g_claims[c].name},
                   {Arg::Num, static_cast<float>(sim::Tick()), nullptr},
                   {Arg::Num, static_cast<float>(attacker), nullptr},
                   {Arg::Num, static_cast<float>(victim), nullptr},
                   {Arg::Num, pos[0], nullptr},
                   {Arg::Num, pos[1], nullptr},
                   {Arg::Num, pos[2], nullptr},
                   {Arg::Num, damage, nullptr},
                   {Arg::Num, impulse, nullptr}};
    g_result = HitResult{};
    g_inHit = true;
    simbridge::DispatchArgs(kHitEvent, args, 9);
    g_inHit = false;
    const HitResult r = g_result;
    g_result = HitResult{};
    return r;
}

void Tick() {
    if (g_held.on && g_backend) {
        const int active = g_backend->ActiveSlot();
        const int c = active == g_held.slot ? ClaimByIndex(g_backend->WeaponOf(active)) : -1;
        if (c != g_held.claim) Release("cancel");
    }
    if (!g_queued) return;
    Queued local[kQueueCap];
    const int n = g_queued;
    memcpy(local, g_queue, sizeof(Queued) * n);
    g_queued = 0;
    for (int i = 0; i < n; ++i) Dispatch(local[i]);
}

void ResetMatch() {
    for (Claim& c : g_claims) c = Claim{};
    g_held = Held{};
    g_queued = 0;
    g_hits = g_dropped = 0;
    g_inHit = false;
    g_result = HitResult{};
}

uint32_t HitCount() { return g_hits; }

size_t HashBytes(uint8_t* out, size_t cap) {
    if (cap < kHashBytes) return 0;
    size_t n = 0;
    for (const Claim& c : g_claims) {
        out[n++] = static_cast<uint8_t>(c.used ? c.index : 0xff);
        out[n++] = static_cast<uint8_t>(c.mod & 0xff);
        out[n++] = c.dig;
        out[n++] = static_cast<uint8_t>(c.mode);
    }
    out[n++] = g_held.on;
    out[n++] = static_cast<uint8_t>(g_held.slot & 0xff);
    out[n++] = static_cast<uint8_t>(g_held.claim & 0xff);
    memcpy(out + n, &g_held.pressTick, 4);
    n += 4;
    memcpy(out + n, &g_hits, 4);
    n += 4;
    const uint16_t q = static_cast<uint16_t>(g_queued);
    memcpy(out + n, &q, 2);
    n += 2;
    out[n++] = g_backend ? 1 : 0;  // a peer whose Melee module is inert (layout check failed) shows at tick 0
    return n;
}

void PushTable(l5::State* L, int mod) {
    const auto& a = l5::A();
    a.newtable(L);
    const int t = a.gettop(L);
    SetFn(L, t, "claim", &LClaim, mod);
    SetFn(L, t, "on", &LOn, mod);
    SetFn(L, t, "off", &LOff, mod);
    SetFn(L, t, "held", &LHeld, mod);
    SetFn(L, t, "reach", &LReach, mod);
    SetFn(L, t, "mode", &LMode, mod);
    SetFn(L, t, "strike", &LStrike, mod);
    SetFn(L, t, "result", &LResult, mod);
    SetFn(L, t, "list", &LList, mod);
}
}  // namespace melange::simmelee
