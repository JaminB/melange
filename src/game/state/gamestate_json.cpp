#include "game/state/gamestate_json.h"

#include <cmath>
#include <cstdio>
#include <iterator>

#include "tools/json_mini.h"

namespace melange::gamestate::wire {
namespace {
constexpr const char* kKinds[] = {"Worm", "Projectile", "Crate", "Barrel", "Other"};
constexpr const char* kTypes[] = {"Int", "Uint", "Float", "Vector", "String", "Container", "StringTable", "Color", "Undefined"};

std::string Num(float v) {
    if (!std::isfinite(v)) return "null";
    char b[32];
    snprintf(b, sizeof b, "%.9g", static_cast<double>(v));
    return b;
}
}  // namespace

const char* KindName(EntityKind k) {
    const auto i = static_cast<size_t>(k);
    return i < std::size(kKinds) ? kKinds[i] : "Other";
}

bool KindFromName(std::string_view s, EntityKind* out) {
    for (size_t i = 0; i < std::size(kKinds); ++i)
        if (s == kKinds[i]) {
            *out = static_cast<EntityKind>(i);
            return true;
        }
    return false;
}

const char* TypeName(VarType t) {
    const auto i = static_cast<size_t>(t);
    return i < std::size(kTypes) ? kTypes[i] : "Undefined";
}

std::string VecJson(const Vec3& v) {
    return jsonmini::Obj().Raw("x", Num(v.x)).Raw("y", Num(v.y)).Raw("z", Num(v.z)).End();
}

std::string SnapshotJson(const Snapshot& s, bool available, bool attract) {
    const Match& m = s.match;
    const std::string match = jsonmini::Obj()
                                  .Bool("inMatch", m.inMatch)
                                  .Bool("attract", attract)
                                  .Bool("online", m.online)
                                  .Int("currentTeam", m.currentTeam)
                                  .Int("activeWorm", m.activeWorm)
                                  .Int("turnMs", m.turnMs)
                                  .Int("turnMsLeft", m.turnMsLeft)
                                  .Int("roundMs", m.roundMs)
                                  .Int("roundMsLeft", m.roundMsLeft)
                                  .Raw("windSpeed", Num(m.windSpeed))
                                  .Raw("windDir", Num(m.windDir))
                                  .Raw("waterLevel", Num(m.waterLevel))
                                  .UInt("turnsStarted", m.turnsStarted)
                                  .Bool("suddenDeath", m.suddenDeath)
                                  .Str("theme", m.theme)
                                  .End();
    jsonmini::Arr teams, worms;
    for (int i = 0; i < s.teamCount && i < 4; ++i) {
        const Team& t = s.teams[i];
        teams.Raw(jsonmini::Obj()
                      .Int("slot", t.slot)
                      .Str("name", t.name)
                      .Bool("active", t.active)
                      .Bool("ai", t.ai)
                      .Bool("local", t.local)
                      .Int("colour", t.colour)
                      .Int("alliance", t.alliance)
                      .Int("roundsWon", t.roundsWon)
                      .Int("score", t.score)
                      .End());
    }
    for (int i = 0; i < s.wormCount && i < 16; ++i) {
        const Worm& w = s.worms[i];
        worms.Raw(jsonmini::Obj()
                      .Int("slot", w.slot)
                      .Int("team", w.team)
                      .Int("posInTeam", w.posInTeam)
                      .Str("name", w.name)
                      .Bool("active", w.active)
                      .Bool("alive", w.alive)
                      .Int("health", w.health)
                      .Int("physicsState", w.physicsState)
                      .Int("weapon", w.weapon)
                      .Raw("pos", VecJson(w.pos))
                      .Raw("vel", VecJson(w.vel))
                      .End());
    }
    return jsonmini::Obj()
        .Bool("available", available)
        .UInt("frame", s.frame)
        .UInt("matchSerial", s.matchSerial)
        .Raw("match", match)
        .Raw("teams", teams.End())
        .Raw("worms", worms.End())
        .End();
}

std::string EntityJson(const Entity& e) {
    jsonmini::Obj o;
    o.UInt("handle", e.handle)
        .UInt("object", e.object)
        .UInt("vtable", e.vtable)
        .Str("kind", KindName(e.kind))
        .Str("type", e.type)
        .Str("label", e.label);
    if (e.hasPos) o.Raw("pos", VecJson(e.pos)).Raw("vel", VecJson(e.vel));
    else o.Raw("pos", "null").Raw("vel", "null");
    return o.End();
}

std::string EntitiesJson(const Entity* e, int n, uint32_t kinds) {
    jsonmini::Arr a;
    for (int i = 0; i < n; ++i)
        if (kinds & KindBit(e[i].kind)) a.Raw(EntityJson(e[i]));
    return a.End();
}

std::string VarJson(const Var& v) {
    return jsonmini::Obj().Str("name", v.name).Str("type", TypeName(v.type)).Raw("value", v.value[0] ? v.value : "null").End();
}
}  // namespace melange::gamestate::wire
