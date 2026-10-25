// wum.sim.worm / worms / land and the worm anchor set. No engine addresses here: the Backend is the game (WormAnchor
// module) or the self-test's fake. Lua 5.0 rules: the trampolines keep only plain data alive and raise through
// Raise() after every helper has returned.
#include "lua/sim/sim_worms.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "lua/sim/bridge_internal.h"

namespace melange::simworms {
namespace {
namespace l5 = lua50;
Backend* g_backend = nullptr;
AnchorFn g_observer = nullptr;
void* g_observerUser = nullptr;
char g_err[160];

void Notify(int slot, bool on, bool released) {
    if (g_observer) g_observer(slot, on, released, g_observerUser);
}

struct AnchorState {
    bool anchored = false;   // worm:anchor(true) was called and not undone
    bool pinned = false;     // the pin is still applied (false once the water reached it or the worm died)
    float pin[3] = {0, 0, 0};
};
AnchorState g_anchors[kMaxWorms];

bool ValidSlot(int slot) { return slot >= 0 && slot < kMaxWorms; }

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

// The methods live in one shared table per match VM (the worm tables' metatable, kept in the registry under
// kMethodsKey), so wum.sim.worm allocates one small table per call. They are called as w:fn(...): the slot is read
// from self.slot. -1 when self is not a worm table (the caller raises).
constexpr const char kMethodsKey[] = "melange.simworms.methods";

int SlotOf(l5::State* L) {
    const auto& a = l5::A();
    if (a.type(L, 1) != l5::kTTable) return -1;
    const int top = a.gettop(L);
    a.pushstring(L, "slot");
    a.rawget(L, 1);
    int slot = -1;
    if (a.isnumber(L, -1)) {
        const float f = a.tonumber(L, -1);
        if (std::floor(f) == f && f >= 0 && f < kMaxWorms) slot = static_cast<int>(f);
    }
    a.settop(L, top);
    return slot;
}

// For w.fn() (no self) or a self whose slot is not 0..15.
int BadSelf(l5::State* L) {
    snprintf(g_err, sizeof g_err, "worm methods take the worm table: call them as w:name()");
    return Raise(L);
}

bool ReadSlot(int slot, WormInfo* w) { return g_backend && ValidSlot(slot) && g_backend->Read(slot, w); }

int Push3(l5::State* L, const float v[3]) {
    const auto& a = l5::A();
    a.pushnumber(L, v[0]);
    a.pushnumber(L, v[1]);
    a.pushnumber(L, v[2]);
    return 3;
}

int __cdecl LPos(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    return Push3(L, w.pos);
}

int __cdecl LVel(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    return Push3(L, w.vel);
}

int __cdecl LAlive(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    const bool ok = ReadSlot(slot, &w);
    l5::A().pushboolean(L, ok && Alive(w));
    return 1;
}

int __cdecl LActive(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    const bool ok = ReadSlot(slot, &w);
    l5::A().pushboolean(L, ok && w.active);
    return 1;
}

int __cdecl LTeam(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    l5::A().pushnumber(L, static_cast<float>(w.team));
    l5::A().pushnumber(L, static_cast<float>(w.posInTeam));
    return 2;
}

int __cdecl LHealth(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    l5::A().pushnumber(L, static_cast<float>(w.health));
    return 1;
}

int __cdecl LState(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    l5::A().pushnumber(L, static_cast<float>(w.state));
    return 1;
}

int __cdecl LYaw(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    l5::A().pushnumber(L, w.yaw);
    return 1;
}

int __cdecl LWeapon(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    l5::A().pushnumber(L, static_cast<float>(w.weapon));
    return 1;
}

int __cdecl LName(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, "no worm");
    l5::A().pushstring(L, w.name);
    return 1;
}

int __cdecl LAnchor(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    const auto& a = l5::A();
    const int i = 2;
    const int t = a.type(L, i);
    bool on = true;
    if (t == l5::kTBoolean) on = a.toboolean(L, i) != 0;
    else if (t != l5::kTNone && t != l5::kTNil) {
        snprintf(g_err, sizeof g_err, "worm:anchor(on): on must be a boolean");
        return Raise(L);
    }
    const char* why = nullptr;
    if (!Anchor(slot, on, &why)) return NilReason(L, why ? why : "failed");
    a.pushboolean(L, 1);
    return 1;
}

int __cdecl LAnchored(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    l5::A().pushboolean(L, Anchored(slot));
    l5::A().pushboolean(L, Pinned(slot));
    return 2;
}

int __cdecl LPin(l5::State* L) {
    const int slot = SlotOf(L);
    if (slot < 0) return BadSelf(L);
    float p[3];
    if (!PinOf(slot, p)) return NilReason(L, "not anchored");
    return Push3(L, p);
}

// Pushes the shared method table (also the worm tables' metatable, __index = itself), building it on first use in
// this VM. The registry dies with the VM, so nothing needs freeing at match end.
void PushMethods(l5::State* L) {
    const auto& a = l5::A();
    a.pushstring(L, kMethodsKey);
    a.rawget(L, l5::kRegistry);
    if (a.type(L, -1) == l5::kTTable) return;
    a.settop(L, a.gettop(L) - 1);
    static const struct {
        const char* name;
        l5::CFunction fn;
    } kMethods[] = {{"pos", &LPos},       {"vel", &LVel},       {"alive", &LAlive},   {"active", &LActive},
                    {"team", &LTeam},     {"health", &LHealth}, {"state", &LState},   {"yaw", &LYaw},
                    {"weapon", &LWeapon}, {"name", &LName},     {"anchor", &LAnchor}, {"anchored", &LAnchored},
                    {"pin", &LPin}};
    a.newtable(L);
    const int t = a.gettop(L);
    for (const auto& m : kMethods) {
        a.pushstring(L, m.name);
        a.pushcclosure(L, m.fn, 0);
        a.rawset(L, t);
    }
    a.pushstring(L, "__index");
    a.pushvalue(L, t);
    a.rawset(L, t);
    // Shared by every mod in the VM: getmetatable(w) answers "worm" and setmetatable(w, ...) raises, so no mod can
    // change another's methods.
    a.pushstring(L, "__metatable");
    a.pushstring(L, "worm");
    a.rawset(L, t);
    a.pushstring(L, kMethodsKey);
    a.pushvalue(L, t);
    a.rawset(L, l5::kRegistry);
}
}  // namespace

void SetBackend(Backend* b) { g_backend = b; }
Backend* GetBackend() { return g_backend; }
void SetAnchorObserver(AnchorFn fn, void* user) {
    g_observer = fn;
    g_observerUser = user;
}

bool Alive(const WormInfo& w) {
    return w.active && w.health > 0 && w.state != kDeathThroes && w.state != kDrownFloat;
}

bool Anchor(int slot, bool on, const char** why) {
    if (!ValidSlot(slot)) {
        if (why) *why = "no worm";
        return false;
    }
    AnchorState& s = g_anchors[slot];
    if (!on) {
        const bool was = s.anchored;
        if (was && g_backend) {
            WormInfo w;
            if (g_backend->Read(slot, &w)) g_backend->Write(slot, w.pos, w.vel, 0, kNailFlags);
        }
        s = AnchorState{};
        if (was) Notify(slot, false, false);
        return true;
    }
    WormInfo w;
    if (!g_backend) {
        if (why) *why = "unavailable";
        return false;
    }
    if (!g_backend->Read(slot, &w)) {
        if (why) *why = "no worm";
        return false;
    }
    if (!Alive(w)) {
        if (why) *why = "dead";
        return false;
    }
    if (s.anchored) return true;  // keeps its first pin
    s.anchored = s.pinned = true;
    memcpy(s.pin, w.pos, sizeof s.pin);
    Repin(slot);
    Notify(slot, true, false);
    return true;
}

bool Anchored(int slot) { return ValidSlot(slot) && g_anchors[slot].anchored; }
bool Pinned(int slot) { return ValidSlot(slot) && g_anchors[slot].pinned; }

bool PinOf(int slot, float out[3]) {
    if (!Anchored(slot)) return false;
    memcpy(out, g_anchors[slot].pin, 12);
    return true;
}

void Repin(int slot) {
    if (!ValidSlot(slot) || !g_backend) return;
    AnchorState& s = g_anchors[slot];
    if (!s.anchored || !s.pinned) return;
    WormInfo w;
    if (!g_backend->Read(slot, &w)) return;
    if (w.state == kDeathThroes || w.state == kDrownFloat) {
        // The engine owns the worm from here: it sinks or lies where it died. Its drowning check reads the pinned
        // position, so a worm the water rises over enters DrownFloat on its own (verified in game, 2026-10-24).
        s.pinned = false;
        Notify(slot, false, true);
        return;
    }
    static const float kZero[3] = {0, 0, 0};
    g_backend->Write(slot, s.pin, kZero, kNailFlags, 0);
}

void RepinAll() {
    for (int i = 0; i < kMaxWorms; ++i)
        if (g_anchors[i].anchored && g_anchors[i].pinned) Repin(i);
}

void ResetMatch() {
    for (auto& s : g_anchors) s = AnchorState{};
}

int AnchoredCount() {
    int n = 0;
    for (const auto& s : g_anchors) n += s.anchored;
    return n;
}

size_t HashBytes(uint8_t* out, size_t cap) {
    if (cap < kHashBytes) return 0;
    size_t n = 0;
    for (const auto& s : g_anchors) {
        out[n++] = s.anchored;
        out[n++] = s.pinned;
        memcpy(out + n, s.pin, 12);
        n += 12;
    }
    return n;
}

int __cdecl LWorm(l5::State* L) {
    const auto& a = l5::A();
    if (!a.isnumber(L, 1)) {
        snprintf(g_err, sizeof g_err, "wum.sim.worm(slot): slot must be a number (0 to 15)");
        return Raise(L);
    }
    const float f = a.tonumber(L, 1);
    if (!(std::floor(f) == f) || f < 0 || f >= kMaxWorms) return NilReason(L, "no worm");
    const int slot = static_cast<int>(f);
    WormInfo w;
    if (!ReadSlot(slot, &w)) return NilReason(L, g_backend ? "no worm" : "unavailable");
    if (!a.checkstack(L, 6)) return NilReason(L, "no stack");
    a.newtable(L);
    const int t = a.gettop(L);
    a.pushstring(L, "slot");
    a.pushnumber(L, static_cast<float>(slot));
    a.rawset(L, t);
    PushMethods(L);
    a.setmetatable(L, t);
    return 1;
}

int __cdecl LWorms(l5::State* L) {
    const auto& a = l5::A();
    a.newtable(L);
    const int t = a.gettop(L);
    int n = 0;
    WormInfo w;
    for (int i = 0; i < kMaxWorms; ++i) {
        if (!ReadSlot(i, &w)) continue;
        a.pushnumber(L, static_cast<float>(i));
        a.rawseti(L, t, ++n);
    }
    a.pushnumber(L, static_cast<float>(n));
    return 2;
}

int __cdecl LLand(l5::State* L) {
    float b[6];
    if (!g_backend || !g_backend->Land(b)) return NilReason(L, g_backend ? "no land" : "unavailable");
    const auto& a = l5::A();
    for (float v : b) a.pushnumber(L, v);
    return 6;
}

bool AddSimFunctions() {
    bool ok = simbridge::AddSimFunction("worm", &LWorm);
    ok = simbridge::AddSimFunction("worms", &LWorms) && ok;
    ok = simbridge::AddSimFunction("land", &LLand) && ok;
    return ok;
}
}  // namespace melange::simworms
