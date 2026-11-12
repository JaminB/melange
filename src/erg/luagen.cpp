#include "erg/luagen.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace melange::erg::luagen {
namespace {
constexpr std::string_view kWater = "SetData(\"Water.Level\", ";
constexpr std::string_view kKnotLine = "worm.Spawn = \"WORM\" .. i\n";
// Team spawns: the setup deals the slots team by team in slot order (lib_SetupMultiplayerWorm sets Active and
// TeamIndex and counts WormIndex up; the engine sets PositionInTeam only later, so it reads 0 here: tried in a Quick
// Game on 2026-11-08), so a worm's place in its team is how many of its team's worms came before it.
constexpr std::string_view kTeamTest = "if worm.Active and worm.TeamIndex < 4 then\n";
constexpr std::string_view kTeamLine = "if p < 4 then worm.Spawn = \"WORM\" .. (worm.TeamIndex * 4 + p) end\n";
// Area spawns: Melange's ergSpawnDeal answers each team rank's area (1-based) from the match's seed; without it (no
// Melange hook) the chunk's own fixed deal. The rank is the team's place among the match's teams in TeamIndex order.
constexpr std::string_view kDealCall = "local deal = ergSpawnDeal and ergSpawnDeal(\"";
constexpr std::string_view kAreaLine = "if a and p < areaCount[a] then worm.Spawn = \"WORM\" .. (areaFirst[a] + p) end\n";
constexpr std::string_view kPlaceLine = "SendMessage(\"GameLogic.PlaceObjects\")\n";
// The level's factory replaces the scheme's: stdvs reads MineFactoryOn after this setup to send CreateRandMineFactory,
// and a second factory trips the engine's one-factory assert. GM.SchemeData is the match's copy of the scheme, which
// vanilla level scripts edit the same way.
constexpr std::string_view kFactoryLines[] = {"local lock, scheme = EditContainer(\"GM.SchemeData\")\n",
                                              "scheme.MineFactoryOn = false\n", "CloseContainer(lock)\n"};
constexpr std::string_view kFactoryKnot = "minefactory";
// schemeMines = false: lib_SetupMinesAndOildrums (run by stdvs and Survivor after this setup) places 15 random mines
// for Objects 1 (mines) and 3 (both) and 10 oil drums for 2 and 3; dropping the mines bit keeps the drums.
constexpr std::string_view kNoMinesLine =
    "if scheme.Objects == 1 then scheme.Objects = 0 elseif scheme.Objects == 3 then scheme.Objects = 2 end\n";
// The factory's kMineFactoryData settings, a comment the engine skips: Melange applies them at level start from the
// parsed chunk (levels/registry.cpp), so a match end can put the game's values back.
constexpr std::string_view kFactoryTweak = "-- minefactory:";
constexpr std::string_view kCrateCall = "ergCrate(", kTelepadCall = "lib_CreateTelepad(", kTriggerCall = "ergTrigger(";
// Without "stdvs" in the script list (Survivor), the loader refuses a chunk that redefines a library function at load
// (0x6998ba, checked once before Initialise). The deferred form installs its setup wrap inside Initialise instead.
constexpr std::string_view kDeferredOpen =
    "local ergInit = Initialise\n"
    "function Initialise()\n"
    "    local ergSetup = lib_SetupMultiplayerWormsAndTeams\n"
    "    lib_SetupMultiplayerWormsAndTeams = function()\n";
constexpr std::string_view kDeferredClose =
    "    end\n"
    "    ergInit()\n"
    "    lib_SetupMultiplayerWormsAndTeams = ergSetup\n"
    "end\n";
constexpr std::string_view kLegacyOpen =
    "local ergSetup = lib_SetupMultiplayerWormsAndTeams\n"
    "function lib_SetupMultiplayerWormsAndTeams()\n";

constexpr std::string_view kCrateFn =
    "local function ergCrate(knot, kind, contents, count, hitpoints, parachute)\n"
    "    SendMessage(\"GameLogic.ResetCrateParameters\")\n"
    "    SetData(\"Crate.Type\", kind)\n"
    "    SetData(\"Crate.Contents\", contents)\n"
    "    SetData(\"Crate.NumContents\", count)\n"
    "    SetData(\"Crate.Spawn\", knot)\n"
    "    SetData(\"Crate.GroundSnap\", 1)\n"
    "    SetData(\"Crate.Parachute\", parachute)\n"
    "    SetData(\"Crate.RandomSpawnPos\", 0)\n"
    "    SetData(\"Crate.Hitpoints\", hitpoints)\n"
    "    SetData(\"Crate.Scale\", 1.0)\n"
    "    SendMessage(\"GameLogic.CreateCrate\")\n"
    "end\n";
constexpr std::string_view kTriggerFn =
    "local function ergTrigger(knot, index, radius, teamCollect, teamDestroy, hitpoints, wormCollect)\n"
    "    SendMessage(\"GameLogic.ResetTriggerParams\")\n"
    "    SetData(\"Trigger.Spawn\", knot)\n"
    "    SetData(\"Trigger.Radius\", radius)\n"
    "    SetData(\"Trigger.Index\", index)\n"
    "    SetData(\"Trigger.TeamCollect\", teamCollect)\n"
    "    SetData(\"Trigger.TeamDestroy\", teamDestroy)\n"
    "    SetData(\"Trigger.HitPoints\", hitpoints)\n"
    "    SetData(\"Trigger.WormCollect\", wormCollect)\n"
    "    SendMessage(\"GameLogic.CreateTrigger\")\n"
    "end\n";

std::string Num(double v) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

std::string Quote(std::string_view s) { return "\"" + std::string(s) + "\""; }

bool HasNamedDetail(const Scene& s, std::string_view name) {
    for (auto& d : s.details)
        if (d.name == name) return true;
    return false;
}

bool Fail(std::string* why, std::string text) {
    if (why) *why = std::move(text);
    return false;
}

std::string Header(std::string_view stem) {
    return "-- generated by Erg for " + std::string(stem) + "; edits are overwritten on export\n";
}

std::optional<double> WaterIn(std::string_view text) {
    const size_t at = text.find(kWater);
    if (at == std::string_view::npos) return std::nullopt;
    const size_t start = at + kWater.size(), end = text.find(')', start);
    if (end == std::string_view::npos || end - start > 24) return std::nullopt;
    const std::string num(text.substr(start, end - start));
    char* stop = nullptr;
    const double v = std::strtod(num.c_str(), &stop);
    if (!stop || *stop || !std::isfinite(v)) return std::nullopt;
    return v;
}

// Emitted lines never hold a literal the parser could not read back, so only well-formed objects are written.
std::string ObjectLine(const ObjectSpec& o) {
    switch (o.type) {
        case ObjectType::Crate: {
            const bool health = o.crate.kind == CrateKind::Health;
            return std::string(kCrateCall) + Quote(o.knot) + ", " + Quote(CrateKindName(o.crate.kind)) + ", " +
                   Quote(health ? "health" : o.crate.contents) + ", " + std::to_string(health ? o.crate.amount : o.crate.count) +
                   ", " + std::to_string(o.crate.hitpoints) + ", " + (o.crate.parachute ? "1" : "0") + ")\n";
        }
        case ObjectType::Telepad: return std::string(kTelepadCall) + Quote(o.knot) + ", " + std::to_string(o.group) + ")\n";
        case ObjectType::Trigger: {
            const TriggerSpec& t = o.trigger;
            return std::string(kTriggerCall) + Quote(o.knot) + ", " + std::to_string(t.index) + ", " + Num(t.radius) + ", " +
                   std::to_string(t.teamCollect) + ", " + std::to_string(t.teamDestroy) + ", " + std::to_string(t.hitpoints) +
                   ", " + (t.wormCollect ? "1" : "0") + ")\n";
        }
        case ObjectType::MineFactory: return {};
    }
    return {};
}

// Splits "a, b, c" of a call's argument list; strings are "..." without escapes.
bool SplitArgs(std::string_view s, std::vector<std::string_view>* out) {
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(", ", i);
        if (j == std::string_view::npos) j = s.size();
        if (j == i) return false;
        out->push_back(s.substr(i, j - i));
        if (j == s.size()) break;
        i = j + 2;
    }
    return !out->empty();
}

bool StrArg(std::string_view a, std::string* out) {
    if (a.size() < 2 || a.front() != '"' || a.back() != '"') return false;
    *out = std::string(a.substr(1, a.size() - 2));
    return out->find_first_of("\"\\\n") == std::string::npos;
}

bool IntArg(std::string_view a, int* out) {
    if (a.empty() || a.size() > 6) return false;
    int v = 0;
    for (char c : a) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

bool NumArg(std::string_view a, double* out) {
    if (a.empty() || a.size() > 24) return false;
    const std::string n(a);
    char* stop = nullptr;
    *out = std::strtod(n.c_str(), &stop);
    return stop && !*stop && std::isfinite(*out);
}

bool ParseObjectLine(std::string_view line, ObjectSpec* o) {
    std::string_view call;
    ObjectType type;
    if (line.starts_with(kCrateCall)) call = line.substr(kCrateCall.size()), type = ObjectType::Crate;
    else if (line.starts_with(kTelepadCall)) call = line.substr(kTelepadCall.size()), type = ObjectType::Telepad;
    else if (line.starts_with(kTriggerCall)) call = line.substr(kTriggerCall.size()), type = ObjectType::Trigger;
    else return false;
    if (call.empty() || call.back() != ')') return false;
    std::vector<std::string_view> a;
    if (!SplitArgs(call.substr(0, call.size() - 1), &a)) return false;
    o->type = type;
    if (!StrArg(a[0], &o->knot)) return false;
    switch (type) {
        case ObjectType::Crate: {
            std::string kind, contents;
            int n = 0, parachute = 0;
            if (a.size() != 6 || !StrArg(a[1], &kind) || !ParseCrateKind(kind, &o->crate.kind) || !StrArg(a[2], &contents) ||
                !IntArg(a[3], &n) || !IntArg(a[4], &o->crate.hitpoints) || !IntArg(a[5], &parachute) || parachute > 1)
                return false;
            o->crate.parachute = parachute == 1;
            if (o->crate.kind == CrateKind::Health) {
                if (contents != "health") return false;
                o->crate.amount = n;
            } else {
                o->crate.contents = contents;
                o->crate.count = n;
            }
            break;
        }
        case ObjectType::Telepad:
            if (a.size() != 2 || !IntArg(a[1], &o->group)) return false;
            break;
        case ObjectType::Trigger: {
            TriggerSpec& t = o->trigger;
            int worm = 0;
            if (a.size() != 7 || !IntArg(a[1], &t.index) || !NumArg(a[2], &t.radius) || !IntArg(a[3], &t.teamCollect) ||
                !IntArg(a[4], &t.teamDestroy) || !IntArg(a[5], &t.hitpoints) || !IntArg(a[6], &worm) || worm > 1)
                return false;
            t.wormCollect = worm == 1;
            break;
        }
        case ObjectType::MineFactory: return false;
    }
    return ValidateObject(*o, "object", nullptr);
}
}  // namespace

ChunkSpec SpecOf(const Scene& s) {
    ChunkSpec c;
    c.knots = s.spawns == SpawnMode::Knots;
    c.teams = s.spawns == SpawnMode::Teams;
    if (s.spawns == SpawnMode::Areas) {
        c.areas = s.spawnAreas;
        for (auto& a : c.areas->areas) a.name.clear();
    }
    c.placeObjects = HasNamedDetail(s, "mine") || HasNamedDetail(s, "oildrum");
    c.objects = s.objects;
    c.water = s.water;
    return c;
}

bool Needed(const Scene& s) {
    const ChunkSpec c = SpecOf(s);
    return c.knots || c.teams || c.areas || c.placeObjects || !c.objects.empty() || c.water.has_value();
}

std::string AreasSpec(const SpawnAreas& a) {
    std::string s;
    for (size_t i = 0; i < a.areas.size(); ++i) s += (i ? "/" : "") + a.areas[i].group + ":" + std::to_string(a.areas[i].knots);
    s += ";";
    for (size_t i = 0; i < a.pattern.size(); ++i) s += (i ? "/" : "") + a.pattern[i].group + (a.pattern[i].opposite ? ":opposite" : "");
    s += ";";
    for (size_t i = 0; i < a.opposite.size(); ++i) s += (i ? "/" : "") + std::to_string(a.opposite[i][0]) + "-" + std::to_string(a.opposite[i][1]);
    return s;
}

namespace {
// Splits on `sep`; an empty text is no parts.
std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    if (s.empty()) return out;
    size_t i = 0;
    while (true) {
        const size_t j = s.find(sep, i);
        out.push_back(s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
        if (j == std::string_view::npos) return out;
        i = j + 1;
    }
}

bool SmallInt(std::string_view s, int* out) { return s.size() <= 2 && IntArg(s, out); }
}  // namespace

bool ParseAreasSpec(std::string_view spec, SpawnAreas* out) {
    if (spec.size() > 512) return false;
    const auto parts = Split(spec, ';');
    if (parts.size() != 3) return false;
    SpawnAreas a;
    for (auto e : Split(parts[0], '/')) {
        const size_t c = e.find(':');
        SpawnArea ar;
        if (c == std::string_view::npos || !SmallInt(e.substr(c + 1), &ar.knots)) return false;
        ar.group = std::string(e.substr(0, c));
        ar.name = "area" + std::to_string(a.areas.size());   // for the validator only
        a.areas.push_back(std::move(ar));
        if (a.areas.size() > kMaxSpawnAreas) return false;
    }
    for (auto e : Split(parts[1], '/')) {
        DealStep st;
        constexpr std::string_view kOpp = ":opposite";
        if (e.ends_with(kOpp)) st.opposite = true, e.remove_suffix(kOpp.size());
        st.group = std::string(e);
        a.pattern.push_back(std::move(st));
        if (a.pattern.size() > kMaxDealSteps) return false;
    }
    for (auto e : Split(parts[2], '/')) {
        const size_t d = e.find('-');
        std::array<int, 2> pr{};
        if (d == std::string_view::npos || !SmallInt(e.substr(0, d), &pr[0]) || !SmallInt(e.substr(d + 1), &pr[1])) return false;
        a.opposite.push_back(pr);
        if (a.opposite.size() > kMaxSpawnAreas) return false;
    }
    if (!ValidateSpawnAreas(a, nullptr) || AreasSpec(a) != spec) return false;
    for (auto& ar : a.areas) ar.name.clear();
    *out = std::move(a);
    return true;
}

std::vector<int> DealAreas(const SpawnAreas& a, int teams, std::optional<uint32_t> seed) {
    // xorshift32 over the seed (never 0); draws only where a choice is open, so a fixed map deals alike everywhere
    uint32_t x = (seed ? *seed : 0u) ^ 0x9E3779B9u;
    if (!x) x = 1;
    auto pick = [&](size_t k) -> size_t {
        if (k <= 1 || !seed) return 0;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x % k;
    };
    const size_t n = a.areas.size();
    std::vector<bool> used(n, false);
    std::vector<int> dealt;
    auto partner = [&](int i) {
        for (const auto& [p, q] : a.opposite) {
            if (p == i) return q;
            if (q == i) return p;
        }
        return -1;
    };
    for (const auto& st : a.pattern) {
        std::vector<int> cand;
        for (size_t i = 0; i < n; ++i)
            if (!used[i] && a.areas[i].group == st.group) cand.push_back(static_cast<int>(i));
        if (cand.empty()) continue;
        int chosen = -1;
        if (st.opposite)
            for (int d : dealt) {   // the partner of the earliest dealt area that has an unused one in the group
                const int q = partner(d);
                if (q >= 0 && std::find(cand.begin(), cand.end(), q) != cand.end()) {
                    chosen = q;
                    break;
                }
            }
        if (chosen < 0) chosen = cand[pick(cand.size())];
        used[chosen] = true;
        dealt.push_back(chosen);
    }
    std::vector<int> rest;
    for (size_t i = 0; i < n; ++i)
        if (!used[i]) rest.push_back(static_cast<int>(i));
    while (!rest.empty()) {
        const size_t k = pick(rest.size());
        dealt.push_back(rest[k]);
        rest.erase(rest.begin() + static_cast<std::ptrdiff_t>(k));
    }
    // the teams shuffled over the dealt areas (Fisher-Yates over the ranks)
    const int t = std::max(0, teams);
    std::vector<int> order(t);
    for (int i = 0; i < t; ++i) order[i] = i;
    if (seed)
        for (int i = t - 1; i > 0; --i) std::swap(order[i], order[pick(static_cast<size_t>(i) + 1)]);
    std::vector<int> out(t, -1);
    for (int j = 0; j < t; ++j) out[order[j]] = j < static_cast<int>(dealt.size()) ? dealt[j] : -1;
    return out;
}

std::string Chunk(const Scene& s) {
    if (!Needed(s)) return {};
    return Text(s.stem, SpecOf(s));
}


namespace {
// "-- minefactory: activation=15 inactiveTurns=3 keepKnot=1 launchHeight=20 launchRadius=12 launchers=-300:900:40/...
// throw=min:max:apex:pattern:avoidWater:scatter:targetShare[:volley:avoidRadius] targets=x:z/x:z avoid=x:z/x:z|x:z/x:z":
// the set fields, in this order (launchers: x:y:z world units, one per launcher, separated by '/'; throw: every field,
// the pattern by name, the volley and avoid radius only when either is set (older lines have the first seven); targets:
// x:z world units, only with a throw; avoid: the corridor lines, points x:z world units separated by '/', lines by '|',
// only with a throw).
std::string FactoryTweakLine(const FactorySpec& f) {
    std::string l(kFactoryTweak);
    if (f.activation) l += " activation=" + std::to_string(*f.activation);
    if (f.inactiveTurns) l += " inactiveTurns=" + std::to_string(*f.inactiveTurns);
    if (f.keepKnot) l += " keepKnot=1";
    if (f.launchHeight) l += " launchHeight=" + std::to_string(*f.launchHeight);
    if (f.launchRadius) l += " launchRadius=" + std::to_string(*f.launchRadius);
    for (size_t i = 0; i < f.launchers.size(); ++i) {
        const auto& p = f.launchers[i];
        l += (i ? "/" : " launchers=") + std::to_string(p[0]) + ":" + std::to_string(p[1]) + ":" + std::to_string(p[2]);
    }
    if (f.throwing) {
        const ThrowSpec& t = *f.throwing;
        l += " throw=" + std::to_string(t.minRange) + ":" + std::to_string(t.maxRange) + ":" + std::to_string(t.apex) + ":" +
             ThrowPatternName(t.pattern) + ":" + (t.avoidWater ? "1" : "0") + ":" + std::to_string(t.scatter) + ":" + std::to_string(t.targetShare);
        if (t.volley || t.avoidRadius) l += ":" + std::to_string(t.volley) + ":" + std::to_string(t.avoidRadius);
        for (size_t i = 0; i < t.targets.size(); ++i) l += (i ? "/" : " targets=") + std::to_string(t.targets[i][0]) + ":" + std::to_string(t.targets[i][1]);
        for (size_t i = 0; i < t.avoid.size(); ++i) {
            l += i ? "|" : " avoid=";
            for (size_t k = 0; k < t.avoid[i].size(); ++k) l += (k ? "/" : "") + std::to_string(t.avoid[i][k][0]) + ":" + std::to_string(t.avoid[i][k][1]);
        }
    }
    return l + "\n";
}

// A signed whole number of at most kMaxLauncherCoord, taken off the front of v.
bool TakeInt(std::string_view* v, int* out) {
    const bool neg = !v->empty() && v->front() == '-';
    if (neg) v->remove_prefix(1);
    if (v->empty() || v->front() < '0' || v->front() > '9') return false;
    int64_t n = 0;
    while (!v->empty() && v->front() >= '0' && v->front() <= '9') {
        n = n * 10 + (v->front() - '0');
        if (n > kMaxLauncherCoord) return false;
        v->remove_prefix(1);
    }
    *out = static_cast<int>(neg ? -n : n);
    return true;
}

// "min:max:apex:pattern:avoidWater:scatter:targetShare[:volley:avoidRadius]"
bool ParseThrow(std::string_view v, ThrowSpec* t) {
    auto field = [&](int* out) {
        if (!TakeInt(&v, out)) return false;
        if (v.empty() || v.front() != ':') return false;
        v.remove_prefix(1);
        return true;
    };
    if (!field(&t->minRange) || !field(&t->maxRange) || !field(&t->apex)) return false;
    const size_t colon = v.find(':');
    if (colon == std::string_view::npos || !ParseThrowPattern(v.substr(0, colon), &t->pattern)) return false;
    v.remove_prefix(colon + 1);
    int water = 0;
    if (!field(&water) || water < 0 || water > 1 || !field(&t->scatter) || !TakeInt(&v, &t->targetShare)) return false;
    t->avoidWater = water == 1;
    if (v.empty()) return true;
    // the two later fields, together (a line of the first seven is an older project's)
    if (v.front() != ':') return false;
    v.remove_prefix(1);
    if (!field(&t->volley) || !TakeInt(&v, &t->avoidRadius) || !v.empty()) return false;
    if (t->volley < 0 || t->volley > kMaxVolley || t->avoidRadius < 0 || t->avoidRadius > kMaxAvoidRadius || (!t->volley && !t->avoidRadius)) return false;
    return true;
}

// "x:z/x:z": 1-maxPoints points of signed whole world units.
bool ParsePoints(std::string_view v, std::vector<std::array<int, 2>>* out, size_t maxPoints) {
    out->clear();
    while (true) {
        std::array<int, 2> p{};
        if (!TakeInt(&v, &p[0]) || v.empty() || v.front() != ':') return false;
        v.remove_prefix(1);
        if (!TakeInt(&v, &p[1])) return false;
        out->push_back(p);
        if (out->size() > maxPoints) return false;
        if (v.empty()) return true;
        if (v.front() != '/') return false;
        v.remove_prefix(1);
    }
}
bool ParseTargets(std::string_view v, std::vector<std::array<int, 2>>* out) { return ParsePoints(v, out, kMaxThrowTargets); }

// "x:z/x:z|x:z/x:z": 1-kMaxAvoidLines lines of 2-kMaxAvoidPoints points.
bool ParseAvoid(std::string_view v, std::vector<std::vector<std::array<int, 2>>>* out) {
    out->clear();
    while (true) {
        const size_t bar = v.find('|');
        std::vector<std::array<int, 2>> line;
        if (!ParsePoints(v.substr(0, bar), &line, kMaxAvoidPoints) || line.size() < 2) return false;
        out->push_back(std::move(line));
        if (out->size() > kMaxAvoidLines) return false;
        if (bar == std::string_view::npos) return true;
        v.remove_prefix(bar + 1);
    }
}

// "x:y:z/x:y:z": 1-kMaxLaunchers points of signed whole world units within kMaxLauncherCoord.
bool ParseLaunchers(std::string_view v, std::vector<std::array<int, 3>>* out) {
    out->clear();
    while (true) {
        std::array<int, 3> p{};
        for (size_t k = 0; k < 3; ++k) {
            const bool neg = !v.empty() && v.front() == '-';
            if (neg) v.remove_prefix(1);
            if (v.empty() || v.front() < '0' || v.front() > '9') return false;
            int64_t n = 0;
            while (!v.empty() && v.front() >= '0' && v.front() <= '9') {
                n = n * 10 + (v.front() - '0');
                if (n > kMaxLauncherCoord) return false;
                v.remove_prefix(1);
            }
            p[k] = static_cast<int>(neg ? -n : n);
            if (k < 2) {
                if (v.empty() || v.front() != ':') return false;
                v.remove_prefix(1);
            }
        }
        out->push_back(p);
        if (out->size() > kMaxLaunchers) return false;
        if (v.empty()) return true;
        if (v.front() != '/') return false;
        v.remove_prefix(1);
    }
}

// Reads the line FactoryTweakLine writes, if there is one; Upgrade's regeneration check refuses any other form.
bool ParseFactoryTweak(std::string_view text, FactorySpec* f) {
    const size_t at = text.find("\n" + std::string(kFactoryTweak));
    if (at == std::string_view::npos) return true;
    const size_t start = at + 1 + kFactoryTweak.size(), end = text.find('\n', start);
    if (end == std::string_view::npos) return false;
    std::string_view rest = text.substr(start, end - start);
    while (!rest.empty()) {
        if (rest.front() != ' ') return false;
        rest.remove_prefix(1);
        const size_t eq = rest.find('='), sp = rest.find(' ');
        if (eq == std::string_view::npos || (sp != std::string_view::npos && sp < eq)) return false;
        const std::string_view key = rest.substr(0, eq);
        const std::string_view num = rest.substr(eq + 1, sp == std::string_view::npos ? std::string_view::npos : sp - eq - 1);
        int v = 0;
        if (key == "launchers") {
            if (!f->launchers.empty() || !ParseLaunchers(num, &f->launchers)) return false;
            rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp);
            continue;
        }
        if (key == "throw") {
            ThrowSpec t;
            if (f->throwing || !ParseThrow(num, &t)) return false;
            f->throwing = t;
            rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp);
            continue;
        }
        if (key == "targets") {
            // after the throw it belongs to, once
            if (!f->throwing || !f->throwing->targets.empty() || !ParseTargets(num, &f->throwing->targets)) return false;
            rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp);
            continue;
        }
        if (key == "avoid") {
            // after the throw it belongs to, once, and only with its radius
            if (!f->throwing || !f->throwing->avoid.empty() || !f->throwing->avoidRadius || !ParseAvoid(num, &f->throwing->avoid)) return false;
            rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp);
            continue;
        }
        if (!IntArg(num, &v) || v > 255) return false;
        if (key == "activation") f->activation = v;
        else if (key == "inactiveTurns") f->inactiveTurns = v;
        else if (key == "keepKnot" && v == 1) f->keepKnot = true;
        else if (key == "launchHeight" && v >= 1) f->launchHeight = v;
        else if (key == "launchRadius") f->launchRadius = v;
        else return false;
        rest = sp == std::string_view::npos ? std::string_view{} : rest.substr(sp);
    }
    return true;
}

// The area deal's lines (spawns.mode areas): rank the match's teams, ask for the deal, put each worm on its area's knots.
void AreaLines(const SpawnAreas& a, const std::string& in, std::string* out) {
    std::string first, count, fallback;
    for (size_t i = 0; i < a.areas.size(); ++i) {
        first += (i ? ", " : "") + std::to_string(a.First(i));
        count += (i ? ", " : "") + std::to_string(a.areas[i].knots);
    }
    const std::vector<int> fixed = DealAreas(a, static_cast<int>(a.areas.size()), std::nullopt);
    for (size_t i = 0; i < fixed.size(); ++i) fallback += (i ? ", " : "") + std::to_string(fixed[i] + 1);
    std::string& o = *out;
    o += in + "local areaFirst, areaCount = {" + first + "}, {" + count + "}\n";
    o += in + "local present, rank, n = {}, {}, 0\n";
    o += in + "for i = 0, 15 do\n";
    o += in + "    local lock, worm = EditContainer(lib_GetWormContainerName(i))\n";
    o += in + "    if worm.Active then present[worm.TeamIndex + 1] = true end\n";
    o += in + "    CloseContainer(lock)\n";
    o += in + "end\n";
    o += in + "for t = 1, 16 do\n";
    o += in + "    if present[t] then\n";
    o += in + "        n = n + 1\n";
    o += in + "        rank[t] = n\n";
    o += in + "    end\n";
    o += in + "end\n";
    o += in + std::string(kDealCall) + AreasSpec(a) + "\", n) or {" + fallback + "}\n";
    o += in + "local placed = {}\n";
    o += in + "for i = 0, 15 do\n";
    o += in + "    local lock, worm = EditContainer(lib_GetWormContainerName(i))\n";
    o += in + "    if worm.Active then\n";
    o += in + "        local r = rank[worm.TeamIndex + 1]\n";
    o += in + "        local a, p = deal[r], placed[r] or 0\n";
    o += in + "        placed[r] = p + 1\n";
    o += in + "        " + std::string(kAreaLine);
    o += in + "    end\n";
    o += in + "    CloseContainer(lock)\n";
    o += in + "end\n";
}

std::string Emit(std::string_view stem, const ChunkSpec& c, bool deferred) {
    if (!c.knots && !c.teams && !c.areas && !c.placeObjects && c.objects.empty() && !c.water) return {};
    if (c.knots + c.teams + c.areas.has_value() > 1) return {};
    if (c.areas) {
        // the chunk carries no names: validate a copy with placeholder names
        SpawnAreas v = *c.areas;
        for (size_t i = 0; i < v.areas.size(); ++i)
            if (!v.areas[i].name.empty()) return {};
            else v.areas[i].name = "area" + std::to_string(i);
        if (!ValidateSpawnAreas(v, nullptr)) return {};
    }
    bool crates = false, triggers = false;
    int factories = 0;
    const FactorySpec* factory = nullptr;
    for (auto& o : c.objects) {
        if (!ValidateObject(o, "object", nullptr)) return {};
        crates |= o.type == ObjectType::Crate;
        triggers |= o.type == ObjectType::Trigger;
        if (o.type == ObjectType::MineFactory) ++factories, factory = &o.factory;
    }
    if (factories > 1) return {};
    std::string out = Header(stem);
    if (factory && (factory->Tweaks() || factory->keepKnot || factory->Launch())) out += FactoryTweakLine(*factory);
    if (c.water) out += std::string(kWater) + Num(*c.water) + ")\n";
    if (c.knots || c.teams || c.areas || c.placeObjects || !c.objects.empty()) {
        if (crates) out += kCrateFn;
        if (triggers) out += kTriggerFn;
        out += deferred ? kDeferredOpen : kLegacyOpen;
        const std::string in(deferred ? 8 : 4, ' ');
        out += in + "ergSetup()\n";
        if (c.knots) {
            out += in + "for i = 0, 7 do\n";
            out += in + "    local lock, worm = EditContainer(lib_GetWormContainerName(i))\n";
            out += in + "    " + std::string(kKnotLine);
            out += in + "    CloseContainer(lock)\n";
            out += in + "end\n";
        }
        if (c.teams) {
            out += in + "local placed = {0, 0, 0, 0}\n";
            out += in + "for i = 0, 15 do\n";
            out += in + "    local lock, worm = EditContainer(lib_GetWormContainerName(i))\n";
            out += in + "    " + std::string(kTeamTest);
            out += in + "        local p = placed[worm.TeamIndex + 1]\n";
            out += in + "        placed[worm.TeamIndex + 1] = p + 1\n";
            out += in + "        " + std::string(kTeamLine);
            out += in + "    end\n";
            out += in + "    CloseContainer(lock)\n";
            out += in + "end\n";
        }
        if (c.areas) AreaLines(*c.areas, in, &out);
        if (factory) {
            out += in + std::string(kFactoryLines[0]) + in + std::string(kFactoryLines[1]);
            if (!factory->schemeMines) out += in + std::string(kNoMinesLine);
            out += in + std::string(kFactoryLines[2]);
        }
        if (factories || c.placeObjects) out += in + std::string(kPlaceLine);
        for (auto& o : c.objects)
            if (o.type != ObjectType::MineFactory) out += in + ObjectLine(o);
        if (c.water) out += in + std::string(kWater) + Num(*c.water) + ")\n";
        out += deferred ? kDeferredClose : "end\n";
    }
    if (out.size() > kMaxChunkBytes) return {};
    return out;
}

std::string_view TrimIndent(std::string_view line) {
    const size_t at = line.find_first_not_of(' ');
    return at == std::string_view::npos ? std::string_view{} : line.substr(at);
}
}  // namespace

std::string Text(std::string_view stem, const ChunkSpec& c) { return Emit(stem, c, true); }

std::string LegacyText(std::string_view stem, const ChunkSpec& c) { return Emit(stem, c, false); }

std::string Text(std::string_view stem, bool knots, bool objects, std::optional<double> water) {
    ChunkSpec c;
    c.knots = knots;
    c.placeObjects = objects;
    c.water = water;
    return Text(stem, c);
}

std::string Stub(std::string_view stem) { return "-- generated by Erg for " + std::string(stem) + "; no changes\n"; }

bool Parse(std::string_view stem, std::string_view text, ChunkSpec* out) {
    if (!text.starts_with(Header(stem))) return false;
    ChunkSpec c;
    c.water = WaterIn(text);
    c.knots = text.find(kKnotLine) != std::string_view::npos;
    c.teams = text.find(kTeamLine) != std::string_view::npos;
    c.placeObjects = text.find(kPlaceLine) != std::string_view::npos;
    if (const size_t at = text.find(kDealCall); at != std::string_view::npos) {
        const size_t start = at + kDealCall.size(), end = text.find('"', start);
        SpawnAreas a;
        if (end == std::string_view::npos || !ParseAreasSpec(text.substr(start, end - start), &a)) return false;
        c.areas = std::move(a);
    }
    if (text.find(kFactoryLines[1]) != std::string_view::npos) {
        ObjectSpec f;
        f.type = ObjectType::MineFactory;
        f.knot = kFactoryKnot;
        f.factory.schemeMines = text.find(kNoMinesLine) == std::string_view::npos;
        if (!ParseFactoryTweak(text, &f.factory)) return false;
        c.objects.push_back(std::move(f));
    }
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string_view::npos) e = text.size();
        const std::string_view line = TrimIndent(text.substr(i, e - i));
        if (line.starts_with(kCrateCall) || line.starts_with(kTelepadCall) || line.starts_with(kTriggerCall)) {
            if (c.objects.size() >= kMaxObjects) return false;
            ObjectSpec o;
            if (!ParseObjectLine(line, &o)) return false;
            c.objects.push_back(std::move(o));
        }
        i = e + 1;
    }
    *out = std::move(c);
    return true;
}

bool IsGenerated(std::string_view stem, std::string_view text, std::string* why) {
    std::string up;
    if (!Upgrade(stem, text, &up, why)) return false;
    return up == text || Fail(why, "is a chunk from an older Erg (build the level again)");
}

bool Upgrade(std::string_view stem, std::string_view text, std::string* out, std::string* why) {
    if (!text.empty() && static_cast<unsigned char>(text.front()) == 0x1B) return Fail(why, "is compiled Lua, which is refused");
    if (text.empty() || text.size() > kMaxChunkBytes) return Fail(why, "is not a chunk generated by Erg");
    if (text == Stub(stem)) {
        *out = std::string(text);
        return true;
    }
    ChunkSpec c;
    if (Parse(stem, text, &c)) {
        std::string want = Text(stem, c);
        if (!want.empty() && (want == text || LegacyText(stem, c) == text)) {
            *out = std::move(want);
            return true;
        }
    }
    return Fail(why, "is not a chunk generated by Erg (hand edits are not run)");
}
}  // namespace melange::erg::luagen
