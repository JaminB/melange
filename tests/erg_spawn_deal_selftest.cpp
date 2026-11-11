// Offline self-test for spawns.mode areas (no game): the areas spec the chunk carries, the seeded deal (pattern,
// opposite pairs, the team shuffle, determinism, spread over seeds) and the generated chunk run in stock Lua 5.0.1 with
// float numbers (the engine's match VM build) over fake worm containers, with and without Melange's ergSpawnDeal.
// Exit code 0 = all passed.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "erg/luagen.h"
#include "erg/scene.h"

namespace erg = melange::erg;
namespace luagen = melange::erg::luagen;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

// Dustcrown's areas: four sand areas (W, E, N, S; W-E and N-S opposite) and the palace.
erg::SpawnAreas Desert() {
    erg::SpawnAreas a;
    a.areas = {{"SAND W", "sand", 4}, {"SAND E", "sand", 4}, {"SAND N", "sand", 4}, {"SAND S", "sand", 4}, {"PALACE", "palace", 4}};
    a.pattern = {{"sand", false}, {"sand", true}, {"palace", false}, {"sand", false}};
    a.opposite = {{0, 1}, {2, 3}};
    return a;
}

erg::SpawnAreas Unnamed(erg::SpawnAreas a) {
    for (auto& ar : a.areas) ar.name.clear();
    return a;
}

bool Opposite(const erg::SpawnAreas& a, int x, int y) {
    for (const auto& [p, q] : a.opposite)
        if ((p == x && q == y) || (p == y && q == x)) return true;
    return false;
}

void TestSpec() {
    const erg::SpawnAreas d = Desert();
    std::string err;
    Expect(erg::ValidateSpawnAreas(d, &err), "the desert areas validate: " + err);
    const std::string spec = luagen::AreasSpec(d);
    Expect(spec == "sand:4/sand:4/sand:4/sand:4/palace:4;sand/sand:opposite/palace/sand;0-1/2-3", "the spec text: " + spec);
    erg::SpawnAreas back;
    Expect(luagen::ParseAreasSpec(spec, &back) && back == Unnamed(d), "the spec parses back (without names)");
    for (const char* bad : {"", ";;","sand:4/sand:0;;", "sand:9;;", "Sand:4;;", "sand:4;palace;", "sand:4/sand:4;;0-0",
                            "sand:4/sand:4;;0-2", "sand:4/sand:4/sand:4;;0-1/1-2", "sand:04;;", "sand:4;sand:opposite:opposite;",
                            "sand:4;;;", "sand:4 ;;", "sand:4;\";", "sand:4/sand:4/sand:4/sand:4/sand:4/sand:4/sand:4/sand:4/sand:4;;"}) {
        erg::SpawnAreas x;
        Expect(!luagen::ParseAreasSpec(bad, &x), std::string("a bad spec is refused: ") + bad);
    }
    Expect(luagen::ParseAreasSpec("a:1;;", &back) && back.areas.size() == 1 && back.pattern.empty(), "a lone area without a pattern parses");
    erg::SpawnAreas many;
    for (int i = 0; i < 5; ++i) many.areas.push_back({"A" + std::to_string(i), "g", 8});
    Expect(!erg::ValidateSpawnAreas(many, &err) && err.find("40 knots") != std::string::npos, "more than 32 knots are refused: " + err);
    erg::SpawnAreas twice = d;
    twice.areas[1].name = "SAND W";
    Expect(!erg::ValidateSpawnAreas(twice, &err), "a name used twice is refused");
    Expect(d.First(0) == 0 && d.First(2) == 8 && d.First(4) == 16 && d.Knots() == 20, "area i's knots follow the earlier areas'");
}

void TestDeal() {
    const erg::SpawnAreas d = Desert();
    std::set<std::vector<int>> seen3;
    std::map<int, int> firstSand;
    int good = 0;
    for (uint32_t seed = 1; seed <= 400; ++seed) {
        const uint32_t s = seed * 2654435761u;
        Expect(luagen::DealAreas(d, 4, s) == luagen::DealAreas(d, 4, s), "the deal is the same for the same seed");
        bool ok = true;
        // 2 teams: two opposite sand areas
        const auto two = luagen::DealAreas(d, 2, s);
        ok &= two.size() == 2 && two[0] >= 0 && two[1] >= 0 && two[0] < 4 && two[1] < 4 && Opposite(d, two[0], two[1]);
        // 3 teams: two opposite sand areas and the palace
        const auto three = luagen::DealAreas(d, 3, s);
        int palace = 0;
        std::vector<int> sand;
        for (int a : three) {
            if (a == 4) ++palace;
            else if (a >= 0) sand.push_back(a);
        }
        ok &= palace == 1 && sand.size() == 2 && Opposite(d, sand[0], sand[1]);
        seen3.insert(three);
        // 4 teams: three sand areas and the palace, all different
        const auto four = luagen::DealAreas(d, 4, s);
        ok &= std::set<int>(four.begin(), four.end()).size() == 4 && std::count(four.begin(), four.end(), 4) == 1 &&
              std::count(four.begin(), four.end(), -1) == 0;
        // 5 teams fill every area; a 6th has none
        const auto six = luagen::DealAreas(d, 6, s);
        ok &= std::count(six.begin(), six.end(), -1) == 1 && std::set<int>(six.begin(), six.end()).size() == 6;
        good += ok;
        for (int a : two) ++firstSand[a];
    }
    Expect(good == 400, "over 400 seeds every deal keeps the pattern (2: opposite sand; 3: + palace; 4: 3 sand + palace): " + std::to_string(good));
    // two axes x the 3! ways the teams sit on them
    Expect(seen3.size() == 12, "every 3-team deal comes up over the seeds (" + std::to_string(seen3.size()) + " of 12 seen)");
    bool spread = firstSand.size() == 4;
    for (const auto& [a, n] : firstSand) spread &= n > 100;
    Expect(spread, "both axes and every side come up over the seeds");
    // without a seed: the chunk's fixed deal, the lowest area of each step, the teams in order
    Expect(luagen::DealAreas(d, 5, std::nullopt) == std::vector<int>({0, 1, 4, 2, 3}), "the fixed deal is W, E, palace, N, S");
    Expect(luagen::DealAreas(d, 0, 7).empty(), "no teams, no deal");
    {
        // pinned in tools/erg-mcp/test/spawnareas.test.ts too (its dealAreas must draw alike)
        auto text = [](const std::vector<int>& v) {
            std::string t;
            for (int x : v) t += (t.empty() ? "" : ",") + std::to_string(x);
            return t;
        };
        const std::string a = text(luagen::DealAreas(d, 4, 12345u)), b = text(luagen::DealAreas(d, 5, 0xdeadbeefu));
        Expect(a == "2,3,1,4" && b == "1,4,0,2,3", "the pinned deals: " + a + " / " + b);
    }
    // no pattern: a seeded order of every area
    erg::SpawnAreas free = d;
    free.pattern.clear();
    std::set<std::vector<int>> seenFree;
    for (uint32_t s = 1; s <= 50; ++s) seenFree.insert(luagen::DealAreas(free, 5, s * 40503u));
    Expect(seenFree.size() > 10, "without a pattern the areas still deal in a seeded order");
    // an opposite step with nothing dealt yet, and one whose partner is taken, picks any area of its group
    erg::SpawnAreas lone = d;
    lone.pattern = {{"sand", true}, {"sand", false}, {"sand", true}};
    for (uint32_t s = 1; s <= 50; ++s) {
        const auto deal = luagen::DealAreas(lone, 3, s * 7919u);
        Expect(std::set<int>(deal.begin(), deal.end()).size() == 3 && std::count(deal.begin(), deal.end(), 4) == 0,
               "opposite steps without a free partner still deal sand");
    }
}

// ---- the chunk in Lua 5.0.1 (float numbers)

struct World {
    std::vector<std::pair<bool, int>> worms;          // active, TeamIndex, for containers 0..15
    uint32_t seed = 0;
    bool withDeal = true;
};
World* g_world = nullptr;

int LuaDeal(lua_State* L) {
    const char* spec = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    const int teams = static_cast<int>(lua_tonumber(L, 2));
    erg::SpawnAreas a;
    if (!spec || !luagen::ParseAreasSpec(spec, &a)) {
        lua_pushnil(L);
        return 1;
    }
    const auto deal = luagen::DealAreas(a, teams, g_world->seed);
    lua_newtable(L);
    for (int r = 0; r < teams; ++r)
        if (deal[r] >= 0) {
            lua_pushnumber(L, static_cast<float>(deal[r] + 1));
            lua_rawseti(L, -2, r + 1);
        }
    return 1;
}

constexpr char kFakes[] =
    "spawns = {}\n"
    "local worms = {}\n"
    "function lib_GetWormContainerName(i) return i end\n"
    "function EditContainer(i)\n"
    "    if not worms[i] then worms[i] = {Active = wormActive[i + 1], TeamIndex = wormTeam[i + 1]} end\n"
    "    return i, worms[i]\n"
    "end\n"
    "function CloseContainer(i) spawns[i + 1] = worms[i].Spawn end\n"
    "function lib_SetupMultiplayerWormsAndTeams() end\n"
    "function Initialise() end\n"
    "function SetData() end\n"
    "function SendMessage() end\n";

// The chunk wraps the setup only while the level's Initialise runs (the fake one runs the setup, as stdvs does), so the
// chunk's Initialise is called and each container's knot read back ("" = left to the game), or an error text.
constexpr char kFakesInit[] = "function Initialise() lib_SetupMultiplayerWormsAndTeams() end\n";

std::vector<std::string> RunSetup(const std::string& chunk, const World& w, std::string* error) {
    g_world = const_cast<World*>(&w);
    lua_State* L = lua_open();
    lua_newtable(L);
    lua_newtable(L);
    for (int i = 0; i < 16; ++i) {
        lua_pushboolean(L, w.worms[i].first);
        lua_rawseti(L, -3, i + 1);
        lua_pushnumber(L, static_cast<float>(w.worms[i].second));
        lua_rawseti(L, -2, i + 1);
    }
    lua_setglobal(L, "wormTeam");
    lua_setglobal(L, "wormActive");
    if (w.withDeal) lua_register(L, "ergSpawnDeal", &LuaDeal);
    const std::string all = std::string(kFakes) + kFakesInit + chunk + "Initialise()\n";
    std::vector<std::string> out;
    if (luaL_loadbuffer(L, all.data(), all.size(), "=chunk") || lua_pcall(L, 0, 0, 0)) {
        *error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "?";
        lua_close(L);
        return out;
    }
    lua_getglobal(L, "spawns");
    for (int i = 0; i < 16; ++i) {
        lua_rawgeti(L, -1, i + 1);
        out.push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    lua_close(L);
    return out;
}

// teams of `per` worms in slot order: team t's worms in containers t*per..
World Teams(int teams, int per) {
    World w;
    w.worms.assign(16, {false, 0});
    for (int t = 0; t < teams; ++t)
        for (int k = 0; k < per; ++k) w.worms[t * per + k] = {true, t};
    return w;
}

int KnotNumber(const std::string& k) { return k.size() > 4 && k.compare(0, 4, "WORM") == 0 ? std::stoi(k.substr(4)) : -1; }

void TestChunk() {
    const erg::SpawnAreas d = Desert();
    luagen::ChunkSpec spec;
    spec.areas = Unnamed(d);
    const std::string stem = "mymaps_dunes";
    const std::string text = luagen::Text(stem, spec);
    std::string why;
    luagen::ChunkSpec back;
    Expect(!text.empty() && luagen::IsGenerated(stem, text, &why) && luagen::Parse(stem, text, &back) && back.areas == spec.areas,
           "the areas chunk is generated, verifies and parses back: " + why);
    Expect(text.find("ergSpawnDeal(\"sand:4/sand:4/sand:4/sand:4/palace:4;sand/sand:opposite/palace/sand;0-1/2-3\", n) or {1, 2, 5, 3, 4}") != std::string::npos,
           "the chunk asks Melange for the deal, with its fixed deal as the fallback");
    {
        std::string edited = text;
        edited.replace(edited.find("palace:4"), 8, "palace:5");
        Expect(!luagen::IsGenerated(stem, edited, &why), "an edited spec (knots changed, tables not) is refused");
        std::string other = text;
        other.replace(other.find("{1, 2, 5, 3, 4}"), 15, "{5, 2, 1, 3, 4}");
        Expect(!luagen::IsGenerated(stem, other, &why), "an edited fallback is refused");
    }
    luagen::ChunkSpec named = spec;
    named.areas = d;
    Expect(luagen::Text(stem, named).empty(), "a chunk spec with names is refused (the chunk carries none)");
    luagen::ChunkSpec both = spec;
    both.teams = true;
    Expect(luagen::Text(stem, both).empty(), "area and team spawns together are refused");
    // the scene's areas become the chunk's
    erg::Scene sc;
    sc.stem = stem;
    sc.spawns = erg::SpawnMode::Areas;
    sc.spawnAreas = d;
    Expect(luagen::Needed(sc) && luagen::Chunk(sc) == text, "a scene in spawns.mode areas makes the areas chunk");

    for (int teams = 2; teams <= 4; ++teams)
        for (uint32_t seed : {11u, 222u, 3333u, 44444u, 0xdeadbeefu}) {
            World w = Teams(teams, 3);
            w.seed = seed;
            std::string err;
            const auto knots = RunSetup(text, w, &err);
            Expect(err.empty() && knots.size() == 16, "the chunk runs in Lua 5.0: " + err);
            if (knots.size() != 16) continue;
            const auto deal = luagen::DealAreas(d, teams, seed);
            bool ok = true;
            for (int t = 0; t < teams; ++t)
                for (int k = 0; k < 3; ++k) ok &= KnotNumber(knots[t * 3 + k]) == d.First(deal[t]) + k;
            for (int i = teams * 3; i < 16; ++i) ok &= knots[i].empty();
            Expect(ok, "each worm stands on its team's area's knots in slot order (" + std::to_string(teams) + " teams, seed " + std::to_string(seed) + ")");
        }
    {
        // teams with gaps in TeamIndex (1 and 3) rank 1st and 2nd; a 5-worm team's 5th worm keeps the game's spawn
        World w;
        w.worms.assign(16, {false, 0});
        for (int k = 0; k < 5; ++k) w.worms[k] = {true, 3};
        for (int k = 5; k < 7; ++k) w.worms[k] = {true, 1};
        w.seed = 99;
        std::string err;
        const auto knots = RunSetup(text, w, &err);
        const auto deal = luagen::DealAreas(d, 2, 99u);
        bool ok = err.empty() && knots.size() == 16;
        if (ok) {
            for (int k = 0; k < 4; ++k) ok &= KnotNumber(knots[k]) == d.First(deal[1]) + k;   // TeamIndex 3: rank 2
            ok &= knots[4].empty();
            for (int k = 0; k < 2; ++k) ok &= KnotNumber(knots[5 + k]) == d.First(deal[0]) + k;   // TeamIndex 1: rank 1
        }
        Expect(ok, "teams rank by TeamIndex, and a worm past its area's knots is left to the game: " + err);
    }
    {
        // without Melange's ergSpawnDeal: the fixed deal W, E, palace, N
        World w = Teams(4, 2);
        w.withDeal = false;
        std::string err;
        const auto knots = RunSetup(text, w, &err);
        Expect(err.empty() && knots.size() == 16 && knots[0] == "WORM0" && knots[1] == "WORM1" && knots[2] == "WORM4" &&
                   knots[4] == "WORM16" && knots[5] == "WORM17" && knots[6] == "WORM8",
               "without ergSpawnDeal the chunk uses its fixed deal: " + err);
    }
    {
        // 6 teams over 5 areas: the 6th keeps the game's spawns
        World w = Teams(6, 2);
        w.seed = 5;
        std::string err;
        const auto knots = RunSetup(text, w, &err);
        const auto deal = luagen::DealAreas(d, 6, 5u);
        int left = 0;
        for (int t = 0; t < 6; ++t)
            if (deal[t] < 0) left += knots[t * 2].empty() && knots[t * 2 + 1].empty();
        Expect(err.empty() && left == 1, "a team past the areas keeps the game's spawns: " + err);
    }
}
}  // namespace

int main() {
    TestSpec();
    TestDeal();
    TestChunk();
    printf("erg_spawn_deal_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
