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
/** Within this of the factory's axis (map units, horizontally) a mine at rest is "under the factory". */
constexpr float kUnderFactoryMap = 2.5f;

bool ReadVec(uintptr_t at, float out[3]) { return mem::SafeRead(at, out, sizeof(float) * 3); }

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
        out.push_back({e.handle, {e.pos.x, e.pos.y, e.pos.z}});
    }
    return out;
}

// After a PlaceMines: the mines that were not there before it are this fire's.
void FollowFire(const std::string& stem, const std::vector<MineAt>& before, int asked, bool launch, const float at[3], float vy) {
    Fire f;
    f.stem = stem;
    f.tick = sim::Tick();
    gamestate::Snapshot snap;
    if (gamestate::Read(&snap) && snap.match.inMatch) f.turn = snap.match.turnsStarted;
    f.asked = asked;
    f.launch = launch;
    std::memcpy(f.factory, at, sizeof f.factory);
    f.at = GetTickCount64();
    for (const MineAt& m : Mines()) {
        bool old = false;
        for (const MineAt& b : before) old |= b.handle == m.handle;
        if (old) continue;
        FiredMine fm;
        fm.handle = m.handle;
        std::memcpy(fm.spawn, m.pos, sizeof fm.spawn);
        std::memcpy(fm.last, m.pos, sizeof fm.last);
        fm.changedAt = f.at;
        f.mines.push_back(fm);
    }
    const bool seen = !f.mines.empty();
    if (seen) std::memcpy(g_flight.from, f.mines.front().spawn, sizeof g_flight.from);
    g_following.push_back(std::move(f));
    g_fireAt = GetTickCount64();
    ++g_fireSerial;
    if (seen) {
        g_flight.vy = vy;
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
    return o.End();
}

std::string FireJson(const Fire& f) {
    jsonmini::Arr mines;
    for (const FiredMine& m : f.mines) mines.Raw(MineJson(f, m));
    return jsonmini::Obj().Str("stem", f.stem).UInt("tick", f.tick).UInt("turn", f.turn).Int("asked", f.asked).Bool("launch", f.launch)
        .Raw("factory", MapVec(f.factory)).Raw("mines", mines.End()).End();
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
        all &= m.done;
    }
    return all;
}

// The sim tick: where the followed mines are now; a fire whose mines have all rested (or gone, or past kFollowMs) is
// logged, and then watched for kWatchMs more, a change logged as factory.mine.
void OnTick(uint32_t tick, void*) {
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
            .Int("asked", f.asked).Bool("launch", f.launch).Raw("factory", MapVec(f.factory)).Raw("mines", mines.End());
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
        g_tickHook = sim::AddTickHook(&OnTick, nullptr, 70);   // follows the fires to where their mines rest
        simcore::LevelEngine e;
        e.mine = &EngineMine;
        e.armFactory = &EngineArmFactory;
        simcore::SetLevelEngine(e);
        return true;
    }
    void Uninstall() override {
        if (g_tickHook) sim::RemoveTickHook(g_tickHook);
        g_tickHook = 0;
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
