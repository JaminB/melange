// Offline self-test for the game-state readers (no game needed): the readers run against fake containers, data
// store descriptors, RTTI and a task table built in this process; plus the JSON forms and the Oasis parameters.
// Exit code 0 = all passed.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "game/state/gamestate_internal.h"
#include "game/state/gamestate_json.h"
#include "oasis/state_params.h"
#include "tools/json_read.h"

using namespace melange;
namespace gs = melange::gamestate;
namespace d = melange::gamestate::detail;

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

void ExpectEq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) printf("  %s: got \"%s\", want \"%s\"\n", what.c_str(), got.c_str(), want.c_str());
    Expect(got == want, what);
}

bool ValidJson(const std::string& s, json::Value* v = nullptr) {
    json::Value tmp;
    json::Error e;
    const bool ok = json::Parse(s, v ? v : &tmp, &e);
    if (!ok) printf("  bad JSON (%d:%d %s): %s\n", e.line, e.col, e.text.c_str(), s.c_str());
    return ok;
}

// Fake game memory: blocks that live for the whole run, so addresses stay valid.
struct Block {
    std::vector<uint8_t> bytes;
    explicit Block(size_t n) : bytes(n) {}
    uintptr_t At(size_t off = 0) { return reinterpret_cast<uintptr_t>(bytes.data()) + off; }
    template <class T> void Put(size_t off, T v) { memcpy(bytes.data() + off, &v, sizeof v); }
};
std::vector<Block*> g_blocks;
Block& New(size_t n) {
    g_blocks.push_back(new Block(n));
    return *g_blocks.back();
}
uintptr_t Str(const char* s) {
    Block& b = New(strlen(s) + 1);
    memcpy(b.bytes.data(), s, strlen(s) + 1);
    return b.At();
}

// A resource handle: +4 -> details; details +0x14 name, +0x1c value.
uintptr_t Descriptor(uintptr_t vt, const char* name, const void* value, size_t n) {
    Block& details = New(0x40);
    details.Put<uintptr_t>(0x14, name ? Str(name) : 0);
    memcpy(details.bytes.data() + 0x1c, value, n);
    Block& desc = New(8);
    desc.Put<uintptr_t>(0, vt);
    desc.Put<uintptr_t>(4, details.At());
    return desc.At();
}
template <class T> uintptr_t Descriptor(uintptr_t vt, const char* name, T v) { return Descriptor(vt, name, &v, sizeof v); }

constexpr uintptr_t kInt = 0x887b74, kUint = 0x887bbc, kFloat = 0x887c04, kVector = 0x887c4c, kString = 0x887cdc,
                    kContainer = 0x887d74, kStringTable = 0x887d24, kColor = 0x887c94;

// MSVC RTTI for a fake class: returns a vtable whose [-1] is a complete object locator.
uintptr_t FakeClass(const char* raw, std::vector<const char*> bases = {}) {
    auto td = [](const char* n) {
        Block& b = New(8 + strlen(n) + 1);
        memcpy(b.bytes.data() + 8, n, strlen(n) + 1);
        return b.At();
    };
    bases.insert(bases.begin(), raw);
    Block& bca = New(4 * bases.size());
    for (size_t i = 0; i < bases.size(); ++i) {
        Block& bcd = New(0x1c);
        bcd.Put<uintptr_t>(0, td(bases[i]));
        bca.Put<uintptr_t>(4 * i, bcd.At());
    }
    Block& chd = New(16);
    chd.Put<uint32_t>(8, static_cast<uint32_t>(bases.size()));
    chd.Put<uintptr_t>(12, bca.At());
    Block& col = New(20);
    col.Put<uintptr_t>(12, td(raw));
    col.Put<uintptr_t>(16, chd.At());
    Block& vt = New(64);
    vt.Put<uintptr_t>(0, col.At());
    return vt.At(4);
}

// ---------------------------------------------------------------- the fake engine
std::map<std::string, uintptr_t> g_resources;
std::map<std::string, int32_t> g_ints;
std::vector<uintptr_t> g_enum;
int g_gets = 0, g_releases = 0;

bool FakeGetInt(const char* name, int32_t* out) {
    auto it = g_ints.find(name);
    if (it == g_ints.end()) return false;
    *out = it->second;
    return true;
}
uintptr_t FakeGetResource(const char* name) {
    auto it = g_resources.find(name);
    if (it == g_resources.end()) return 0;
    ++g_gets;
    return it->second;
}
void FakeRelease(uintptr_t) { ++g_releases; }
bool FakeEnumerate(d::EnumCb cb, void* ctx) {
    for (uintptr_t x : g_enum)
        if (!cb(x, ctx)) break;
    return true;
}
constexpr d::Engine kEngine{&FakeGetInt, &FakeGetResource, &FakeRelease, &FakeEnumerate};

// ---------------------------------------------------------------- tests
void TestText() {
    char out[32];
    d::Utf8("Worm \xC3\xBC", out, sizeof out);
    ExpectEq(out, "Worm \xC3\xBC", "valid UTF-8 kept");
    d::Utf8("Ren\xE9 \x80", out, sizeof out);
    ExpectEq(out, "Ren\xC3\xA9 \xE2\x82\xAC", "Windows-1252 converted");
    d::Utf8("\x81", out, sizeof out);
    ExpectEq(out, "\xEF\xBF\xBD", "undefined Windows-1252 byte -> U+FFFD");
    d::Utf8("ab\xC3\xA9", out, 4);
    ExpectEq(out, "ab", "truncated on a character boundary");
    d::Utf8(std::string_view("ab\0cd", 5), out, sizeof out);
    ExpectEq(out, "ab", "stops at NUL");

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize;
    auto* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    DWORD old;
    VirtualProtect(mem + page, page, PAGE_NOACCESS, &old);
    memcpy(mem + page - 4, "abc", 4);
    char s[64];
    d::ReadCString(reinterpret_cast<uintptr_t>(mem + page - 4), s, sizeof s);
    ExpectEq(s, "abc", "string ending at a page end");
    memset(mem + page - 8, 'x', 8);
    d::ReadCString(reinterpret_cast<uintptr_t>(mem + page - 8), s, sizeof s);
    ExpectEq(s, "xxxxxxxx", "unterminated string stops at the unreadable page");
    d::ReadCString(0, s, sizeof s);
    ExpectEq(s, "", "null pointer");

    uint8_t buf[4096];
    Expect(d::PeekGuarded(reinterpret_cast<uintptr_t>(mem), buf, 16), "peek readable");
    Expect(!d::PeekGuarded(reinterpret_cast<uintptr_t>(mem + page - 8), buf, 16), "peek into a no-access page fails");
    Expect(!d::PeekGuarded(reinterpret_cast<uintptr_t>(mem), buf, 4097), "peek over 4096 bytes refused");
    Expect(!d::PeekGuarded(0, buf, 4), "peek at null fails");
    Expect(!d::PeekGuarded(0xfffffff0u, buf, 32), "peek that wraps fails");
    VirtualProtect(mem + page, page, PAGE_READWRITE | PAGE_GUARD, &old);
    Expect(!d::PeekGuarded(reinterpret_cast<uintptr_t>(mem + page), buf, 16), "peek at a guard page fails");
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(mem + page, &mbi, sizeof mbi);
    Expect((mbi.Protect & PAGE_GUARD) != 0, "the guard page is left armed");
    VirtualFree(mem, 0, MEM_RELEASE);
    Expect(!d::Copy(reinterpret_cast<uintptr_t>(mem), buf, 4), "copy from freed memory fails without a crash");
}

struct World {
    d::Layout layout{};
    uintptr_t wormVt = 0x7770001, teamVt = 0x7770002;
    Block* worms[16] = {};
    Block* teams[4] = {};
};

Block& Container(uintptr_t handles, int i, uintptr_t vt, size_t size) {
    Block& c = New(size);
    c.Put<uintptr_t>(0, vt);
    Block& inner = New(0x20);
    inner.Put<uintptr_t>(0x1c, c.At());
    Block& h = New(8);
    h.Put<uintptr_t>(4, inner.At());
    const uintptr_t hp = h.At();
    memcpy(reinterpret_cast<void*>(handles + 4 * i), &hp, 4);
    return c;
}

World MakeWorld() {
    World w;
    Block& wh = New(64);
    Block& th = New(16);
    w.layout.wormHandles = wh.At();
    w.layout.teamHandles = th.At();
    w.layout.wormVt = w.wormVt;
    w.layout.teamVt = w.teamVt;
    auto worm = [&](int slot, const char* name, bool active, uint8_t team, uint16_t energy, uint32_t phys, int32_t weapon) {
        Block& c = Container(wh.At(), slot, w.wormVt, 0x164);
        c.Put<uintptr_t>(0xb0, name ? Str(name) : 0);
        c.Put<uint8_t>(0x124, active);
        c.Put<uint8_t>(0x127, team);
        c.Put<uint8_t>(0x128, static_cast<uint8_t>(slot % 4));
        c.Put<uint16_t>(0x11e, energy);
        c.Put<uint32_t>(0xf0, phys);
        c.Put<int32_t>(0xf4, weapon);
        const float pos[3] = {10.f * slot, 20.f, -5.f}, vel[3] = {1.f, 0.f, 0.5f};
        memcpy(&c.bytes[0x38], pos, 12);
        memcpy(&c.bytes[0x50], vel, 12);
        c.Put<float>(0x90, slot == 0 ? 6.25f : slot == 1 ? std::numeric_limits<float>::quiet_NaN() : 0.f);
        w.worms[slot] = &c;
    };
    worm(0, "Paul", true, 0, 100, 6, 1);
    worm(1, "Chani", true, 1, 54, 8, 67);
    worm(2, "", false, 0, 0, 8, 0);         // unused slot: skipped
    worm(3, "Stilgar", false, 1, 0, 7, 2);  // dead, still named
    Block& wrong = Container(wh.At(), 4, 0x1234, 0x164);  // another class: skipped
    wrong.Put<uint8_t>(0x124, 1);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    w.worms[0]->Put<float>(0x3c, nan);
    auto team = [&](int slot, const char* name, bool active, bool ai) {
        Block& c = Container(th.At(), slot, w.teamVt, 0x78);
        c.Put<uintptr_t>(0x14, name ? Str(name) : 0);
        c.Put<uint8_t>(0x68, active);
        c.Put<uint8_t>(0x6a, static_cast<uint8_t>(slot + 2));
        c.Put<uint8_t>(0x6c, static_cast<uint8_t>(slot));
        c.Put<uint8_t>(0x6e, ai);
        c.Put<uint8_t>(0x6f, 1);
        c.Put<uint8_t>(0x75, !ai);
        c.Put<uint32_t>(0x48, 1234u * static_cast<uint32_t>(slot + 1));
        w.teams[slot] = &c;
    };
    team(0, "Atreides", true, false);
    team(1, "Fremen \xE9", true, true);
    team(2, "", false, false);
    return w;
}

void TestSnapshot() {
    World w = MakeWorld();
    g_ints = {{"CurrentTeamIndex", 1}, {"ActiveWormIndex", 7}};
    g_resources = {
        {"TurnTime", Descriptor(kInt, "TurnTime", 45000)},
        {"TurnTimeRemaining", Descriptor(kInt, "TurnTimeRemaining", 12340)},
        {"RoundTime", Descriptor(kInt, "RoundTime", 1800000)},
        {"RoundTimeRemaining", Descriptor(kInt, "RoundTimeRemaining", 900000)},
        {"Wind.Speed", Descriptor(kFloat, "Wind.Speed", 4.5e-5f)},
        {"Wind.Direction", Descriptor(kFloat, "Wind.Direction", 3.14f)},
        {"Water.Level", Descriptor(kInt, "Water.Level", 7)},  // wrong type: not read
        {"Land.Theme", Descriptor(kString, "Land.Theme", Str("ARABIAN"))},
    };
    g_gets = g_releases = 0;
    gs::Snapshot s{};
    s.frame = 42;
    d::FillSnapshot(w.layout, kEngine, d::MatchInfo{true, true, false, 3, 6}, &s);
    Expect(s.frame == 42 && s.matchSerial == 3, "frame and match serial");
    Expect(s.match.inMatch && s.match.online && s.match.turnsStarted == 6 && !s.match.suddenDeath, "match info");
    Expect(s.match.currentTeam == 1 && s.match.activeWorm == 7, "current team and active worm");
    Expect(s.match.turnMs == 45000 && s.match.turnMsLeft == 12340 && s.match.roundMs == 1800000 && s.match.roundMsLeft == 900000,
           "timers");
    Expect(std::fabs(s.match.windSpeed - 4.5e-5f) < 1e-9f && std::fabs(s.match.windDir - 3.14f) < 1e-6f, "wind");
    Expect(s.match.waterLevel == 0.f, "a value of the wrong type is not read");
    ExpectEq(s.match.theme, "ARABIAN", "theme");
    Expect(g_gets == g_releases && g_gets == 8, "every descriptor taken is released");

    Expect(s.teamCount == 2, "two teams (the empty inactive slot skipped)");
    ExpectEq(s.teams[1].name, "Fremen \xC3\xA9", "team name converted to UTF-8");
    Expect(s.teams[1].slot == 1 && s.teams[1].ai && !s.teams[1].local && s.teams[1].colour == 3 && s.teams[1].alliance == 1 &&
               s.teams[1].roundsWon == 1 && s.teams[1].score == 2468,
           "team fields");
    Expect(s.wormCount == 3, "three worms (unused slot and wrong class skipped)");
    const gs::Worm& a = s.worms[0];
    Expect(a.slot == 0 && a.alive && a.health == 100 && a.physicsState == 6 && a.weapon == 1, "worm 0 fields");
    Expect(a.pos.x == 0.f && a.pos.y == 0.f && a.pos.z == -5.f && a.vel.z == 0.5f, "non-finite coordinate reads as 0");
    Expect(a.yaw == 6.25f, "yaw read from +0x90, unwrapped");
    const gs::Worm& b = s.worms[1];
    Expect(b.slot == 1 && b.active && !b.alive && b.physicsState == 8 && b.weapon == -1 && b.health == 54,
           "drowning worm is not alive; weapon 67 is none");
    Expect(b.yaw == 0.f, "non-finite yaw reads as 0");
    Expect(s.worms[2].slot == 3 && !s.worms[2].active && !s.worms[2].alive, "dead named worm kept");

    g_ints["CurrentTeamIndex"] = 9;
    g_ints["ActiveWormIndex"] = -1;
    d::FillSnapshot(w.layout, kEngine, d::MatchInfo{true, false, false, 3, 6}, &s);
    Expect(s.match.currentTeam == -1 && s.match.activeWorm == -1, "out-of-range indices read as none");

    d::FillSnapshot(w.layout, kEngine, d::MatchInfo{false, false, false, 4, 0}, &s);
    Expect(!s.match.inMatch && s.wormCount == 0 && s.teamCount == 0 && s.match.activeWorm == -1 && !s.match.theme[0],
           "outside a match: no worms, no teams, no match values");

    d::FillSnapshot(w.layout, d::Engine{}, d::MatchInfo{true, false, true, 5, 1}, &s);
    Expect(s.wormCount == 3 && s.match.turnMs == 0 && s.match.suddenDeath, "no engine calls: containers still read");

    const std::string js = gs::wire::SnapshotJson(s, true);
    json::Value v;
    Expect(ValidJson(js, &v), "snapshot JSON parses");
    const json::Value* worms = v.Get("worms");
    Expect(worms && worms->IsArray() && worms->items.size() == 3, "snapshot JSON worms");
    const json::Value* pos = worms && !worms->items.empty() ? worms->items[0].Get("pos") : nullptr;
    Expect(pos && pos->Get("z") && pos->Get("z")->number == -5.0, "worm pos as {x,y,z}");
    const json::Value* m = v.Get("match");
    Expect(m && m->Get("suddenDeath") && m->Get("suddenDeath")->boolean && m->Get("activeWorm")->number == -1,
           "snapshot JSON match");
    Expect(v.Get("available") && v.Get("available")->boolean, "snapshot JSON available");
    Expect(m && m->Get("attract") && !m->Get("attract")->boolean, "snapshot JSON attract defaults to false");
    json::Value demo;
    Expect(ValidJson(gs::wire::SnapshotJson(s, true, true), &demo) && demo.Get("match")->Get("attract")->boolean,
           "snapshot JSON reports the attract demo");
    gs::Snapshot odd{};
    odd.match.windSpeed = std::numeric_limits<float>::infinity();
    Expect(gs::wire::SnapshotJson(odd, false).find("\"windSpeed\":null") != std::string::npos, "non-finite as null");
}

void TestVars() {
    const float vec[3] = {1.f, -2.5f, std::numeric_limits<float>::quiet_NaN()};
    const uintptr_t containerVt = FakeClass(".?AVWormDataContainer@@", {".?AVXContainer@@"});
    Block& cont = New(16);
    cont.Put<uintptr_t>(0, containerVt);
    std::string longText(300, 'q');
    longText[5] = '"';
    longText[6] = '\n';
    g_enum = {
        Descriptor(kInt, "ActiveWormIndex", -1),
        Descriptor(kUint, "AllianceCount", 2u),
        Descriptor(kFloat, "Gravity", -0.00025f),
        Descriptor(kFloat, "Bad.Float", std::numeric_limits<float>::infinity()),
        Descriptor(kVector, "Land.MinBounds", vec, sizeof vec),
        Descriptor(kString, "Land.Theme", Str("ARABIAN")),
        Descriptor(kString, "Long.Text", Str(longText.c_str())),
        Descriptor(kString, "Latin.Text", Str("caf\xE9")),
        Descriptor(kContainer, "Worm.Data00", cont.At()),
        Descriptor(kContainer, "Worm.Data01", uintptr_t{0}),
        Descriptor(kColor, "Team.Colour", 0xff102030u),
        Descriptor(kStringTable, "Some.Table", 0),
        Descriptor(0x1234, "Mystery", 0),
        Descriptor(kInt, nullptr, 5),
    };
    std::vector<gs::Var> vars(32);
    const int n = d::EnumerateVars(kEngine, vars.data(), 32, nullptr);
    Expect(n == 13, "every named descriptor enumerated (" + std::to_string(n) + ")");
    std::map<std::string, gs::Var> by;
    for (int i = 0; i < n && i < 32; ++i) by[vars[i].name] = vars[i];
    ExpectEq(by["ActiveWormIndex"].value, "-1", "Int");
    Expect(by["AllianceCount"].type == gs::VarType::Uint, "Uint type");
    ExpectEq(by["AllianceCount"].value, "2", "Uint");
    ExpectEq(by["Gravity"].value, "-0.000250000012", "Float");
    ExpectEq(by["Bad.Float"].value, "null", "non-finite Float");
    ExpectEq(by["Land.MinBounds"].value, "[1,-2.5,null]", "Vector");
    ExpectEq(by["Land.Theme"].value, "\"ARABIAN\"", "String");
    ExpectEq(by["Latin.Text"].value, "\"caf\xC3\xA9\"", "String as UTF-8");
    ExpectEq(by["Worm.Data00"].value, "{\"addr\":" + std::to_string(cont.At()) + ",\"class\":\"WormDataContainer\"}", "Container");
    ExpectEq(by["Worm.Data01"].value, "null", "null Container");
    ExpectEq(by["Team.Colour"].value, "\"#ff102030\"", "Color");
    Expect(by["Some.Table"].type == gs::VarType::StringTable, "StringTable type");
    ExpectEq(by["Some.Table"].value, "null", "StringTable value");
    Expect(by["Mystery"].type == gs::VarType::Undefined, "unknown descriptor class is Undefined");
    const std::string lt = by["Long.Text"].value;
    Expect(lt.size() < 96 && lt.front() == '"' && lt.back() == '"', "long string truncated inside the value");
    for (int i = 0; i < n && i < 32; ++i) Expect(ValidJson(gs::wire::VarJson(vars[i])), std::string("VarJson ") + vars[i].name);

    Expect(d::EnumerateVars(kEngine, vars.data(), 32, "Land.") == 2, "prefix filter");
    Expect(d::EnumerateVars(kEngine, vars.data(), 3, nullptr) == 13, "total beyond max");
    Expect(d::EnumerateVars(kEngine, nullptr, 0, "Worm.") == 2, "count only");
    Expect(d::EnumerateVars(d::Engine{}, vars.data(), 32, nullptr) == -1, "no store");

    g_resources = {{"Gravity", g_enum[2]}};
    g_gets = g_releases = 0;
    gs::Var one{};
    Expect(d::ReadVar1(kEngine, "Gravity", &one) && one.type == gs::VarType::Float, "Var1");
    Expect(!d::ReadVar1(kEngine, "Missing", &one), "Var1 missing");
    Expect(g_gets == 1 && g_releases == 1, "Var1 releases its descriptor");
}

void TestRtti() {
    ExpectEq(d::Demangle(".?AVFoo@@"), "Foo", "demangle");
    ExpectEq(d::Demangle(".?AUIUnknown@@"), "IUnknown", "demangle struct");
    ExpectEq(d::Demangle(".?AVOnHeap@XOM@@"), "XOM::OnHeap", "demangle namespace");
    ExpectEq(d::Demangle(".?AV?$XomObject@UIXSerializable@@UOnHeap@XOM@@@@"), "?$XomObject@UIXSerializable@@UOnHeap@XOM@@",
             "templates stay raw");
    ExpectEq(d::Demangle("junk"), "junk", "not a type name");
    char name[48];
    bool payload = true;
    Expect(!d::Rtti(0, name, sizeof name, &payload) && !payload, "no vtable");
    Block& junk = New(16);
    Expect(!d::Rtti(junk.At(4), name, sizeof name, &payload), "no locator");
    const uintptr_t vt = FakeClass(".?AVParabolicPayloadLogicEntity@@", {".?AVPayloadLogicEntity@@", ".?AVLogicEntity@@"});
    Expect(d::Rtti(vt, name, sizeof name, &payload) && payload, "payload subclass detected");
    ExpectEq(name, "ParabolicPayloadLogicEntity", "RTTI name");
    d::SetRttiRange(vt - 4, vt);
    Expect(!d::Rtti(vt, name, sizeof name, &payload), "RTTI outside the allowed range is not followed");
    d::SetRttiRange(vt, vt + 64);
    Expect(!d::Rtti(vt, name, sizeof name, &payload), "the locator slot must be inside the range too");
    d::SetRttiRange(0, UINTPTR_MAX);
    Expect(d::Rtti(vt, name, sizeof name, &payload), "full range again");
}

void TestEntities() {
    World w = MakeWorld();
    const uintptr_t vtWorm = FakeClass(".?AVWXWormLogicEntity@@", {".?AVLogicEntity@@"});
    const uintptr_t vtShell = FakeClass(".?AVParabolicPayloadLogicEntity@@", {".?AVPayloadLogicEntity@@"});
    const uintptr_t vtCrate = FakeClass(".?AVCrateLogicEntity@@", {".?AVLogicEntity@@"});
    const uintptr_t vtDrum = FakeClass(".?AVOilDrumLogicEntity@@", {".?AVLogicEntity@@"});
    const uintptr_t vtTimer = FakeClass(".?AVTimerLogicEntity@@", {".?AVLogicEntity@@"});
    Block& nortti = New(16);

    auto obj = [&](uintptr_t vt, size_t size) -> Block& {
        Block& b = New(size);
        b.Put<uintptr_t>(0, vt);
        return b;
    };
    Block& worm = obj(vtWorm, 0x40);
    worm.Put<uint8_t>(0x30, 1);
    Block& shell = obj(vtShell, 0xd0);
    const float sp[3] = {1.f, 2.f, 3.f}, sv[3] = {0.1f, 0.2f, 0.3f};
    memcpy(&shell.bytes[0x28], sp, 12);
    memcpy(&shell.bytes[0x34], sv, 12);
    shell.Put<uintptr_t>(0xc4, Descriptor(kString, "kWeaponBazooka", 0));
    Block& crate = obj(vtCrate, 0x40);
    const float cp[3] = {5.f, 6.f, 7.f};
    memcpy(&crate.bytes[0x2c], cp, 12);
    Block& drum = obj(vtDrum, 0x70);
    const float distant[3] = {1e9f, 0.f, 0.f};
    memcpy(&drum.bytes[0x20], distant, 12);
    Block& timer = obj(vtTimer, 0x10);
    Block& raw = obj(nortti.At(4), 0x10);

    constexpr uint16_t cap = 12;
    Block& entries = New(cap * 0x24);
    auto entry = [&](uint16_t i, uintptr_t o, uint32_t serial, uint16_t freeFlag = 0, uint16_t idx = 0xffff) {
        const size_t e = static_cast<size_t>(i) * 0x24;
        entries.Put<uint16_t>(e + 8, freeFlag);
        entries.Put<uintptr_t>(e + 0xc, o);
        entries.Put<uint32_t>(e + 0x14, (idx == 0xffff ? i : idx) | serial << 12);
    };
    entry(1, worm.At(), 3);
    entry(2, shell.At(), 5);
    entry(3, crate.At(), 1);
    entry(4, drum.At(), 1);
    entry(5, timer.At(), 1);
    entry(6, raw.At(), 1);
    entry(7, timer.At(), 1, 1);     // free
    entry(8, timer.At(), 1, 0, 9);  // stale handle
    entry(9, 0, 1);                 // no object
    Block& tbl = New(0x18);
    tbl.Put<uintptr_t>(0, entries.At());
    tbl.Put<uint16_t>(0x16, cap);
    Block& tm = New(0x20);
    tm.Put<uintptr_t>(0x1c, tbl.At());
    Block& global = New(4);
    global.Put<uintptr_t>(0, tm.At());
    w.layout.taskManager = global.At();

    gs::Entity out[16] = {};
    const int n = d::WalkEntities(w.layout, out, 16);
    Expect(n == 6, "six live entities (" + std::to_string(n) + ")");
    Expect(out[0].kind == gs::EntityKind::Worm && out[0].handle == (1u | 3u << 12) && out[0].hasPos && out[0].pos.x == 10.f,
           "worm entity takes its container's position");
    ExpectEq(out[0].label, "Chani", "worm entity label");
    Expect(out[1].kind == gs::EntityKind::Projectile && out[1].hasPos && out[1].pos.z == 3.f && out[1].vel.y == 0.2f,
           "projectile position and velocity");
    ExpectEq(out[1].label, "Bazooka", "projectile weapon name");
    ExpectEq(out[1].type, "ParabolicPayloadLogicEntity", "projectile type");
    Expect(out[2].kind == gs::EntityKind::Crate && out[2].hasPos && out[2].pos.y == 6.f, "crate");
    Expect(out[3].kind == gs::EntityKind::Barrel && !out[3].hasPos, "implausible position hidden");
    Expect(out[4].kind == gs::EntityKind::Other && !out[4].hasPos, "other entity");
    ExpectEq(out[4].type, "TimerLogicEntity", "other entity type");
    char want[48];
    snprintf(want, sizeof want, "vtbl:0x%08x", static_cast<unsigned>(nortti.At(4)));
    ExpectEq(out[5].type, want, "entity without RTTI named by vtable");
    Expect(out[5].object == raw.At() && out[5].vtable == nortti.At(4), "object and vtable");

    gs::Entity two[2] = {};
    Expect(d::WalkEntities(w.layout, two, 2) == 6 && two[1].kind == gs::EntityKind::Projectile, "total beyond max");
    tbl.Put<uint16_t>(0x16, 5000);
    Expect(d::WalkEntities(w.layout, out, 16) == 0, "implausible capacity refused");
    tm.Put<uintptr_t>(0x1c, 0);
    Expect(d::WalkEntities(w.layout, out, 16) == 0, "no table");

    json::Value v;
    const std::string all = gs::wire::EntitiesJson(out, 6);
    Expect(ValidJson(all, &v) && v.items.size() == 6, "entities JSON");
    const std::string few = gs::wire::EntitiesJson(out, 6, gs::wire::KindBit(gs::EntityKind::Crate) | gs::wire::KindBit(gs::EntityKind::Worm));
    Expect(ValidJson(few, &v) && v.items.size() == 2, "entities JSON filtered by kind");
    Expect(v.items.size() == 2 && v.items[0].Get("kind")->string == "Worm" && v.items[0].Get("pos")->IsObject(), "entity JSON members");
    Expect(ValidJson(gs::wire::EntityJson(out[3]), &v) && v.Get("pos")->IsNull(), "entity without a position");
}

void TestParams() {
    namespace sp = melange::oasis::stateparams;
    auto f = sp::ParseFilter("{}", 5, 10);
    Expect(f.hz == 5 && f.kinds == gs::wire::kAllKinds, "filter defaults");
    f = sp::ParseFilter("{\"hz\":0}", 5, 10);
    Expect(f.hz == 1, "hz clamped up");
    f = sp::ParseFilter("{\"hz\":99}", 2, 5);
    Expect(f.hz == 5, "hz clamped down");
    f = sp::ParseFilter("{\"hz\":3.7,\"kinds\":[\"Crate\",\"nope\",7,\"Projectile\"]}", 2, 5);
    Expect(f.hz == 3 && f.kinds == (gs::wire::KindBit(gs::EntityKind::Crate) | gs::wire::KindBit(gs::EntityKind::Projectile)),
           "hz and kinds");
    f = sp::ParseFilter("not json", 4, 10);
    Expect(f.hz == 4 && f.kinds == gs::wire::kAllKinds, "malformed filter");

    auto q = sp::ParseInspect("{\"handle\":4101}");
    Expect(q.error.empty() && q.byHandle && q.handle == 4101 && q.len == 256, "inspect by handle");
    q = sp::ParseInspect("{\"addr\":\"0x95B4A8\",\"len\":16}");
    Expect(q.error.empty() && !q.byHandle && q.addr == 0x95b4a8 && q.len == 16, "inspect by hex address");
    q = sp::ParseInspect("{\"addr\":9811112}");
    Expect(q.error.empty() && q.addr == 9811112, "inspect by numeric address");
    for (const char* bad : {"{}", "{\"handle\":1,\"addr\":2}", "{\"addr\":0}", "{\"addr\":\"95b4a8\"}", "{\"addr\":\"0xzz\"}",
                            "{\"addr\":-4}", "{\"addr\":1,\"len\":0}", "{\"addr\":1,\"len\":4097}", "{\"handle\":-1}",
                            "{\"addr\":\"0xfffffff0\",\"len\":64}", "[]"})
        Expect(!sp::ParseInspect(bad).error.empty(), std::string("inspect rejects ") + bad);

    std::string p;
    Expect(sp::ParsePrefix("{}", &p) && p.empty(), "no prefix");
    Expect(sp::ParsePrefix("{\"prefix\":\"Wind.\"}", &p) && p == "Wind.", "prefix");
    Expect(!sp::ParsePrefix("{\"prefix\":5}", &p), "prefix must be a string");
    Expect(!sp::ParsePrefix(("{\"prefix\":\"" + std::string(64, 'a') + "\"}").c_str(), &p), "prefix length");

    const uint8_t bytes[4] = {0x00, 0xab, 0x7f, 0xff}, ok[4] = {1, 1, 0, 1};
    ExpectEq(sp::Hex(bytes, ok, 4), "00ab??ff", "hex with unreadable bytes");

    gs::EntityKind k;
    Expect(gs::wire::KindFromName("Barrel", &k) && k == gs::EntityKind::Barrel && !gs::wire::KindFromName("barrel", &k), "kind names");
    ExpectEq(gs::wire::TypeName(gs::VarType::StringTable), "StringTable", "type names");
}

// A fake engine sweep: land fills y <= 0 and reports a raw normal of (0, 3, 0), like the engine's unnormalised one.
gs::Vec3 g_sweepStep{};
int g_sweepTicks = 0, g_sweeps = 0;
gs::Vec3 g_sweepNormal{0.f, 3.f, 0.f};
float g_sweepTimeBias = 0.f;
bool FloorSweep(const gs::Vec3& o, const gs::Vec3& step, int ticks, d::RawLandHit* out) {
    ++g_sweeps;
    g_sweepStep = step;
    g_sweepTicks = ticks;
    *out = d::RawLandHit{};
    float k;
    if (o.y <= 0.f) k = 0.f;
    else if (step.y < 0.f) k = -o.y / step.y;
    else return true;
    k += g_sweepTimeBias;
    if (k > static_cast<float>(ticks) + 1.f) return true;  // the engine gives up one step past the end
    *out = d::RawLandHit{true, k, g_sweepNormal};
    return true;
}
bool FailedSweep(const gs::Vec3&, const gs::Vec3&, int, d::RawLandHit*) { return false; }

bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void TestLandRay() {
    gs::LandHit h{};
    using R = gs::LandRayResult;
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    g_sweeps = 0;
    Expect(d::SegmentLandRay(&FloorSweep, {0, nan, 0}, {0, -1, 0}, &h) == R::Invalid, "NaN start is invalid");
    Expect(d::SegmentLandRay(&FloorSweep, {0, 1, 0}, {inf, -1, 0}, &h) == R::Invalid, "infinite end is invalid");
    Expect(d::SegmentLandRay(&FloorSweep, {2e6f, 1, 0}, {0, -1, 0}, &h) == R::Invalid, "a point beyond 1e6 is invalid");
    Expect(d::SegmentLandRay(&FloorSweep, {0, 1, 0}, {0, -1, 0}, nullptr) == R::Invalid, "no output is invalid");
    Expect(d::SegmentLandRay(&FloorSweep, {5, -3, 5}, {5, -3, 5}, &h) == R::Miss, "a zero-length segment never hits");
    Expect(g_sweeps == 0, "rejected segments never reach the engine");

    Expect(d::SegmentLandRay(&FloorSweep, {0, 10, 0}, {0, -30, 0}, &h) == R::Hit, "a ray into the floor hits");
    Expect(g_sweepTicks == d::kLandRayTicks && Near(g_sweepStep.y, -0.04f, 1e-6f), "the engine's 1000 steps");
    Expect(Near(h.t, 0.25f), "t along the segment");
    Expect(h.normal.x == 0.f && Near(h.normal.y, 1.f) && h.normal.z == 0.f, "the normal is normalised");
    Expect(d::SegmentLandRay(&FloorSweep, {0, -5, 0}, {0, -6, 0}, &h) == R::Hit && h.t == 0.f, "a start inside land hits at 0");
    Expect(d::SegmentLandRay(&FloorSweep, {0, 10, 0}, {0, 20, 0}, &h) == R::Miss, "a ray away from the floor misses");
    Expect(d::SegmentLandRay(&FloorSweep, {0, 10, 0}, {0, 0.5f, 0}, &h) == R::Miss, "a ray that stops short misses");
    g_sweepTimeBias = 0.5f;
    Expect(d::SegmentLandRay(&FloorSweep, {0, 10, 0}, {0, 0.f, 0}, &h) == R::Miss,
           "a hit the engine reports past the end of the segment is a miss");
    g_sweepTimeBias = 0.f;

    // Long segments: only the first 4096 units are swept; t stays relative to the whole segment.
    Expect(d::SegmentLandRay(&FloorSweep, {0, 1000, 0}, {0, -9000, 0}, &h) == R::Hit, "a long ray hits");
    Expect(Near(g_sweepStep.y, -gs::kLandRayMaxLength / d::kLandRayTicks, 1e-4f), "a long ray is swept over 4096 units");
    Expect(Near(h.t, 0.1f), "t of a long ray is relative to the whole segment");
    Expect(d::SegmentLandRay(&FloorSweep, {0, 5000, 0}, {0, -5000, 0}, &h) == R::Miss, "land beyond 4096 units is not seen");

    g_sweepNormal = {0.f, 0.f, 0.f};
    const float len = std::sqrt(425.f);  // the segment below is (3, -20, 4)
    Expect(d::SegmentLandRay(&FloorSweep, {0, 10, 0}, {3, -10, 4}, &h) == R::Hit && Near(h.normal.x, -3.f / len) &&
               Near(h.normal.y, 20.f / len) && Near(h.normal.z, -4.f / len),
           "no usable normal: it faces the ray");
    g_sweepNormal = {nan, 1.f, 0.f};
    Expect(d::SegmentLandRay(&FloorSweep, {0, 10, 0}, {0, -10, 0}, &h) == R::Hit && Near(h.normal.y, 1.f),
           "a NaN normal also faces the ray");
    g_sweepNormal = {0.f, 3.f, 0.f};
    Expect(d::SegmentLandRay(&FailedSweep, {0, 10, 0}, {0, -10, 0}, &h) == R::Unavailable, "a failed sweep is unavailable");
    Expect(d::SegmentLandRay(nullptr, {0, 10, 0}, {0, -10, 0}, &h) == R::Unavailable, "no sweep is unavailable");

    d::FrameBudget b;
    int taken = 0;
    for (int i = 0; i < 300; ++i) taken += b.Take(7, gs::kLandRaysPerFrame);
    Expect(taken == gs::kLandRaysPerFrame, "the per-frame budget caps the calls");
    Expect(b.Take(8, gs::kLandRaysPerFrame), "the budget refills on the next frame");
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::pair<const char*, void (*)()> tests[] = {{"text", TestText},         {"snapshot", TestSnapshot},
                                                         {"vars", TestVars},         {"rtti", TestRtti},
                                                         {"entities", TestEntities}, {"params", TestParams},
                                                         {"landray", TestLandRay}};
    for (const auto& [name, fn] : tests) {
        const int before = g_fail;
        fn();
        printf("  %-10s %s\n", name, g_fail == before ? "ok" : "FAILED");
    }
    for (Block* b : g_blocks) delete b;
    printf("gamestate_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
