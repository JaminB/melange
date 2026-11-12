// spawns.mode areas: ergSpawnDeal in the match VM. The chunk passes its areas spec and its team count; the answer is a
// table of team rank (1-based, the match's teams in TeamIndex order) -> area (1-based), or nil when the spec is refused
// (the chunk then uses its own fixed deal). The seed is the logic RNG's state when the match VM was made (LogicSeed),
// which every peer of a match shares and a Wormsign replay forces, so the deal is the same everywhere.
#include "levels/spawn_deal.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "erg/luagen.h"
#include "lua/engine50.h"
#include "melange/jlog.h"

namespace melange::levels::spawndeal {
namespace {
namespace l5 = melange::lua50;
constexpr int kMaxTeams = 16;
bool g_subscribed = false;

// Every C++ object is gone before the first Lua call (lua_error is a longjmp).
bool Deal(const char* spec, int teams, int out[kMaxTeams], uint32_t* seedOut) {
    erg::SpawnAreas areas;
    if (!spec || teams < 1 || teams > kMaxTeams || !erg::luagen::ParseAreasSpec(spec, &areas)) return false;
    const uint32_t seed = l5::LogicSeed();
    const std::vector<int> deal = erg::luagen::DealAreas(areas, teams, seed);
    std::string text;
    for (int r = 0; r < teams; ++r) {
        out[r] = deal[r];
        text += (r ? ", " : "") + std::string(deal[r] < 0 ? "-" : std::to_string(deal[r]));
    }
    *seedOut = seed;
    LOG_INFO("[levels] spawn areas: seed %08x, %d teams -> areas [%s] (by team rank)", seed, teams, text.c_str());
    jlog::Rec("levels", jlog::Level::Info, "spawn_deal").Hex("seed", seed).Int("teams", teams).Str("areas", text);
    return true;
}

int __cdecl ErgSpawnDeal(l5::State* L) {
    const auto& a = l5::A();
    const char* spec = a.isstring(L, 1) ? a.tostring(L, 1) : nullptr;
    const int teams = a.isnumber(L, 2) ? static_cast<int>(a.tonumber(L, 2)) : 0;
    int deal[kMaxTeams];
    uint32_t seed = 0;
    if (!Deal(spec, teams, deal, &seed)) {
        LOG_WARN("[levels] spawn areas: ergSpawnDeal refused its arguments (%d teams); the chunk's fixed deal is used", teams);
        a.pushnil(L);
        return 1;
    }
    a.newtable(L);
    for (int r = 0; r < teams; ++r) {
        if (deal[r] < 0) continue;
        a.pushnumber(L, static_cast<float>(deal[r] + 1));
        a.rawseti(L, -2, r + 1);
    }
    return 1;
}

void OnContext(bool created, l5::State* L, void*) {
    if (!created || !L) return;
    const auto& a = l5::A();
    a.pushstring(L, "ergSpawnDeal");
    a.pushcclosure(L, &ErgSpawnDeal, 0);
    a.rawset(L, l5::kGlobals);
}
}  // namespace

void OnFrame() {
    if (g_subscribed) return;
    g_subscribed = true;
    const bool ok = l5::Track() && l5::OnContext(&OnContext, nullptr) != 0;
    LOG_INFO("[levels] spawn areas: ergSpawnDeal %s", ok ? "installed in every match VM" : "NOT installed (area spawns use the chunk's fixed deal)");
}
}  // namespace melange::levels::spawndeal
