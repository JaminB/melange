// The level's mine factory in the match, build #1077 (docs/erg.md, The mine factory).
//
// MineFactoryLogicEntity's set-up (0x5d0170) first drops the factory from its knot (0x5cf710: a land sweep of at most
// 1000 ticks from the knot, the hit point copied from the sweep's globals whether or not it hit), then reads
// kMineFactoryData.NumTurnsInactive (+0x2d) into the idle countdown (+0x88); each GameLogic.StartMineFactory
// (0x5cfe40) decrements the countdown and, at 0 or under, fires min(NumMineActivation (+0x2c) - mines, 10) when fewer
// mines than the target are in the level (+0x8c: the fire state, +0x84: the mines to fire). registry writes the level's
// values into kMineFactoryData at level start; the hook on the drop writes them again just before the set-up reads
// them (the match's VM is open then, so the tweak store still puts the game's back when it closes) and records what it
// found there, so a writer between level start and the factory's placement shows in the report: in Tests on
// 2026-11-08 it held the game's 20 and 7 again by then, which is why the level's settings did not reach the factory.
// With keepKnot a drop that misses leaves the factory at its knot instead of the stale point. A miss is a sweep that
// reports no hit, or one whose point is not under the knot (the sweep is straight down; over a draped skin it reported
// a hit 63 map units off, the stale point).
//
// Where the mines come from: MineFactoryLogicEntity::PlaceMines (0x5d05c0, a thiscall of no arguments) gathers the
// positions of the living worms (0x5b27a0, 16 slots) and of the level's objects (0x4f5900/0x4f5910), then for each of
// the m_nNumMinesToSpawn (+0x84, 1-10) mines picks one of them at random (the sim's random, 0x68c015), offsets it by a
// random angle and radius, rejects spots too close to another, and makes the mine there high up with the velocity
// (0, -0.3, 0) (GameLogicService::CreateMine 0x4f9c40): mines rain onto the worms, not round the factory (on Caldera
// they landed 28-95 map units out, the worms' distances). With the level's launchHeight / launchRadius the hook makes
// them itself instead: each over the factory's own position, launchHeight up and at a random spot within launchRadius
// of it (an even spread over the disc), falling with the same velocity, two of the sim's randoms a mine so every machine
// places them alike.
//
// With launchers (several launch points, the one engine factory's: GameLogicService asserts "Only allowed one mine
// factory at a time" when a second is made, tried 2026-11-08) the fire's mines are shared out over them, starting at a
// random one, and each erupts straight up from a spot within launchRadius of its launcher, 1 map unit over the
// launcher's height, so it tops out launchHeight over that and falls back onto the spot. The spots of a fire are kept
// kMinApart from each other where they can be. The velocity CreateMine takes is in world units per millisecond, and a
// mine falls at kMineGravity (measured on 2026-11-08, testenv B: throws of 0.05-0.8 from 20, 160 and 560 over a flat
// island landed where 2.5e-4 world units/ms^2 puts them, within the quarter-second sampling). A mine landing while
// moving sideways (about 0.05 world units/ms or more) often sank into the voxels (on a solid block it came to rest a
// layer down; arcs thrown across Caldera's crater lost 25 of 30), while straight falls and lobs rested on the top, so the
// mines go straight up and down. Some ground lets even those through (on Caldera four of eight launcher pads held no
// mine, the same pads every time; docs/erg.md): level.test.dropMines finds it.
//
// The throw (FactorySpec::throwing, 2026-11-09): each mine of a fire is lobbed from its launcher (or the factory) at a
// landing point minRange-maxRange map units out (pattern random, ring or targets), the ground there read with the
// engine's own straight land sweep (gamestate::LandRay, 0x466ae0) so a point in the sea is drawn again, and its
// velocity is the ballistic one that tops out `apex` over the higher end. Melange then lands it itself: every sim tick
// the mine's analytic position (the parabolic payload keeps m_vInitialPosition +0x28, m_vInitialVelocity +0x34,
// m_vAcceleration +0x40 and m_tInitialTime +0x1a0, and moves nothing until a collision re-bases them) is swept from
// the last tick's with LandRay, and at the first hit the mine is put there and the engine's own come-to-rest
// (ParabolicPayloadLogicEntity 0x5758c0: posts the landing, zeroes the velocity and acceleration, arms the proximity
// fuse through the usual rest path) is called on it. Why: the engine's mine collision (the collider service's step
// 0x5150e0, the same sweep over 20 ticks from the sphere's bottom) lets mines through some voxel frames while a
// straight sweep of the same function hits them: on Caldera (2026-11-09, testenv B) LandRay found the ground top at
// all 84 probe columns while straight drops at the 28 launcher-pad columns all fell through; on a 24-block test
// level two blocks (ids -1 and -13, grass01 and dinoegg02 at scale 1) let 6 of 6 drops through in one match and held
// 16 of 16 drops and 4 sideways throws (0.05-0.3 world units/ms, resting at the top, no sinking) in the next, with 12
// materials and scales 1, 2 and 2.5 holding alike: the miss is frame-wise and state-dependent, not a matter of
// material, scale or height, and its state (the land's broadphase mask and ray cache, which LandRay saves and puts back
// round its own calls, are the candidates) was not pinned down. Landing at the straight sweep's hit also leaves no
// sideways speed to slide or sink on. If the engine collides first (a worm, an object, a frame it does see), the mine is
// rested at the contact point when the ground is within 2 map units of it, else its new parabola is followed.
//
// Every fire is followed to its end (factory.fired on the session log, FiresJson): the mine entities that appear in
// PlaceMines (either way) with where each appeared, then, from the sim tick, where each comes to rest (the entity list
// moves a mine's position from its launch point to where it lies once it rests) or that it is gone. A mine counts as at
// rest once its position has not changed for kRestMs; a pause in a bounce is shorter. After the fire is logged its mines
// are still watched (kWatchMs): a mine that moves again (knocked by a blast, sliding) gets its new final position, and
// one that goes gets its fate, drowned (under the water when it went), destroyed (set off, or removed, after it had
// landed) or gone before it landed (its position is still the launch point then: sunk, or set off in the air), so FiresJson
// gives where each mine ended up, not a sample taken on its way (factory.mine on the session log for each such change).
// FireNow starts a
// fire outside a turn start for level.test.fireFactory: StartMineFactory (0x5cfe40) decrements the countdown, returns
// while it is over 0, then counts the level's mines (GameLogicService +0x1d8 vector) and fires only when they are
// fewer than NumMineActivation (read from kMineFactoryData at that moment), min(target - mines, 10) mines.
#include "levels/factory.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include <safetyhook.hpp>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "lua/sim/sim_core.h"
#include "lua/sim/tweak.h"
#include "melange/gamestate.h"
#include "melange/jlog.h"
#include "melange/sim.h"
#include "tools/json_mini.h"

namespace melange::levels::factory {
namespace {
constexpr uintptr_t kDrop = 0x5cf710;          // void __thiscall MineFactoryLogicEntity::Drop()
constexpr uintptr_t kSweepTicks = 0x952cfc;    // the last land sweep's hit tick; 1000 = no hit
constexpr int kNoHit = 1000;
constexpr float kUnderKnot = 20;               // world units: a hit further from the knot in x, z is not under it
constexpr uintptr_t kCreateMine = 0x4f9c40;    // void __thiscall GameLogicService::CreateMine(pos*, pos*, vel*)
constexpr uintptr_t kLogicService = 0x95aa14;  // GameLogicService*
constexpr uintptr_t kPlaceMines = 0x5d05c0;   // void __thiscall MineFactoryLogicEntity::PlaceMines()
constexpr uintptr_t kSimRandom = 0x68c015;    // uint32_t __cdecl (the sim's random, on its generator at 0x96d034)
constexpr uintptr_t kStartFactory = 0x5cfe40;  // void __thiscall MineFactoryLogicEntity::StartMineFactory()
constexpr uint32_t kMinesBegin = 0x1d8, kMinesEnd = 0x1dc;   // GameLogicService: the level's mines (a vector of 4-byte entries)
constexpr uint32_t kPos = 0x2c, kNormal = 0x38, kExploded = 0x48, kCountdown = 0x88, kFireState = 0x8c, kToSpawn = 0x84;
constexpr float kWorldPerMap = 20.f;
constexpr float kMineGravity = 2.5e-4f;        // world units per ms^2: a mine's fall (CreateMine's velocity is per ms)
// The parabolic payload (vtable 0x85b504): its flight's base, and the come-to-rest the collision path calls once the
// velocity after a bounce is zero (ParabolicPayloadLogicEntity.cpp 0x22c-0x25c: the landing message, m_vInitialVelocity
// and m_vAcceleration zeroed, the expiry clamped).
constexpr uintptr_t kParaRest = 0x5758c0, kTaskManager = 0x96d030;
constexpr uint32_t kPInitPos = 0x28, kPInitVel = 0x34, kPAccel = 0x40, kPInitTime = 0x1a0, kTmClock = 0x38;
constexpr uint32_t kMaxFlightMs = 20000;       // a thrown mine not down by then is let go
constexpr int kTargetTries = 6;                // landing points drawn before a mine falls back to straight up
constexpr float kRestSnap = 2.f * kWorldPerMap;   // a bounce's contact point this close to the land (either way) rests there
constexpr float kThrowLift = 2.5f * kWorldPerMap;   // world units a thrown mine appears over its launch point (see the throw)
constexpr float kMinApart = 30.f;              // world units: a launcher's mines start at least this far apart, if they can
constexpr int kSpotTries = 12;

struct Field {
    bool asked = false;
    int value = 0;
    std::optional<float> found;   // kMineFactoryData's value just before the set-up's write
    std::string result;           // the tweak store's answer
};
struct State {
    bool armed = false;
    std::string stem;
    erg::FactorySpec spec;
    // the last set-up
    bool reported = false;
    std::string reportStem;
    Field activation, inactive;
    float knot[3] = {}, settled[3] = {};
    bool hit = false, keptKnot = false;
};
std::mutex g_mx;
State g;
SafetyHookInline g_hDrop, g_hPlace;

// The fires being followed and the last ones done (main thread: PlaceMines and the sim tick).
struct FiredMine {
    uint32_t handle = 0;
    float spawn[3] = {}, last[3] = {};
    bool thrown = false;          // a throw: aimed at target (world units)
    float target[3] = {};
    const char* landing = "";     // thrown: "melange" (landed at the sweep's hit), "engine" (the engine's own rest), "water", "lost", "" (in flight)
    ULONGLONG changedAt = 0;
    bool moved = false, done = false, gone = false;
    int moves = 0;                // position changes seen
    int restsAfter = 0;           // times it moved again after it had come to rest (a blast, a slide)
    bool drowned = false;         // gone with its last position under the water
    uint32_t goneTick = 0;
    float waterThen = NAN;        // world units, when it went
};
struct Fire {
    std::string stem;
    uint32_t tick = 0, turn = 0;
    int asked = 0;
    bool launch = false;
    bool thrown = false;
    float factory[3] = {};
    ULONGLONG at = 0;
    bool logged = false;
    std::vector<FiredMine> mines;
};
constexpr ULONGLONG kRestMs = 2500, kFollowMs = 30000, kWatchMs = 180000;
constexpr size_t kKeepFires = 8;
std::vector<Fire> g_following;
std::vector<Fire> g_fired;          // the logged fires, oldest first, still watched for kWatchMs
std::string g_trigger = "null";     // the last FireNow
int g_tickHook = 0;
std::atomic<uint32_t> g_fireSerial{0};   // fires seen this session (PlaceMines), for a shot after a fire
std::atomic<ULONGLONG> g_fireAt{0};      // GetTickCount64 of the last
Flight g_flight;                         // the last fire's first mine (main thread: PlaceMines, render.fireShot's frame)
bool g_hasFlight = false;
bool g_restOk = false;                   // kParaRest matches build #1077 (Install)
bool g_landRayWarned = false;

// A thrown mine Melange lands (main thread: PlaceMines and the sim tick).
struct Guided {
    uint32_t handle = 0;
    uintptr_t object = 0, vtable = 0;   // the entity at launch: a changed vtable means the object is gone (freed, reused)
    bool parabolic = false;
    float p0[3] = {}, v0[3] = {}, a[3] = {};   // the parabola last seen (the engine re-bases it at a collision)
    uint32_t t0 = 0, lastT = 0;
    float target[3] = {};
    uint32_t fireTick = 0;
    bool pinned = false;        // landed: held at `at` while the engine leaves it at rest (see GuideThrown)
    float at[3] = {};
    int rePins = 0;
};
std::vector<Guided> g_guided;
struct Throw {       // one mine of a throw, for pairing the entities that appear with their targets
    float from[3], target[3], vel[3];
};
/** Within this of the factory's axis (map units, horizontally) a mine at rest is "under the factory". */
constexpr float kUnderFactoryMap = 2.5f;

bool ReadVec(uintptr_t at, float out[3]) { return mem::SafeRead(at, out, sizeof(float) * 3); }
float WaterNow();

void ApplyField(const char* name, const std::optional<int>& v, Field* f) {
    *f = Field{};
    if (!v) return;
    f->asked = true;
    f->value = *v;
    float was = 0;
    if (tweak::Get("kMineFactoryData", name, &was) == tweak::TweakError::Ok) f->found = was;
    f->result = tweak::ToString(tweak::Set("kMineFactoryData", name, static_cast<float>(*v)));
}

void __fastcall HkDrop(uintptr_t self, void*) {
    State s;
    {
        std::lock_guard lk(g_mx);
        s = g;
    }
    if (!s.armed) {
        g_hDrop.thiscall<void>(self);
        return;
    }
    ApplyField("NumMineActivation", s.spec.activation, &s.activation);
    ApplyField("NumTurnsInactive", s.spec.inactiveTurns, &s.inactive);
    ReadVec(self + kPos, s.knot);
    g_hDrop.thiscall<void>(self);
    int ticks = kNoHit;
    mem::SafeRead(kSweepTicks, &ticks, sizeof ticks);
    ReadVec(self + kPos, s.settled);
    s.hit = ticks != kNoHit && std::hypot(s.settled[0] - s.knot[0], s.settled[2] - s.knot[2]) <= kUnderKnot;
    s.keptKnot = false;
    if (!s.hit && s.spec.keepKnot) {
        const float up[3] = {0, 1, 0};
        s.keptKnot = mem::Write(self + kPos, s.knot, sizeof s.knot) && mem::Write(self + kNormal, up, sizeof up);
    }
    ReadVec(self + kPos, s.settled);
    s.reported = true;
    s.reportStem = s.stem;
    auto fieldText = [](const Field& f) {
        if (!f.asked) return std::string("game's");
        char b[96];
        if (f.found) snprintf(b, sizeof b, "%d (was %g): %s", f.value, static_cast<double>(*f.found), f.result.c_str());
        else snprintf(b, sizeof b, "%d: %s", f.value, f.result.c_str());
        return std::string(b);
    };
    LOG_INFO("[levels] %s: the mine factory's set-up: kMineFactoryData.NumMineActivation %s, NumTurnsInactive %s; its drop "
             "from (%.1f, %.1f, %.1f) %s (%.1f, %.1f, %.1f)%s",
             s.stem.c_str(), fieldText(s.activation).c_str(), fieldText(s.inactive).c_str(), s.knot[0], s.knot[1], s.knot[2],
             s.hit ? "landed at" : "hit no land; it is at", s.settled[0], s.settled[1], s.settled[2],
             s.hit ? "" : s.keptKnot ? " (keepKnot: its knot)" : " (the stale point of the match's last sweep)");
    std::lock_guard lk(g_mx);
    State keep = g;
    g = s;
    g.armed = keep.armed;
    g.spec = keep.spec;
    g.stem = keep.stem;
}

const char* EngineMine(const float pos[3], const float vel[3]);

// The mine entities of the match and where they are (main thread).
struct MineAt {
    uint32_t handle;
    float pos[3];
    uintptr_t object = 0, vtable = 0;
    bool parabolic = false;   // the entity list's class is ParabolicPayloadLogicEntity (the come-to-rest below is its)
};
std::vector<MineAt> Mines() {
    static std::vector<gamestate::Entity> ents(1024);
    int n = gamestate::Entities(ents.data(), static_cast<int>(ents.size()));
    n = (std::min)(n, static_cast<int>(ents.size()));
    std::vector<MineAt> out;
    for (int i = 0; i < n; ++i) {
        const gamestate::Entity& e = ents[static_cast<size_t>(i)];
        if (e.kind != gamestate::EntityKind::Projectile || !e.hasPos) continue;
        char both[96];
        snprintf(both, sizeof both, "%s %s", e.type, e.label);
        for (char* c = both; *c; ++c) *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
        if (!std::strstr(both, "mine") || std::strstr(both, "factory")) continue;
        out.push_back({e.handle, {e.pos.x, e.pos.y, e.pos.z}, e.object, e.vtable, std::strcmp(e.type, "ParabolicPayloadLogicEntity") == 0});
    }
    return out;
}

uint32_t LogicTime() {
    uintptr_t tm = 0;
    uint32_t t = 0;
    return mem::SafeRead(kTaskManager, &tm, sizeof tm) && tm && mem::SafeRead(tm + kTmClock, &t, sizeof t) ? t : 0;
}

// The land under (x, z) by the engine's straight sweep: the top's world y, or nothing (no land: the sea, or outside).
std::optional<float> GroundAt(float x, float z, float fromY) {
    const float top = (std::max)(3600.f, fromY + 1000.f);
    gamestate::LandHit h{};
    if (gamestate::LandRay({x, top, z}, {x, top - 4000.f, z}, &h) != gamestate::LandRayResult::Hit) return std::nullopt;
    return top - 4000.f * h.t;
}

// The velocity (world units per ms) that lobs a mine from `from` to `to`, topping out apexWorld over the higher end.
void ThrowVelocity(const float from[3], const float to[3], float apexWorld, float out[3]) {
    const float apexY = (std::max)(from[1], to[1]) + (std::max)(apexWorld, kWorldPerMap);
    const float vy = std::sqrt(2.f * kMineGravity * (apexY - from[1]));
    const float tUp = vy / kMineGravity, tDown = std::sqrt(2.f * (apexY - to[1]) / kMineGravity), total = tUp + tDown;
    out[0] = (to[0] - from[0]) / total;
    out[1] = vy;
    out[2] = (to[2] - from[2]) / total;
}

// Where a thrown mine is at logic time t on its parabola.
void At(const Guided& g, uint32_t t, float out[3]) {
    const float dt = static_cast<float>(static_cast<int32_t>(t - g.t0));
    for (int k = 0; k < 3; ++k) out[k] = g.p0[k] + g.v0[k] * dt + 0.5f * g.a[k] * dt * dt;
}

bool ReadParabola(uintptr_t obj, float p0[3], float v0[3], float a[3], uint32_t* t0) {
    return mem::SafeRead(obj + kPInitPos, p0, 12) && mem::SafeRead(obj + kPInitVel, v0, 12) && mem::SafeRead(obj + kPAccel, a, 12) &&
           mem::SafeRead(obj + kPInitTime, t0, 4);
}

// The mine put at rest at `at`: its flight's base moved there and the engine's own come-to-rest run on it (or, on
// another build, the velocity and acceleration zeroed by hand).
bool RestAt(uintptr_t obj, bool parabolic, const float at[3], uint32_t now) {
    if (!mem::Write(obj + kPInitPos, at, 12) || !mem::Write(obj + kPInitTime, &now, 4)) return false;
    if (g_restOk && parabolic) {
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(kParaRest)(obj);
        return true;
    }
    const float zero[3] = {0, 0, 0};
    return mem::Write(obj + kPInitVel, zero, 12) && mem::Write(obj + kPAccel, zero, 12);
}

void SetLanding(uint32_t handle, const char* how, const float* at) {
    for (std::vector<Fire>* fires : {&g_following, &g_fired})
        for (Fire& f : *fires)
            for (FiredMine& m : f.mines)
                if (m.handle == handle) {
                    m.landing = how;
                    if (at) std::memcpy(m.last, at, sizeof m.last);
                }
}

bool Guiding(uint32_t handle) {
    for (const Guided& g : g_guided)
        if (g.handle == handle) return !g.pinned;
    return false;
}

// Every sim tick: each thrown mine still in flight is swept from where it was last tick to where it is now, and
// landed at the first hit. See the throw notes at the top.
void GuideThrown(uint32_t tick) {
    if (g_guided.empty()) return;
    const uint32_t now = LogicTime();
    const float water = WaterNow();
    for (size_t i = 0; i < g_guided.size();) {
        Guided& g = g_guided[i];
        float p0[3], v0[3], a[3];
        uint32_t t0 = 0;
        uintptr_t vt = 0;
        const bool alive = mem::SafeRead(g.object, &vt, sizeof vt) && vt == g.vtable && ReadParabola(g.object, p0, v0, a, &t0);
        auto drop = [&](const char* how, const float* at) {
            SetLanding(g.handle, how, at);
            g_guided.erase(g_guided.begin() + static_cast<std::ptrdiff_t>(i));
        };
        auto pin = [&](const float* at) {
            SetLanding(g.handle, "melange", at);
            std::memcpy(g.at, at, sizeof g.at);
            g.pinned = true;
            ++i;
        };
        if (!alive || !now) {
            if (g.pinned) {
                if (g.rePins) LOG_INFO("[levels] thrown mine %u is gone (set off, or removed) after %d re-pins at its landing", g.handle, g.rePins);
                g_guided.erase(g_guided.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                LOG_INFO("[levels] thrown mine %u let go at tick %u: %s", g.handle, tick, !now ? "no sim clock" : !alive ? "its entity is gone" : "unreadable");
                drop("lost", nullptr);
            }
            continue;
        }
        if (g.pinned) {
            // landed: the engine keeps simulating a resting mine's contact with the ground, and where its own sweep does
            // not see the ground it lets the mine fall or roll through (the Caldera volley run of 2026-11-09: 32 of 58
            // landed mines ended 2-12 map units under the land the game's own vertical ray finds at their spot, 11 of
            // them on the shore shelf under a 4.5 beach), so a mine at rest is held where it landed: moved with no
            // velocity or acceleration of its own, it is put back. A blast that gives it a velocity frees it.
            const bool still = v0[0] == 0.f && v0[1] == 0.f && v0[2] == 0.f && a[0] == 0.f && a[1] == 0.f && a[2] == 0.f;
            if (!still) {
                LOG_INFO("[levels] thrown mine %u was set moving again at tick %u (%.3f, %.3f, %.3f per ms): no longer held", g.handle, tick, v0[0], v0[1], v0[2]);
                g_guided.erase(g_guided.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
            if (std::fabs(p0[0] - g.at[0]) + std::fabs(p0[1] - g.at[1]) + std::fabs(p0[2] - g.at[2]) > 0.3f) {
                if (!g.rePins)
                    LOG_INFO("[levels] thrown mine %u moved at rest from (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f) at tick %u; held at its landing", g.handle, g.at[0], g.at[1],
                             g.at[2], p0[0], p0[1], p0[2], tick);
                ++g.rePins;
                mem::Write(g.object + kPInitPos, g.at, 12);
                SetLanding(g.handle, "melange", g.at);
            }
            ++i;
            continue;
        }
        auto differs = [](const float* x, const float* y) { return std::fabs(x[0] - y[0]) + std::fabs(x[1] - y[1]) + std::fabs(x[2] - y[2]) > 1e-6f; };
        // CreateMine leaves the velocity and acceleration zero; the engine launches the mine (its flight's base) on a later
        // tick, which is when the parabola to follow is known
        if (!differs(g.v0, g.a) && g.v0[0] == 0.f && g.v0[1] == 0.f && g.v0[2] == 0.f) {
            if (differs(v0, g.v0) || differs(a, g.a)) {
                std::memcpy(g.p0, p0, 12), std::memcpy(g.v0, v0, 12), std::memcpy(g.a, a, 12);
                g.t0 = t0;
                g.lastT = (std::min)(now, t0);
                if (i == 0)
                    LOG_INFO("[levels] thrown mine %u launched by the engine at tick %u: (%.3f, %.3f, %.3f) per ms, gravity %.5f, since %u (sim clock %u)", g.handle, tick,
                             v0[0], v0[1], v0[2], -a[1], t0, now);
            } else if (now - g.t0 > 2000) {
                LOG_WARN("[levels] thrown mine %u was never launched by the engine (no velocity after 2 s); let go", g.handle);
                drop("lost", nullptr);
                continue;
            }
            ++i;
            continue;
        }
        const bool rebased = t0 != g.t0 || differs(p0, g.p0) || differs(v0, g.v0) || differs(a, g.a);
        if (rebased) {
            // the engine collided with something: at rest already, or bouncing
            const float speed = std::hypot(v0[0], v0[1], v0[2]), accel = std::hypot(a[0], a[1], a[2]);
            gamestate::LandHit h{};
            const gamestate::Vec3 up{p0[0], p0[1] + kRestSnap, p0[2]}, down{p0[0], p0[1] - kRestSnap, p0[2]};
            const bool nearLand = gamestate::LandRay(up, down, &h) == gamestate::LandRayResult::Hit;
            if (speed == 0.f && accel == 0.f) {
                // the engine's own rest; lifted onto the land if it sank in
                if (nearLand) {
                    const float top = up.y - 2.f * kRestSnap * h.t;
                    if (top > p0[1] + 1.f) {
                        const float at[3] = {p0[0], top, p0[2]};
                        mem::Write(g.object + kPInitPos, at, 12);
                        LOG_INFO("[levels] thrown mine %u: the engine rested it %.1f world units under the land; lifted onto it", g.handle, top - p0[1]);
                    }
                }
                SetLanding(g.handle, "engine", p0);
                std::memcpy(g.at, p0, sizeof g.at);
                if (nearLand) {
                    const float top = up.y - 2.f * kRestSnap * h.t;
                    if (top > p0[1] + 1.f) g.at[1] = top;
                }
                g.pinned = true;
                ++i;
                continue;
            }
            if (nearLand) {
                const float at[3] = {p0[0], up.y - 2.f * kRestSnap * h.t, p0[2]};
                RestAt(g.object, g.parabolic, at, now);
                LOG_INFO("[levels] thrown mine %u: bounced at tick %u; rested on the land there (%.1f, %.1f, %.1f)", g.handle, tick, at[0], at[1], at[2]);
                pin(at);
                continue;
            }
            // off a worm or an object in the air: its new parabola is followed
            std::memcpy(g.p0, p0, 12), std::memcpy(g.v0, v0, 12), std::memcpy(g.a, a, 12);
            g.t0 = t0;
            g.lastT = now;
            ++i;
            continue;
        }
        float prev[3], cur[3];
        At(g, g.lastT, prev);
        At(g, now, cur);
        if (!std::isnan(water) && cur[1] < water - 2.f * kWorldPerMap) {
            drop("water", cur);
            continue;
        }
        if (now - g.t0 > kMaxFlightMs) {
            drop("lost", cur);
            continue;
        }
        gamestate::LandHit h{};
        const gamestate::LandRayResult r = gamestate::LandRay({prev[0], prev[1], prev[2]}, {cur[0], cur[1], cur[2]}, &h);
        if (r == gamestate::LandRayResult::Hit) {
            float at[3];
            for (int k = 0; k < 3; ++k) at[k] = prev[k] + (cur[k] - prev[k]) * h.t;
            if (RestAt(g.object, g.parabolic, at, now)) {
                LOG_INFO("[levels] thrown mine %u landed at tick %u, %u ms out: (%.1f, %.1f, %.1f) world, %.1f map from its target", g.handle, tick,
                         now - g.t0, at[0], at[1], at[2], std::hypot(at[0] - g.target[0], at[2] - g.target[2]) / kWorldPerMap);
                pin(at);
            } else drop("lost", at);
            continue;
        }
        if (r == gamestate::LandRayResult::Unavailable || r == gamestate::LandRayResult::Budget) {
            if (!g_landRayWarned) LOG_WARN("[levels] the land sweep is unavailable: thrown mines land by the engine's own collision");
            g_landRayWarned = true;
        }
        g.lastT = now;
        ++i;
    }
}

// After a PlaceMines: the mines that were not there before it are this fire's. With `throws` (a throw's mines, each
// from a launch point at a target) each new mine is paired with the throw it appeared at and guided to the land.
void FollowFire(const std::string& stem, const std::vector<MineAt>& before, int asked, bool launch, const float at[3], float vy,
                const std::vector<Throw>* throws = nullptr) {
    Fire f;
    f.stem = stem;
    f.tick = sim::Tick();
    gamestate::Snapshot snap;
    if (gamestate::Read(&snap) && snap.match.inMatch) f.turn = snap.match.turnsStarted;
    f.asked = asked;
    f.launch = launch;
    f.thrown = throws && !throws->empty();
    std::memcpy(f.factory, at, sizeof f.factory);
    f.at = GetTickCount64();
    std::vector<bool> used(throws ? throws->size() : 0, false);
    const Throw* first = nullptr;
    for (const MineAt& m : Mines()) {
        bool old = false;
        for (const MineAt& b : before) old |= b.handle == m.handle;
        if (old) continue;
        FiredMine fm;
        fm.handle = m.handle;
        std::memcpy(fm.spawn, m.pos, sizeof fm.spawn);
        std::memcpy(fm.last, m.pos, sizeof fm.last);
        fm.changedAt = f.at;
        if (throws) {
            // the throw this mine appeared at: its launch point and (several mines leave one point) the velocity
            // CreateMine gave it, read back from the payload, each throw taken once
            float mv[3] = {0, 0, 0};
            const bool hasVel = mem::SafeRead(m.object + kPInitVel, mv, 12);
            size_t best = SIZE_MAX;
            float bestD = 1e9f;
            for (size_t i = 0; i < throws->size(); ++i) {
                if (used[i]) continue;
                const Throw& t = (*throws)[i];
                float d = std::hypot(t.from[0] - m.pos[0], t.from[1] - m.pos[1], t.from[2] - m.pos[2]);
                if (hasVel) d += 1000.f * std::hypot(t.vel[0] - mv[0], t.vel[1] - mv[1], t.vel[2] - mv[2]);
                if (d < bestD) bestD = d, best = i;
            }
            if (best != SIZE_MAX && bestD < kWorldPerMap) {
                used[best] = true;
                const Throw& t = (*throws)[best];
                if (!first) first = &t;
                fm.thrown = true;
                std::memcpy(fm.target, t.target, sizeof fm.target);
                Guided g;
                g.handle = m.handle;
                g.object = m.object;
                g.vtable = m.vtable;
                g.parabolic = m.parabolic;
                g.fireTick = f.tick;
                if (ReadParabola(m.object, g.p0, g.v0, g.a, &g.t0)) {
                    g.lastT = g.t0;
                    std::memcpy(g.target, t.target, sizeof g.target);
                    if (g_guided.empty())
                        LOG_INFO("[levels] thrown mine %u (%s, vtable 0x%x): from (%.0f, %.0f, %.0f) at (%.3f, %.3f, %.3f) per ms, gravity (%.5f, %.5f, %.5f), since %u (sim clock %u)",
                                 m.handle, m.parabolic ? "parabolic" : "other", static_cast<unsigned>(m.vtable), g.p0[0], g.p0[1], g.p0[2], g.v0[0], g.v0[1], g.v0[2],
                                 g.a[0], g.a[1], g.a[2], g.t0, LogicTime());
                    g_guided.push_back(g);
                } else fm.landing = "lost";
            }
        }
        f.mines.push_back(fm);
    }
    const bool seen = !f.mines.empty();
    if (seen) std::memcpy(g_flight.from, f.mines.front().spawn, sizeof g_flight.from);
    g_following.push_back(std::move(f));
    g_fireAt = GetTickCount64();
    ++g_fireSerial;
    if (seen) {
        g_flight.vy = first ? first->vel[1] : vy;
        g_flight.vx = first ? first->vel[0] : 0.f;
        g_flight.vz = first ? first->vel[2] : 0.f;
        g_flight.gravity = kMineGravity;
        g_flight.serial = g_fireSerial.load();
        g_flight.at = g_fireAt.load();
        g_hasFlight = true;
    }
}

std::string MapVec(const float* v) {
    char b[96];
    snprintf(b, sizeof b, "[%.2f,%.2f,%.2f]", v[0] / kWorldPerMap, v[1] / kWorldPerMap, v[2] / kWorldPerMap);
    return b;
}

float WaterNow() {
    gamestate::Var v{};
    return gamestate::Var1("Water.Level", &v) ? static_cast<float>(std::atof(v.value)) : NAN;
}

// One mine of a fire: where it appeared and where it ended up (rest: its last still position, final once it is gone
// or the watch is over), or its fate.
std::string MineJson(const Fire& f, const FiredMine& m) {
    jsonmini::Obj o;
    o.UInt("handle", m.handle).Raw("spawn", MapVec(m.spawn));
    if (m.gone) {
        // the entity list moves a mine's position only once it rests, so one that goes before that (sunk in the sea, or
        // set off in the air) was last seen at its launch point
        o.Bool("gone", true).Raw("lastSeen", MapVec(m.last)).Str("fate", m.drowned ? "drowned" : m.moved ? "destroyed" : "gone before it landed")
            .UInt("goneTick", m.goneTick);
        if (!std::isnan(m.waterThen)) o.Float("waterMap", m.waterThen / kWorldPerMap);
        if (m.moved && !m.restsAfter) o.Raw("rest", MapVec(m.last));
    } else {
        o.Raw("rest", MapVec(m.last)).Bool("settled", m.moved).Str("fate", m.moved ? "resting" : "not seen to land");
    }
    o.Int("moves", m.moves).Int("movedAfterRest", m.restsAfter);
    o.Float("restFromFactoryMap", std::hypot(m.last[0] - f.factory[0], m.last[2] - f.factory[2]) / kWorldPerMap);
    if (m.thrown) {
        o.Raw("target", MapVec(m.target)).Str("landing", m.landing);
        o.Float("restFromTargetMap", std::hypot(m.last[0] - m.target[0], m.last[2] - m.target[2]) / kWorldPerMap);
    }
    return o.End();
}

std::string FireJson(const Fire& f) {
    jsonmini::Arr mines;
    for (const FiredMine& m : f.mines) mines.Raw(MineJson(f, m));
    return jsonmini::Obj().Str("stem", f.stem).UInt("tick", f.tick).UInt("turn", f.turn).Int("asked", f.asked).Bool("launch", f.launch)
        .Bool("thrown", f.thrown).Raw("factory", MapVec(f.factory)).Raw("mines", mines.End()).End();
}

// Where each mine of a logged fire ended up, one line each, in map units: its rest (or where it was last seen and its
// fate), how far it lies from the factory's axis and how far under or over the factory, and whether that is under the
// factory (within kUnderFactoryMap of its axis) or in the water. Whether it is in the crater is the level's to say:
// erg-mcp's game_mines reads these with the project's ground.
void LogWhere(const Fire& f) {
    const float water = WaterNow();
    int i = 0;
    for (const FiredMine& m : f.mines) {
        ++i;
        const float* p = m.last;
        const float across = std::hypot(p[0] - f.factory[0], p[2] - f.factory[2]) / kWorldPerMap, dy = (p[1] - f.factory[1]) / kWorldPerMap;
        const bool wet = m.drowned || (!std::isnan(water) && p[1] < water);
        char where[96];
        snprintf(where, sizeof where, "%s%s", across <= kUnderFactoryMap ? "under the factory" : "off the factory",
                 wet ? ", in the water" : "");
        LOG_INFO("[levels] %s: mine %d of %zu (%u) %s (%.2f, %.2f, %.2f) map: %.2f from the factory's axis, %.2f %s it; %s", f.stem.c_str(), i,
                 f.mines.size(), m.handle, m.gone ? (m.drowned ? "drowned, last seen at" : "gone, last seen at") : m.moved ? "rests at" : "not seen to land, at",
                 static_cast<double>(p[0] / kWorldPerMap), static_cast<double>(p[1] / kWorldPerMap), static_cast<double>(p[2] / kWorldPerMap),
                 static_cast<double>(across), static_cast<double>(std::fabs(dy)), dy < 0 ? "under" : "over", where);
    }
}

// One look at a fire's mines: a move updates the last position (a mine still for kRestMs is at rest); a mine no longer
// in the entity list is gone, drowned when its last position was under the water. Returns whether every mine is at
// rest or gone; `changed` is set when a mine of a logged fire moved again or went.
bool Watch(Fire& f, const std::vector<MineAt>& live, ULONGLONG now, uint32_t tick, bool* changed) {
    bool all = true;
    for (FiredMine& m : f.mines) {
        if (m.gone) continue;
        const MineAt* at = nullptr;
        for (const MineAt& l : live)
            if (l.handle == m.handle) at = &l;
        if (!at) {
            m.gone = m.done = true;
            m.goneTick = tick;
            m.waterThen = WaterNow();
            m.drowned = !std::isnan(m.waterThen) && m.last[1] < m.waterThen;
            *changed |= f.logged;
            continue;
        }
        if (std::hypot(at->pos[0] - m.last[0], at->pos[1] - m.last[1], at->pos[2] - m.last[2]) > 0.5f) {
            std::memcpy(m.last, at->pos, sizeof m.last);
            m.changedAt = now;
            ++m.moves;
            if (m.done && m.moved) {
                ++m.restsAfter;
                *changed |= f.logged;
            }
            m.moved = true;
            m.done = false;
        }
        if ((m.moved && now - m.changedAt >= kRestMs) || (!f.logged && now - f.at >= kFollowMs)) m.done = true;
        // a thrown mine still in flight is not done, whatever its flight's base did (the engine re-bases it in the air)
        if (m.thrown && !f.logged && Guiding(m.handle) && now - f.at < kFollowMs) m.done = false;
        all &= m.done;
    }
    return all;
}

// The sim tick: where the followed mines are now; a fire whose mines have all rested (or gone, or past kFollowMs) is
// logged, and then watched for kWatchMs more, a change logged as factory.mine.
void OnTick(uint32_t tick, void*) {
    GuideThrown(tick);
    if ((g_following.empty() && g_fired.empty()) || tick % 5) return;
    const ULONGLONG now = GetTickCount64();
    const std::vector<MineAt> live = Mines();
    for (size_t i = 0; i < g_following.size();) {
        Fire& f = g_following[i];
        bool changed = false;
        if (!Watch(f, live, now, tick, &changed)) {
            ++i;
            continue;
        }
        int rested = 0;
        for (const FiredMine& m : f.mines) rested += !m.gone && m.moved;
        jsonmini::Arr mines;
        for (const FiredMine& m : f.mines) mines.Raw(MineJson(f, m));
        jlog::Rec("levels", jlog::Level::Info, "factory.fired").Str("stem", f.stem).Uint("tick", f.tick).Uint("turn", f.turn)
            .Int("asked", f.asked).Bool("launch", f.launch).Bool("thrown", f.thrown).Raw("factory", MapVec(f.factory)).Raw("mines", mines.End());
        LOG_INFO("[levels] %s: the mine factory's fire at tick %u (turn %u): %zu mines appeared, %d came to rest", f.stem.c_str(), f.tick,
                 f.turn, f.mines.size(), rested);
        LogWhere(f);
        f.logged = true;
        g_fired.push_back(std::move(f));
        if (g_fired.size() > kKeepFires) g_fired.erase(g_fired.begin());
        g_following.erase(g_following.begin() + static_cast<std::ptrdiff_t>(i));
    }
    for (Fire& f : g_fired) {
        if (now - f.at > kWatchMs) continue;
        bool changed = false;
        Watch(f, live, now, tick, &changed);
        if (!changed) continue;
        for (const FiredMine& m : f.mines)
            if (m.gone ? m.goneTick == tick : m.changedAt == now)
                jlog::Rec("levels", jlog::Level::Info, "factory.mine").Str("stem", f.stem).Uint("fireTick", f.tick).Uint("tick", tick)
                    .Raw("mine", MineJson(f, m));
    }
}

// PlaceMines with the level's launch: the factory's mines appear over it, within launchRadius, launchHeight up, or
// erupt from its launchers and land within launchRadius of them.
void __fastcall HkPlaceMines(uintptr_t self, void*) {
    erg::FactorySpec spec;
    bool armed = false;
    std::string stem;
    {
        std::lock_guard lk(g_mx);
        armed = g.armed;
        spec = g.spec;
        stem = g.stem;
    }
    int32_t n = 0;
    float at[3] = {};
    mem::SafeRead(self + kToSpawn, &n, sizeof n);
    ReadVec(self + kPos, at);
    const std::vector<MineAt> before = Mines();
    if (!armed || !spec.Launch() || n < 1 || n > 10) {
        g_hPlace.thiscall<void>(self);
        FollowFire(stem.empty() ? "?" : stem, before, n, false, at, -0.3f);
        return;
    }
    using Random = uint32_t(__cdecl*)();
    const auto rnd = reinterpret_cast<Random>(kSimRandom);
    const float height = static_cast<float>(spec.launchHeight.value_or(20)) * kWorldPerMap;
    const float radius = static_cast<float>(spec.launchRadius.value_or(0)) * kWorldPerMap;
    const float vel[3] = {0.f, -0.3f, 0.f};
    int made = 0;
    if (spec.throwing) {
        // the throw: each mine from a launch point (the launchers in turn from a random one, else the factory), lobbed
        // at a landing point of the pattern whose ground the land sweep finds (and, with avoidWater, above the water)
        const erg::ThrowSpec& t = *spec.throwing;
        const float water = WaterNow();
        const uint32_t count = static_cast<uint32_t>(spec.launchers.size());
        const uint32_t first = count ? rnd() % count : 0;
        const float base = static_cast<float>(rnd() % 36000u) * (6.2831853f / 36000.f);
        const uint32_t firstTarget = t.targets.empty() ? 0 : rnd() % static_cast<uint32_t>(t.targets.size());
        const float minR = static_cast<float>(t.minRange) * kWorldPerMap, maxR = static_cast<float>(t.maxRange) * kWorldPerMap;
        std::vector<Throw> throws;
        int fellBack = 0, atTargets = 0;
        for (int i = 0; i < n; ++i) {
            // kThrowLift over the launch point: a mine made within its own radius (about a map unit) of the ground is
            // rested by the engine at once (the collider step's sweep starts at the sphere's bottom), floating where
            // it appeared: seen on 2026-11-09 with mines made 1 map unit over a flat block (velocity and acceleration
            // zeroed on their first tick), while 8 over it flew
            float from[3];
            if (count) {
                const auto& l = spec.launchers[(first + static_cast<uint32_t>(i)) % count];
                from[0] = static_cast<float>(l[0]), from[1] = static_cast<float>(l[1]) + kThrowLift, from[2] = static_cast<float>(l[2]);
            } else from[0] = at[0], from[1] = at[1] + kThrowLift, from[2] = at[2];
            bool aimed = false;
            Throw th{};
            std::memcpy(th.from, from, sizeof th.from);
            for (int tries = 0; tries < kTargetTries && !aimed; ++tries) {
                float x, z;
                const bool aimTarget = t.pattern == erg::ThrowPattern::Targets && !t.targets.empty() && static_cast<int>(rnd() % 100u) < t.targetShare;
                if (aimTarget) {
                    const auto& g = t.targets[(firstTarget + static_cast<uint32_t>(i)) % static_cast<uint32_t>(t.targets.size())];
                    const float a = static_cast<float>(rnd() % 36000u) * (6.2831853f / 36000.f);
                    const float r = static_cast<float>(t.scatter) * kWorldPerMap * std::sqrt(static_cast<float>(rnd() % 10000u) / 10000.f);
                    x = static_cast<float>(g[0]) + r * std::cos(a), z = static_cast<float>(g[1]) + r * std::sin(a);
                } else {
                    float a;
                    if (t.pattern == erg::ThrowPattern::Ring && n > 1) {
                        const float step = 6.2831853f / static_cast<float>(n);
                        a = base + step * static_cast<float>(i) + (static_cast<float>(rnd() % 10000u) / 10000.f - 0.5f) * step * 0.5f;
                    } else a = static_cast<float>(rnd() % 36000u) * (6.2831853f / 36000.f);
                    // even over the annulus
                    const float u = static_cast<float>(rnd() % 10000u) / 10000.f;
                    const float r = std::sqrt(minR * minR + (maxR * maxR - minR * minR) * u);
                    x = from[0] + r * std::cos(a), z = from[2] + r * std::sin(a);
                }
                const std::optional<float> ground = GroundAt(x, z, from[1]);
                if (!ground) continue;
                if (t.avoidWater && !std::isnan(water) && *ground < water + kWorldPerMap) continue;
                th.target[0] = x, th.target[1] = *ground, th.target[2] = z;
                aimed = true;
                atTargets += aimTarget;
            }
            if (!aimed) {
                // no landing point found (the sea all round, or no land sweep): straight up, as the launchers' eruption
                ++fellBack;
                th.target[0] = from[0], th.target[1] = from[1] - kWorldPerMap, th.target[2] = from[2];
                th.vel[0] = th.vel[2] = 0.f;
                th.vel[1] = std::sqrt(2.f * kMineGravity * (std::max)(height, kWorldPerMap));
            } else ThrowVelocity(from, th.target, static_cast<float>(t.apex) * kWorldPerMap, th.vel);
            if (!EngineMine(from, th.vel)) {
                ++made;
                throws.push_back(th);
            }
        }
        LOG_INFO("[levels] %s: the mine factory threw %d of %d mines from %s at landing points %d-%d map units out (%s%s, apex %d), %d at the targets, %d straight up (no landing point found)",
                 stem.c_str(), made, n, count ? "its launchers" : "the factory", t.minRange, t.maxRange, erg::ThrowPatternName(t.pattern),
                 t.avoidWater ? ", clear of the water" : "", t.apex, atTargets, fellBack);
        FollowFire(stem, before, n, true, at, throws.empty() ? 0.f : throws.front().vel[1], &throws);
        return;
    }
    if (!spec.launchers.empty()) {
        const uint32_t count = static_cast<uint32_t>(spec.launchers.size());
        const uint32_t first = rnd() % count;
        const float up = std::sqrt(2.f * kMineGravity * height);   // tops out launchHeight over where it starts
        std::vector<std::array<float, 2>> spots;
        for (int i = 0; i < n; ++i) {
            const auto& l = spec.launchers[(first + static_cast<uint32_t>(i)) % count];
            // a spot within launchRadius of the launcher, the farthest of kSpotTries from the fire's other spots
            float best[2] = {static_cast<float>(l[0]), static_cast<float>(l[2])}, bestGap = -1;
            for (int t = 0; t < kSpotTries; ++t) {
                const float a = static_cast<float>(rnd() % 36000u) * (6.2831853f / 36000.f);
                const float r = radius * std::sqrt(static_cast<float>(rnd() % 10000u) / 10000.f);
                const float p[2] = {static_cast<float>(l[0]) + r * std::cos(a), static_cast<float>(l[2]) + r * std::sin(a)};
                float gap = 1e9f;
                for (const auto& q : spots) gap = (std::min)(gap, std::hypot(p[0] - q[0], p[1] - q[1]));
                if (gap > bestGap) best[0] = p[0], best[1] = p[1], bestGap = gap;
                if (gap >= kMinApart) break;
            }
            spots.push_back({best[0], best[1]});
            const float from[3] = {best[0], static_cast<float>(l[1]) + kWorldPerMap, best[1]};
            const float v[3] = {0.f, up, 0.f};
            if (!EngineMine(from, v)) ++made;
        }
        LOG_INFO("[levels] %s: the mine factory threw %d of %d mines from %zu launchers, %.0f up, within %.0f of each (world units)",
                 stem.c_str(), made, n, spec.launchers.size(), static_cast<double>(height), static_cast<double>(radius));
        FollowFire(stem, before, n, true, at, up);
        return;
    }
    for (int i = 0; i < n; ++i) {
        const float a = static_cast<float>(rnd() % 36000u) * (6.2831853f / 36000.f);
        const float r = radius * std::sqrt(static_cast<float>(rnd() % 10000u) / 10000.f);
        const float pos[3] = {at[0] + r * std::cos(a), at[1] + height, at[2] + r * std::sin(a)};
        if (!EngineMine(pos, vel)) ++made;
    }
    LOG_INFO("[levels] %s: the mine factory fired %d of %d mines from %.0f over it, within %.0f (world units)", stem.c_str(), made, n,
             static_cast<double>(height), static_cast<double>(radius));
    FollowFire(stem, before, n, true, at, vel[1]);
}

// The match's one MineFactoryLogicEntity, or 0.
uintptr_t FindFactory() {
    std::vector<gamestate::Entity> ents(512);
    int n = gamestate::Entities(ents.data(), static_cast<int>(ents.size()));
    if (n > static_cast<int>(ents.size())) {
        ents.resize(static_cast<size_t>((std::min)(n, 4096)));
        n = gamestate::Entities(ents.data(), static_cast<int>(ents.size()));
    }
    for (int i = 0; i < (std::min)(n, static_cast<int>(ents.size())); ++i)
        if (std::strcmp(ents[static_cast<size_t>(i)].type, "MineFactoryLogicEntity") == 0) return ents[static_cast<size_t>(i)].object;
    return 0;
}

// wum.level.mine: a mine at pos (world units) with velocity vel, made as the factory makes its own.
const char* EngineMine(const float pos[3], const float vel[3]) {
    uintptr_t svc = 0;
    if (!mem::SafeRead(kLogicService, &svc, sizeof svc) || !svc) return "no game logic service";
    using CreateMine = void(__thiscall*)(uintptr_t, const float*, const float*, const float*);
    reinterpret_cast<CreateMine>(kCreateMine)(svc, pos, pos, vel);
    return nullptr;
}

// wum.level.fireFactory: the idle countdown to 0, so the next GameLogic.StartMineFactory fires when fewer mines than
// the target are in the level.
const char* EngineArmFactory() {
    const uintptr_t f = FindFactory();
    if (!f) return "the level has no mine factory (or it is gone)";
    uint8_t exploded = 0;
    int32_t fire = 0;
    if (!mem::SafeRead(f + kExploded, &exploded, 1) || !mem::SafeRead(f + kFireState, &fire, 4)) return "the factory is unreadable";
    if (exploded) return "the factory has exploded";
    if (fire != 0) return "the factory is firing";
    const int32_t zero = 0;
    return mem::Write(f + kCountdown, &zero, sizeof zero) ? nullptr : "the factory could not be written";
}

// The level's mines as the factory counts them (GameLogicService +0x1d8), or -1.
int LevelMines() {
    uintptr_t svc = 0, b = 0, e = 0;
    if (!mem::SafeRead(kLogicService, &svc, sizeof svc) || !svc || !mem::SafeRead(svc + kMinesBegin, &b, 4) ||
        !mem::SafeRead(svc + kMinesEnd, &e, 4) || e < b)
        return -1;
    return static_cast<int>((e - b) / 4);
}

class LevelFactory final : public Module {
public:
    const char* Name() const override { return "LevelFactory"; }
    const char* Description() const override { return "the level's mine factory settings, drop and wum.level.mine"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 55; }   // after SimTweak (53)
    bool Install() override {
        using mem::Expect;
        // Drop: sub esp,0x18; fldz; push esi; fst [esp+0x10]; push edi. CreateMine: push -1; push 0x7cf488; mov eax,fs:[0].
        // StartMineFactory: sub esp,0xc; push esi; mov esi,ecx; cmp dword [esi+0x8c],0 (the fire state).
        if (!Expect(kDrop, {0x83, 0xec, 0x18, 0xd9, 0xee, 0x56, 0xd9, 0x54, 0x24, 0x10, 0x57}) ||
            !Expect(kCreateMine, {0x6a, 0xff, 0x68, 0x88, 0xf4, 0x7c, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00}) ||
            !Expect(0x5cfe40, {0x83, 0xec, 0x0c, 0x56, 0x8b, 0xf1, 0x83, 0xbe, 0x8c, 0x00, 0x00, 0x00, 0x00}) ||
            !Expect(0x5cf76a, {0x81, 0x3d, 0xfc, 0x2c, 0x95, 0x00, 0xe8, 0x03, 0x00, 0x00})) {
            LOG_WARN("[levels] the mine factory's code differs from build #1077: LevelFactory stays inert");
            return true;
        }
        g_hDrop = safetyhook::create_inline(kDrop, &HkDrop);
        if (!g_hDrop) {
            LOG_ERROR("[levels] the mine factory hook failed: level factory settings apply at level start only");
            return true;
        }
        // PlaceMines: push ebp; mov ebp,esp; and esp,-8; push -1; push 0x7d81a8. The sim's random: push ebp; mov ebp,esp;
        // mov ecx,0x96d034. Without them the launch settings are skipped and the game places its mines.
        if (Expect(kPlaceMines, {0x55, 0x8b, 0xec, 0x83, 0xe4, 0xf8, 0x6a, 0xff, 0x68, 0xa8, 0x81, 0x7d, 0x00}) &&
            Expect(kSimRandom, {0x55, 0x8b, 0xec, 0xb9, 0x34, 0xd0, 0x96, 0x00})) {
            g_hPlace = safetyhook::create_inline(kPlaceMines, &HkPlaceMines);
            if (!g_hPlace) LOG_WARN("[levels] the mine factory's PlaceMines hook failed: launchHeight and launchRadius are skipped");
        } else LOG_WARN("[levels] the mine factory's PlaceMines differs from build #1077: launchHeight and launchRadius are skipped");
        // ParabolicPayloadLogicEntity's come-to-rest: push -1; mov eax, fs:[0]; push 0x7c9786 (its handler frame)
        g_restOk = Expect(kParaRest, {0x6a, 0xff, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x68, 0x86, 0x97, 0x7c, 0x00});
        if (!g_restOk) LOG_WARN("[levels] the parabolic payload's come-to-rest differs from build #1077: a thrown mine is stopped by hand");
        g_tickHook = sim::AddTickHook(&OnTick, nullptr, 70);   // follows the fires to where their mines rest, lands the thrown ones
        simcore::LevelEngine e;
        e.mine = &EngineMine;
        e.armFactory = &EngineArmFactory;
        simcore::SetLevelEngine(e);
        return true;
    }
    void Uninstall() override {
        if (g_tickHook) sim::RemoveTickHook(g_tickHook);
        g_tickHook = 0;
        g_guided.clear();
        simcore::SetLevelEngine(simcore::LevelEngine{});
        g_hPlace = {};
        g_hDrop = {};
    }
};
}  // namespace

void Arm(const std::string& stem, const erg::FactorySpec& f) {
    std::lock_guard lk(g_mx);
    g.armed = true;
    g.stem = stem;
    g.spec = f;
}

void Disarm() {
    std::lock_guard lk(g_mx);
    g.armed = false;
}

std::string ReportJson() {
    State s;
    {
        std::lock_guard lk(g_mx);
        s = g;
    }
    if (!s.reported) return "null";
    auto field = [](const Field& f) {
        if (!f.asked) return std::string("null");
        jsonmini::Obj o;
        o.Int("asked", f.value).Str("write", f.result);
        if (f.found) o.Float("found", *f.found);
        return o.End();
    };
    auto vec = [](const float* v) {
        return jsonmini::Arr().Raw(std::to_string(v[0])).Raw(std::to_string(v[1])).Raw(std::to_string(v[2])).End();
    };
    return jsonmini::Obj().Str("stem", s.reportStem).Raw("activation", field(s.activation)).Raw("inactiveTurns", field(s.inactive))
        .Raw("knot", vec(s.knot)).Raw("settled", vec(s.settled)).Bool("landed", s.hit).Bool("keptKnot", s.keptKnot).End();
}

const char* FireNow(int count, std::string* detail) {
    static char why[160];
    const char* result = nullptr;
    const uintptr_t f = g_hDrop ? FindFactory() : 0;
    uint8_t exploded = 0;
    int32_t fire = 0, countdown = 0;
    const int mines = LevelMines();
    float target = -1;
    tweak::Get("kMineFactoryData", "NumMineActivation", &target);
    if (!g_hDrop) result = "the mine factory hooks are not installed (another game build)";
    else if (!f) result = "the level has no mine factory (or it is gone)";
    else if (!mem::SafeRead(f + kExploded, &exploded, 1) || !mem::SafeRead(f + kFireState, &fire, 4)) result = "the factory is unreadable";
    else if (exploded) result = "the factory has exploded";
    else if (fire != 0) result = "the factory is firing";
    else if (mines < 0) result = "the level's mines could not be counted";
    else if (count <= 0 && target >= 0 && mines >= static_cast<int>(target)) {
        snprintf(why, sizeof why, "the level holds %d mines, not fewer than the factory's target %g: it fires nothing (give a count)", mines,
                 static_cast<double>(target));
        result = why;
    } else {
        // the target for this call alone: the mines there plus count, so StartMineFactory fires count of them
        const bool raise = count > 0 && target >= 0;
        if (raise) tweak::Set("kMineFactoryData", "NumMineActivation", static_cast<float>((std::min)(255, mines + (std::min)(count, 10))));
        const int32_t zero = 0;
        mem::Write(f + kCountdown, &zero, sizeof zero);
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(kStartFactory)(f);
        if (raise) tweak::Set("kMineFactoryData", "NumMineActivation", target);
        mem::SafeRead(f + kFireState, &fire, 4);
        if (fire == 0) result = "the factory's StartMineFactory did not start a fire";
    }
    int32_t toFire = 0;
    if (f) {
        mem::SafeRead(f + kToSpawn, &toFire, 4);
        mem::SafeRead(f + kCountdown, &countdown, 4);
    }
    char d[200];
    snprintf(d, sizeof d, "mines in the level %d, target %g, fire state %d, mines to fire %d, idle countdown %d", mines,
             static_cast<double>(target), fire, toFire, countdown);
    if (detail) *detail = d;
    g_trigger = jsonmini::Obj().UInt("tick", sim::Tick()).Int("count", count).Bool("started", !result).Str("why", result ? result : "")
                    .Str("detail", d).End();
    LOG_INFO("[levels] the mine factory on demand: %s (%s)", result ? result : "a fire started", d);
    return result;
}

const char* Mine(const float pos[3], const float vel[3]) { return g_hDrop ? EngineMine(pos, vel) : "the mine factory hooks are not installed (another game build)"; }

bool Position(float out[3]) {
    const uintptr_t f = g_hDrop ? FindFactory() : 0;
    return f && ReadVec(f + kPos, out);
}

bool LastFlight(Flight* out) {
    if (!g_hasFlight) return false;
    *out = g_flight;
    return true;
}

uint32_t FireSerial(ULONGLONG* at) {
    if (at) *at = g_fireAt.load();
    return g_fireSerial.load();
}

std::string FiresJson() {
    jsonmini::Arr fires;
    for (const Fire& f : g_fired) fires.Raw(FireJson(f));
    return jsonmini::Obj().Raw("fires", fires.End()).Int("following", static_cast<int>(g_following.size())).Raw("trigger", g_trigger).End();
}

MELANGE_MODULE(LevelFactory);
}  // namespace melange::levels::factory
