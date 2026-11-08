// The Test probe (levels/probe.h). While a Test the game started plays: at the first GameLogic.Turn.Started every
// worm's position, health and physics state; then every ExplosionMessage (posted or delivered) with its epicentre,
// damage and radius, the worms inside its worm-damage radius, and its likely source: the nearest mine, oil drum, crate
// or projectile of the entity list (cached every few ticks) within 2 map units of the epicentre, or the mine factory
// when it is nearer (it blows up in the water), a dying worm's own blast (in its column), else the weapon of the worm
// whose turn it is (seen in Tests: the factory's blast at its knot when the sea rose over it, the 35-damage blasts of
// drowned worms, crates and drums going off in the water); every worm's falls (the drop from the highest point of a
// flight or slide to where it stands again) and health changes; and when a worm stops being alive (health 0, dying or
// drowning: sim_worms Alive), a death record with the cause: drowned when it is in the engine's DrownFloat state or
// under the water, else the latest of its hits and its falls of more than kFallReport. The cause is inferred from what the probe saw, not read from the engine (the game has
// no damage source): `evidence` lists what it was inferred from. Read-only: the worm containers, the bus and the entity
// list are read, nothing is written, so the match is the same with or without it.
#include "levels/probe.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/module.h"
#include "levels/factory.h"
#include "lua/sim/sim_worms.h"
#include "melange/bus.h"
#include "melange/gamestate.h"
#include "melange/jlog.h"
#include "melange/levels.h"
#include "melange/sim.h"
#include "tools/json_mini.h"
#include "weapons/engine.h"

namespace melange::levels::probe {
namespace {
namespace sw = simworms;
constexpr uintptr_t kExplosionVt = 0x854184;   // ExplosionMessage (core/bus_decoders.cpp)
constexpr float kWorldPerMap = 20.f;
constexpr float kSourceNear = 2.f * kWorldPerMap;   // an entity this close to an epicentre is its source
constexpr float kWormReach = 0.75f * kWorldPerMap;  // a worm's half-width beyond the damage radius
constexpr float kWormBlast = 1.f * kWorldPerMap;    // a dying worm's own blast goes off this close to it
constexpr float kFallReport = 1.f * kWorldPerMap;   // falls shorter than this are not kept
constexpr uint32_t kCacheEvery = 4;                  // ticks between entity-list reads
constexpr size_t kMaxExplosions = 64, kMaxHits = 6, kMaxFalls = 4, kMaxHealth = 12, kMaxDeaths = 32;

struct Near {   // an entity of the cached list
    uint32_t handle = 0;
    char kind[12] = {}, type[48] = {}, label[32] = {};
    float pos[3] = {};
};
struct Explosion {
    uint32_t tick = 0, turn = 0;
    float at[3] = {}, damage = 0, radius = 0, land = 0;
    std::string source;          // "mine", "oildrum", "weapon", "explosion"
    Near entity;                 // the source entity (handle 0: none found)
    int byWorm = -1;             // the slot whose turn it was
    std::string weapon;          // that worm's selected weapon (kNames)
};
struct Hit {
    size_t explosion;            // index into g.explosions
    float distance;
};
struct Fall {
    uint32_t tick = 0;
    float from = 0, to = 0;
};
struct HealthStep {
    uint32_t tick;
    int from, to;
};
struct Worm {
    bool seen = false, alive = false, dead = false, flying = false;
    uint32_t state = 0;
    int health = 0;
    float pos[3] = {}, peak = 0;
    std::vector<Hit> hits;
    std::vector<Fall> falls;
    std::vector<HealthStep> steps;
};
struct State {
    std::string key;
    bool playing = false;
    uint32_t turn = 0, lastTick = 0;
    std::string spawns = "[]";
    std::vector<std::string> deaths;
    std::vector<Explosion> explosions;
    size_t explosionsSeen = 0;
    Worm worms[sw::kMaxWorms];
    std::vector<Near> cache;
    uint32_t cachedAt = 0;
};
std::mutex g_mx;
State g;
int g_tick = 0;
bus::SubId g_subPost = 0, g_subDeliver = 0, g_subTurn = 0;

std::string Vec(const float v[3]) {
    char b[96];
    snprintf(b, sizeof b, "[%.1f,%.1f,%.1f]", v[0], v[1], v[2]);
    return b;
}
std::string MapVec(const float v[3]) {
    char b[96];
    snprintf(b, sizeof b, "[%.2f,%.2f,%.2f]", v[0] / kWorldPerMap, v[1] / kWorldPerMap, v[2] / kWorldPerMap);
    return b;
}
float Dist(const float a[3], const float b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

bool ContainsNoCase(const char* s, const char* what) {
    const size_t n = std::strlen(what);
    for (; *s; ++s)
        if (_strnicmp(s, what, n) == 0) return true;
    return false;
}

float WaterLevel() {
    gamestate::Var v{};
    return gamestate::Var1("Water.Level", &v) ? static_cast<float>(std::atof(v.value)) : NAN;
}

// The entity list's mines, oil drums, crates and projectiles (main thread, in a match).
void RefreshCache(uint32_t tick) {
    static std::vector<gamestate::Entity> ents(1024);
    int n = gamestate::Entities(ents.data(), static_cast<int>(ents.size()));
    n = (std::min)(n, static_cast<int>(ents.size()));
    std::vector<Near> out;
    for (int i = 0; i < n; ++i) {
        const gamestate::Entity& e = ents[static_cast<size_t>(i)];
        if (!e.hasPos) continue;
        const char* kind = nullptr;
        if (e.kind == gamestate::EntityKind::Barrel) kind = "oildrum";
        else if (e.kind == gamestate::EntityKind::Crate) kind = "crate";
        else if (e.kind == gamestate::EntityKind::Projectile) {
            char both[96];
            snprintf(both, sizeof both, "%s %s", e.type, e.label);
            kind = ContainsNoCase(both, "mine") && !ContainsNoCase(both, "factory") ? "mine" : "projectile";
        }
        if (!kind) continue;
        Near x;
        x.handle = e.handle;
        snprintf(x.kind, sizeof x.kind, "%s", kind);
        snprintf(x.type, sizeof x.type, "%s", e.type);
        snprintf(x.label, sizeof x.label, "%s", e.label);
        x.pos[0] = e.pos.x, x.pos[1] = e.pos.y, x.pos[2] = e.pos.z;
        out.push_back(x);
    }
    g.cache.swap(out);
    g.cachedAt = tick;
}

// Main thread: an explosion message, posted or delivered (the copies of one post are skipped).
void OnMessage(const bus::MessageView& m, void*) {
    if (m.vtable != kExplosionVt || (m.path == bus::Path::Deliver && m.fromPost)) return;
    float at[3], damage = 0, radius = 0, land = 0;
    if (!m.Read(0x08, at, sizeof at)) return;
    m.Get(0x2c, damage);
    m.Get(0x34, radius);
    m.Get(0x38, land);
    std::lock_guard lk(g_mx);
    if (!g.playing) return;
    const uint32_t tick = sim::Tick();
    for (const Explosion& x : g.explosions)   // one explosion seen both posted and delivered
        if (x.tick == tick && Dist(x.at, at) < 0.5f) return;
    ++g.explosionsSeen;
    Explosion x;
    x.tick = tick;
    x.turn = g.turn;
    std::memcpy(x.at, at, sizeof at);
    x.damage = damage, x.radius = radius, x.land = land;
    float best = kSourceNear;
    for (const Near& n : g.cache) {
        const float d = Dist(n.pos, at);
        if (d <= best) best = d, x.entity = n;
    }
    // the mine factory, when it is nearer than any of them (it blows up in the water, its own mines lying round it)
    float fac[3];
    if (factory::Position(fac) && Dist(fac, at) <= best) {
        best = Dist(fac, at);
        x.entity = Near{};
        x.entity.handle = 0x20000u;
        snprintf(x.entity.kind, sizeof x.entity.kind, "minefactory");
        snprintf(x.entity.type, sizeof x.entity.type, "MineFactoryLogicEntity");
        std::memcpy(x.entity.pos, fac, sizeof x.entity.pos);
    }
    // a dying worm's own blast (worms blow up once they have died; a drowned one has sunk a little by then): a worm that
    // is not alive straight under or over the epicentre goes first
    for (int s = 0; s < sw::kMaxWorms && sw::GetBackend(); ++s) {
        sw::WormInfo wi{};
        const bool read = sw::GetBackend()->Read(s, &wi) && wi.active;
        if (read ? sw::Alive(wi) : !g.worms[s].dead) continue;
        const float* p = read ? wi.pos : g.worms[s].pos;
        if (std::hypot(p[0] - at[0], p[2] - at[2]) > kWormBlast || std::fabs(p[1] - at[1]) > 3 * kWorldPerMap) continue;
        x.entity = Near{};
        x.entity.handle = 0x10000u | static_cast<uint32_t>(s);
        snprintf(x.entity.kind, sizeof x.entity.kind, "worm");
        snprintf(x.entity.type, sizeof x.entity.type, "WXWormLogicEntity");
        snprintf(x.entity.label, sizeof x.entity.label, "%s", read ? wi.name : "");
        std::memcpy(x.entity.pos, p, sizeof x.entity.pos);
        break;
    }
    gamestate::Snapshot snap;
    if (gamestate::Read(&snap) && snap.match.inMatch && snap.match.activeWorm >= 0) {
        x.byWorm = snap.match.activeWorm;
        sw::WormInfo w{};
        if (sw::GetBackend() && sw::GetBackend()->Read(x.byWorm, &w) && w.weapon >= 0)
            if (const char* name = weapons::engine::EnumName(w.weapon)) x.weapon = name;
    }
    x.source = !x.entity.handle ? (x.byWorm >= 0 ? "weapon" : "explosion") : std::strcmp(x.entity.kind, "projectile") == 0 ? "weapon" : x.entity.kind;
    if (g.explosions.size() >= kMaxExplosions) g.explosions.erase(g.explosions.begin());
    g.explosions.push_back(x);
    const size_t idx = g.explosions.size() - 1;
    if (damage <= 0 || !sw::GetBackend()) return;
    for (int s = 0; s < sw::kMaxWorms; ++s) {
        Worm& w = g.worms[s];
        sw::WormInfo wi{};
        if (!w.seen || w.dead || !sw::GetBackend()->Read(s, &wi) || !sw::Alive(wi)) continue;
        const float d = Dist(wi.pos, at);
        if (d > radius + kWormReach) continue;
        if (w.hits.size() >= kMaxHits) w.hits.erase(w.hits.begin());
        w.hits.push_back({idx, d});
    }
}

void OnTurn(const bus::MessageView&, void*) {
    std::lock_guard lk(g_mx);
    if (g.playing) ++g.turn;
}

std::string ExplosionJson(const Explosion& x) {
    jsonmini::Obj o;
    o.UInt("tick", x.tick).UInt("turn", x.turn).Raw("at", MapVec(x.at)).Raw("atWorld", Vec(x.at)).Float("wormDamage", x.damage)
        .Float("wormDamageRadiusMap", x.radius / kWorldPerMap).Str("source", x.source);
    if (x.entity.handle)
        o.Raw("entity", jsonmini::Obj().UInt("handle", x.entity.handle).Str("kind", x.entity.kind).Str("type", x.entity.type)
                            .Str("label", x.entity.label).Raw("at", MapVec(x.entity.pos)).End());
    if (x.byWorm >= 0) o.Int("turnWorm", x.byWorm);
    if (!x.weapon.empty()) o.Str("turnWeapon", x.weapon);
    return o.End();
}

std::string WormJson(int slot, const sw::WormInfo& w) {
    return jsonmini::Obj().Int("slot", slot).Int("team", w.team).Int("posInTeam", w.posInTeam).Str("name", w.name)
        .Raw("pos", MapVec(w.pos)).Raw("posWorld", Vec(w.pos)).Int("health", w.health).UInt("state", w.state).End();
}

// A worm stopped being alive: the death record (under g_mx).
void Died(int slot, Worm& w, const sw::WormInfo& now, uint32_t tick) {
    w.dead = true;
    const float water = WaterLevel();
    const bool drowning = now.state == sw::kDrownFloat || (!std::isnan(water) && now.pos[1] < water);
    const Hit* hit = w.hits.empty() ? nullptr : &w.hits.back();
    const Explosion* ex = hit && hit->explosion < g.explosions.size() ? &g.explosions[hit->explosion] : nullptr;
    const Fall* fall = w.falls.empty() ? nullptr : &w.falls.back();
    std::string cause = "unknown";
    jsonmini::Arr evidence;
    if (drowning) {
        cause = "drown";
        evidence.Str(now.state == sw::kDrownFloat ? "the engine's DrownFloat state" : "its feet under the water");
    } else if (ex && (!fall || fall->tick <= ex->tick)) {
        cause = ex->source;
        evidence.Str("its latest hit: inside an explosion's worm-damage radius");
    } else if (fall) {
        cause = "fall";
        evidence.Str("its latest fall, after any explosion that hit it");
    } else {
        evidence.Str("no hit, fall or water seen: poison, sudden death's 1 health, or damage the probe does not see");
    }
    jsonmini::Arr hits, falls, health;
    for (const Hit& h : w.hits)
        if (h.explosion < g.explosions.size())
            hits.Raw(jsonmini::Obj().Raw("explosion", ExplosionJson(g.explosions[h.explosion])).Float("distanceMap", h.distance / kWorldPerMap).End());
    for (const Fall& f : w.falls)
        falls.Raw(jsonmini::Obj().UInt("tick", f.tick).Float("fromY", f.from / kWorldPerMap).Float("toY", f.to / kWorldPerMap)
                      .Float("dropMap", (f.from - f.to) / kWorldPerMap).End());
    for (const HealthStep& h : w.steps) health.Raw(jsonmini::Obj().UInt("tick", h.tick).Int("from", h.from).Int("to", h.to).End());
    jsonmini::Obj o;
    o.Int("slot", slot).Int("team", now.team).Int("posInTeam", now.posInTeam).Str("name", now.name).UInt("tick", tick).UInt("turn", g.turn)
        .Raw("pos", MapVec(now.pos)).Raw("posWorld", Vec(now.pos)).Raw("lastAlivePos", MapVec(w.pos)).Int("health", now.health)
        .UInt("state", now.state).Str("cause", cause).Raw("evidence", evidence.End());
    if (!std::isnan(water)) o.Float("waterWorld", water).Float("waterMap", water / kWorldPerMap);
    if (ex && cause != "drown" && cause != "fall") o.Raw("source", ExplosionJson(*ex));
    o.Raw("hits", hits.End()).Raw("falls", falls.End()).Raw("healthSteps", health.End());
    std::string rec = o.End();
    if (g.deaths.size() < kMaxDeaths) g.deaths.push_back(rec);
    jlog::Rec("levels", jlog::Level::Info, "test.death").Str("key", g.key).Raw("death", rec);
    LOG_INFO("[levels] test probe: %s (slot %d) died at turn %u, tick %u: %s at (%.1f, %.1f, %.1f) map", now.name, slot, g.turn, tick,
             cause.c_str(), now.pos[0] / kWorldPerMap, now.pos[1] / kWorldPerMap, now.pos[2] / kWorldPerMap);
}

void OnTick(uint32_t tick, void*) {
    std::lock_guard lk(g_mx);
    if (!g.playing || !sw::GetBackend()) return;
    g.lastTick = tick;
    if (tick - g.cachedAt >= kCacheEvery || tick < g.cachedAt) RefreshCache(tick);
    const bool first = g.turn >= 1 && g.spawns == "[]";
    jsonmini::Arr spawns;
    for (int s = 0; s < sw::kMaxWorms; ++s) {
        sw::WormInfo wi{};
        if (!sw::GetBackend()->Read(s, &wi) || !wi.active) continue;
        Worm& w = g.worms[s];
        const bool alive = sw::Alive(wi);
        if (!w.seen) {
            w.seen = true;
            w.alive = alive;
            w.health = wi.health;
        }
        if (first && alive) spawns.Raw(WormJson(s, wi));
        if (w.dead) continue;
        if (static_cast<int>(wi.health) != w.health) {
            if (w.steps.size() >= kMaxHealth) w.steps.erase(w.steps.begin());
            w.steps.push_back({tick, w.health, static_cast<int>(wi.health)});
        }
        // a flight or a slide: its highest point, and the drop when it ends
        const bool flying = wi.state == sw::kBallistic || wi.state == sw::kSliding;
        if (flying && !w.flying) w.peak = wi.pos[1];
        if (flying) w.peak = (std::max)(w.peak, wi.pos[1]);
        if (!flying && w.flying && w.peak - wi.pos[1] >= kFallReport) {
            if (w.falls.size() >= kMaxFalls) w.falls.erase(w.falls.begin());
            w.falls.push_back({tick, w.peak, wi.pos[1]});
        }
        w.flying = flying;
        w.health = wi.health;
        if (w.alive && !alive) Died(s, w, wi, tick);
        else std::memcpy(w.pos, wi.pos, sizeof w.pos);
        w.alive = alive;
    }
    if (first) {
        g.spawns = spawns.End();
        jlog::Rec("levels", jlog::Level::Info, "test.spawns").Str("key", g.key).Uint("tick", tick).Raw("worms", g.spawns);
        LOG_INFO("[levels] test probe: %s: the worms' positions at turn 1 are logged (levels/test.spawns)", g.key.c_str());
    }
}

void Subscribe(bool on) {
    if (on && !g_subPost && bus::Installed()) {
        g_subPost = bus::SubscribeAll(bus::Path::Post, &OnMessage);
        g_subDeliver = bus::SubscribeAll(bus::Path::Deliver, &OnMessage);
        g_subTurn = bus::SubscribeName("GameLogic.Turn.Started", bus::Path::Post, &OnTurn);
    } else if (!on && g_subPost) {
        bus::Unsubscribe(g_subPost);
        bus::Unsubscribe(g_subDeliver);
        bus::Unsubscribe(g_subTurn);
        g_subPost = g_subDeliver = g_subTurn = 0;
    }
}

// Main thread.
void OnState(TestState s, const char* key, const char*, void*) {
    if (s == TestState::Playing && key && *key) {
        // the match's turns so far: the Test plays from the intro of turn 1, after its GameLogic.Turn.Started
        gamestate::Snapshot snap;
        const uint32_t turns = gamestate::Read(&snap) && snap.match.inMatch ? snap.match.turnsStarted : 0;
        {
            std::lock_guard lk(g_mx);
            g = State{};
            g.key = key;
            g.playing = true;
            g.turn = turns;
        }
        Subscribe(true);
        LOG_INFO("[levels] test probe: watching %s (worm spawns and deaths)", key);
    } else if (s == TestState::Ended || s == TestState::Failed || s == TestState::Idle) {
        Subscribe(false);
        std::lock_guard lk(g_mx);
        g.playing = false;
    }
}

class LevelTestProbe final : public Module {
public:
    const char* Name() const override { return "LevelTestProbe"; }
    const char* Description() const override { return "the Test match's worm spawns and deaths with their causes (level.test.probe)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 57; }
    bool Install() override {
        OnTestState(&OnState, nullptr);
        g_tick = sim::AddTickHook(&OnTick, nullptr, 60);
        if (!g_tick) LOG_WARN("[levels] the Test probe has no sim tick hook: no spawns or deaths are recorded");
        return true;
    }
    void Uninstall() override {
        Subscribe(false);
        if (g_tick) sim::RemoveTickHook(g_tick);
        g_tick = 0;
    }
};
}  // namespace

std::string ReportJson() {
    std::lock_guard lk(g_mx);
    jsonmini::Arr deaths, ex;
    for (const std::string& d : g.deaths) deaths.Raw(d);
    for (const Explosion& x : g.explosions) ex.Raw(ExplosionJson(x));
    return jsonmini::Obj().Str("key", g.key).Bool("playing", g.playing).UInt("turn", g.turn).UInt("tick", g.lastTick)
        .Raw("spawns", g.spawns).Raw("deaths", deaths.End()).UInt("explosionsSeen", g.explosionsSeen).Raw("explosions", ex.End())
        .End();
}

MELANGE_MODULE(LevelTestProbe);
}  // namespace melange::levels::probe
