// Offline self-test for the replay player's engine-free parts: record payloads, loading a .wsr for replay, the arm
// checks, forced seeds and draws, the input schedule, the per-tick compare, the virtual scheduler clock and the setup
// fingerprint, plus a record/replay round trip against a toy simulation. Exit code 0 = all passed.
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "wormsign/format.h"
#include "wormsign/hash_engine.h"
#include "wormsign/recording.h"
#include "wormsign/records.h"
#include "wormsign/replay_core.h"
#include "wormsign/setup.h"

using namespace melange::wormsign;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what);
    }
}

TickHash MakeTick(uint32_t tick, uint64_t salt) {
    TickHash h{};
    h.tick = tick;
    for (int i = 0; i < kEngineComps; ++i) h.c[i] = FnvV(salt + i, FnvV(tick));
    h.engine = Fnv(h.c, sizeof h.c);
    h.mods = salt & 1 ? FnvV(salt) : 0;
    h.rngLogic = tick * 7u;
    h.rng2 = tick * 13u;
    h.fpucw = 0x027f;
    h.inputs = static_cast<uint16_t>(tick % 3);
    return h;
}

bool SameTick(const TickHash& a, const TickHash& b) {
    return a.tick == b.tick && a.engine == b.engine && a.mods == b.mods && !memcmp(a.c, b.c, sizeof a.c) &&
           a.rngLogic == b.rngLogic && a.rng2 == b.rng2 && a.fpucw == b.fpucw && a.inputs == b.inputs;
}

// ---------------------------------------------------------------- record payloads
void Records() {
    std::vector<uint8_t> b;
    const rec::Seed s1{0, 0x12345678, 0x4ee250, 100}, s2{1, 0x9abcdef0, 0x4ee256, 100};
    rec::AppendSeed(b, s1);
    rec::AppendSeed(b, s2);
    Expect(b.size() == 2 * rec::kSeedBytes, "seed record size");
    std::vector<rec::Seed> seeds;
    Expect(rec::DecodeSeeds(b.data(), b.size(), &seeds) && seeds.size() == 2, "seeds decode");
    Expect(seeds[1].kind == 1 && seeds[1].value == 0x9abcdef0 && seeds[1].caller == 0x4ee256 && seeds[1].t == 100,
           "seed fields");
    seeds.clear();
    Expect(!rec::DecodeSeeds(b.data(), b.size() - 1, &seeds) && seeds.size() == 1, "truncated seeds reported");

    b.clear();
    rec::AppendDraw(b, rec::Draw{1, 0x50a45b, 0xdeadbeef, 0x3f000000});
    std::vector<rec::Draw> draws;
    Expect(rec::DecodeDraws(b.data(), b.size(), &draws) && draws.size() == 1 && draws[0].rng == 1 &&
               draws[0].ret == 0x50a45b && draws[0].stateAfter == 0xdeadbeef && draws[0].bits == 0x3f000000,
           "draw round trip");

    b.clear();
    rec::Input in{rec::kSendInt2, 0x1234, 7, 0xffffffff, 1040, 1020, 0x5058b0, ""};
    rec::AppendInput(b, in);
    rec::Input str{rec::kSendString, 0x77, 0, 0, 2000, 1990, 0x5057a0, "hello \xc3\xa9"};
    rec::AppendInput(b, str);
    rec::Input longStr{rec::kSendString, 0x78, 0, 0, 3000, 2990, 0x5057a0, std::string(300, 'x')};
    rec::AppendInput(b, longStr);
    Expect(b.size() == 3 * rec::kInputFixedBytes + 0 + str.str.size() + 255, "input sizes (string cut at 255)");
    std::vector<rec::Input> inputs;
    Expect(rec::DecodeInputs(b.data(), b.size(), &inputs) && inputs.size() == 3, "inputs decode");
    Expect(inputs[0].type == rec::kSendInt2 && inputs[0].id == 0x1234 && inputs[0].a == 7 && inputs[0].b == 0xffffffff &&
               inputs[0].time == 1040 && inputs[0].callT == 1020 && inputs[0].caller == 0x5058b0 && inputs[0].str.empty(),
           "input fields");
    Expect(inputs[1].str == str.str && inputs[2].str.size() == 255, "input strings");
    inputs.clear();
    Expect(!rec::DecodeInputs(b.data(), b.size() - 1, &inputs), "truncated input string reported");
    inputs.clear();
    Expect(!rec::DecodeInputs(b.data(), 10, &inputs), "truncated input header reported");

    rec::TickChunk tc;
    for (uint32_t t = 5; t < 10; ++t) tc.Add(MakeTick(t, 1));
    tc.Add(MakeTick(3, 1));  // behind: ignored
    for (uint32_t t = 14; t < 16; ++t) tc.Add(MakeTick(t, 1));
    Expect(tc.Count() == 7 && tc.FirstTick() == 5 && tc.LastTick() == 15, "tick chunk counts");
    const auto tb = tc.Take();
    Expect(tc.Empty() && tb.size() == 4 + 7 * (1 + rec::kTickBytes) + 5, "tick chunk size with one gap");
    std::vector<TickHash> got;
    Expect(rec::DecodeTicks(tb.data(), tb.size(), [&](const TickHash& h) { got.push_back(h); }), "ticks decode");
    bool same = got.size() == 7;
    for (size_t i = 0; same && i < got.size(); ++i) same = SameTick(got[i], MakeTick(i < 5 ? 5 + static_cast<uint32_t>(i) : 9 + static_cast<uint32_t>(i), 1));
    Expect(same, "ticks round trip with the gap");
    Expect(!rec::DecodeTicks(tb.data(), tb.size() - 3, [](const TickHash&) {}), "truncated tick reported");
    std::vector<uint8_t> badKind = tb;
    badKind[4] = 9;
    Expect(!rec::DecodeTicks(badKind.data(), badKind.size(), [](const TickHash&) {}), "unknown tick record kind");
}

// ---------------------------------------------------------------- recordings
struct Build {
    std::string head = R"({"format":1,"engineHash":1,"exeBuild":"1077","melange":"test","online":false,)"
                       R"("contentHash":"","contributors":[{"name":"mod.b.env","version":1},"mod.a.env@2"],"tickMs":20})";
    std::vector<rec::Seed> seeds;
    std::vector<rec::Draw> draws;
    std::vector<rec::Input> inputs;
    std::vector<TickHash> ticks;
    std::string setup;
    bool close = true;
};

std::vector<uint8_t> WriteWsr(const Build& b) {
    std::vector<uint8_t> out;
    wsr::Writer w;
    w.OpenMemory(&out);
    w.Chunk(wsr::kHEAD, b.head.data(), b.head.size());
    std::vector<uint8_t> p;
    for (const auto& s : b.seeds) rec::AppendSeed(p, s);
    w.Chunk(wsr::kSEED, p.data(), p.size());
    p.clear();
    for (const auto& d : b.draws) rec::AppendDraw(p, d);
    w.Chunk(wsr::kPDRW, p.data(), p.size());
    if (!b.setup.empty()) w.Chunk(wsr::kSETP, b.setup.data(), b.setup.size());
    rec::TickChunk tc;
    size_t nextInput = 0;
    for (const TickHash& h : b.ticks) {
        tc.Add(h);
        if (tc.Count() == 500) {
            const auto t = tc.Take();
            w.Chunk(wsr::kTICK, t.data(), t.size(), true, h.tick - 499, h.tick);
        }
        if (h.tick % 700 == 0 || &h == &b.ticks.back()) {
            p.clear();
            for (; nextInput < b.inputs.size() && (b.inputs[nextInput].callT <= h.tick * kTickMs || &h == &b.ticks.back());
                 ++nextInput)
                rec::AppendInput(p, b.inputs[nextInput]);
            if (!p.empty()) w.Chunk(wsr::kINPT, p.data(), p.size());
        }
    }
    if (!tc.Empty()) {
        const uint32_t from = tc.FirstTick(), to = tc.LastTick();
        const auto t = tc.Take();
        w.Chunk(wsr::kTICK, t.data(), t.size(), true, from, to);
    }
    if (b.close) w.Close();
    else w.Abandon();
    return out;
}

bool Load(const std::vector<uint8_t>& bytes, Recording* rec, std::string* err) {
    wsr::Reader r;
    if (!r.OpenMemory(bytes, err)) return false;
    return LoadRecording(r, rec, err);
}

Build Sample() {
    Build b;
    b.seeds = {{0, 111, 0x4ee250, 0}, {1, 111, 0x4ee256, 0}, {0, 222, 0x4ee250, 0}, {1, 222, 0x4ee256, 0}};
    b.draws = {{0, 0x4a44c9, 1, 10}, {1, 0x50a45b, 2, 20}, {0, 0x4a44c9, 3, 30}, {0, 0x4e9573, 4, 40}};
    for (uint32_t t = 1; t <= 1600; ++t) b.ticks.push_back(MakeTick(t, 2));
    b.ticks.erase(b.ticks.begin() + 999, b.ticks.begin() + 1004);  // ticks 1000..1004 missing
    for (uint32_t k = 0; k < 40; ++k) {
        const uint32_t callT = 30 + k * 700;
        b.inputs.push_back(rec::Input{static_cast<uint8_t>(k % 6), static_cast<uint16_t>(0x100 + k), k, k * 2,
                                      (callT / 20 + 1) * 20, callT, 0x5056b0, k % 6 == 5 ? "str" : ""});
    }
    setup::Data d;
    d.landFile = "cropcircle-w3d.xan";
    d.teams = {{"A", 4}, {"B", 4}};
    b.setup = setup::ToJson(d);
    return b;
}

void Recordings() {
    const Build b = Sample();
    Recording rec;
    std::string err;
    Expect(Load(WriteWsr(b), &rec, &err), "sample loads");
    Expect(rec.complete && rec.format == 1 && rec.engineHash == 1 && rec.exeBuild == "1077" && !rec.online,
           "header facts");
    Expect(rec.contributors == "mod.a.env@2,mod.b.env@1", "contributor key sorted, both spellings");
    Expect(rec.seeds.size() == 4 && rec.draws.size() == 4 && rec.inputs.size() == 40, "record counts");
    Expect(rec.firstTick == 1 && rec.lastTick == 1600 && rec.TickCount() == 1595, "tick range and gap");
    TickHash h;
    Expect(rec.Tick(999, &h) && SameTick(h, MakeTick(999, 2)), "tick before the gap");
    Expect(!rec.Tick(1000, &h) && !rec.Tick(1004, &h) && rec.Tick(1005, &h) && SameTick(h, MakeTick(1005, 2)),
           "gap ticks absent");
    Expect(!rec.Tick(0, &h) && !rec.Tick(1601, &h), "out of range");
    bool ordered = true;
    for (size_t i = 1; i < rec.inputs.size(); ++i) ordered &= rec.inputs[i - 1].callT <= rec.inputs[i].callT;
    Expect(ordered && rec.inputs[5].str == "str", "inputs in call order");
    Expect(rec.setup == b.setup, "setup kept");

    Build open = b;
    open.close = false;
    Recording inc;
    Expect(Load(WriteWsr(open), &inc, &err) && !inc.complete && inc.lastTick == 1600, "incomplete file still loads");

    Build bad = b;
    bad.head = R"({"format":1,"engineHash":1,"tickMs":10})";
    Expect(!Load(WriteWsr(bad), &inc, &err) && err.find("tick length") != std::string::npos, "other tick length refused");

    std::vector<uint8_t> out;
    {
        wsr::Writer w;
        w.OpenMemory(&out);
        const std::string head = R"({"format":1,"engineHash":1})";
        w.Chunk(wsr::kHEAD, head.data(), head.size());
        const uint8_t junk[30] = {5, 1, 0};
        w.Chunk(wsr::kINPT, junk, sizeof junk);
        w.Close();
    }
    Expect(!Load(out, &inc, &err) && err.find("INPT") != std::string::npos, "malformed INPT refused");

    out.clear();
    {
        wsr::Writer w;
        w.OpenMemory(&out);
        const std::string head = R"({"format":1,"engineHash":1})";
        w.Chunk(wsr::kHEAD, head.data(), head.size());
        rec::TickChunk tc;
        tc.Add(MakeTick(1, 0));
        tc.Add(MakeTick(3000000, 0));
        const auto t = tc.Take();
        w.Chunk(wsr::kTICK, t.data(), t.size());
        w.Close();
    }
    Expect(!Load(out, &inc, &err) && err.find("implausible") != std::string::npos, "huge tick span refused");
    Expect(ContributorKey(R"({"contributors":[]})").empty() && ContributorKey("nope").empty(), "empty contributor keys");
}

void Refusals() {
    Recording rec;
    std::string err;
    Load(WriteWsr(Sample()), &rec, &err);
    ArmEnv env;
    Expect(ArmRefusal(rec, env).empty(), "sample arms");
    ArmEnv e = env;
    e.inLobby = true;
    Expect(ArmRefusal(rec, e).find("lobby") != std::string::npos, "lobby refused");
    e = env;
    e.online = true;
    Expect(ArmRefusal(rec, e).find("offline") != std::string::npos, "online refused");
    e = env;
    e.inMatch = true;
    Expect(ArmRefusal(rec, e).find("match") != std::string::npos, "in a match refused");
    e = env;
    e.contentHash = "abcdef0123456789abcdef";
    Expect(ArmRefusal(rec, e).find("mod content") != std::string::npos, "other content refused");
    e.anyContent = true;
    Expect(ArmRefusal(rec, e).empty(), "ReplayAnyContent allows other content");
    Recording r2 = rec;
    r2.online = true;
    Expect(ArmRefusal(r2, env).find("online matches") != std::string::npos, "online recording refused");
    r2 = rec;
    r2.exeBuild = "1076";
    Expect(ArmRefusal(r2, env).find("build 1076") != std::string::npos, "other build refused");
    r2 = rec;
    r2.seeds.clear();
    Expect(ArmRefusal(r2, env).find("seeds") != std::string::npos, "no seeds refused");
    r2 = rec;
    r2.ticks.clear();
    Expect(ArmRefusal(r2, env).find("ticks") != std::string::npos, "no ticks refused");
}

// ---------------------------------------------------------------- the core
void Forcing() {
    auto rec = std::make_shared<Recording>();
    std::string err;
    Load(WriteWsr(Sample()), rec.get(), &err);
    ReplayCore c;
    c.Start(rec, true, true);
    uint32_t v = 0;
    Expect(c.ForceSeed(0, 0x4ee250, &v) && v == 222, "logic seed: the latest of that caller");
    Expect(c.ForceSeed(0, 0x4ee250, &v) && v == 222, "every later call of that caller gets it too");
    Expect(c.ForceSeed(1, 0x4ee256, &v) && v == 222, "second seed");
    Expect(!c.ForceSeed(0, 0x4ffbd0, &v) && !c.ForceSeed(1, 0x4ee250, &v), "unrecorded seed sites pass");
    uint32_t st = 0, bits = 0;
    Expect(c.ForceDraw(0, 0x4a44c9, &st, &bits) && st == 1 && bits == 10, "site draw 1");
    Expect(c.ForceDraw(0, 0x4e9573, &st, &bits) && st == 4 && bits == 40, "other site keeps its own cursor");
    Expect(c.ForceDraw(0, 0x4a44c9, &st, &bits) && st == 3 && bits == 30, "site draw 2");
    Expect(!c.ForceDraw(0, 0x4a44c9, &st, &bits), "site used up");
    Expect(!c.ForceDraw(0, 0x50a45b, &st, &bits), "same address, other RNG: not that site");
    Expect(c.ForceDraw(1, 0x50a45b, &st, &bits) && st == 2, "second RNG site");
    Expect(c.Count().seedsForced == 3 && c.Count().drawsForced == 4 && c.Count().drawsUnforced == 2, "forcing counters");
    c.Start(rec, true, true);
    Expect(c.ForceDraw(0, 0x4a44c9, &st, &bits) && st == 1 && c.Count().drawsForced == 1, "Start resets the cursors");
}

void Schedule() {
    auto rec = std::make_shared<Recording>();
    std::string err;
    Load(WriteWsr(Sample()), rec.get(), &err);
    ReplayCore c;
    c.Start(rec, true, true);
    std::vector<std::pair<uint16_t, uint32_t>> sent;
    for (uint32_t t = 0; t <= 40 * 700; t += 10)
        c.Due(t, [&](const rec::Input& i) {
            sent.push_back({i.id, i.time});
            return true;
        });
    bool same = sent.size() == rec->inputs.size();
    for (size_t i = 0; same && i < sent.size(); ++i) same = sent[i].first == rec->inputs[i].id && sent[i].second == rec->inputs[i].time;
    Expect(same && c.Count().injected == 40, "every input once, in order, at its call time");

    c.Start(rec, true, true);
    c.SetSkip(0x105, 0);
    size_t n = 0;
    for (uint32_t t = 0; t <= 40 * 700; t += 10) c.Due(t, [&](const rec::Input&) { return ++n, true; });
    Expect(c.Count().skipped == 1 && n == 39 && c.Count().late == 0, "ReplaySkip by id");
    c.Start(rec, true, true);
    c.SetSkip(0x105, 1);
    n = 0;
    c.Due(40 * 700, [&](const rec::Input&) { return ++n, true; });
    Expect(c.Count().skipped == 0 && c.Count().late > 0, "ReplaySkip with another time keeps it; late inputs counted");

    c.Start(rec, true, true);
    c.SetSkip(0, 0);
    c.Due(29, [](const rec::Input&) { return true; });
    Expect(c.NextInput() == 0, "nothing before the first call time");
    c.Due(30, [](const rec::Input&) { return false; });
    Expect(c.NextInput() == 1 && c.Count().failed == 1 && c.Count().injected == 0, "failed injection counted");
    c.Due(30 + 700, [](const rec::Input&) { return true; });
    Expect(c.Count().injected == 1, "next input at its call time");
}

void Compare() {
    auto rec = std::make_shared<Recording>();
    std::string err;
    Load(WriteWsr(Sample()), rec.get(), &err);
    ReplayCore c;
    c.Start(rec, true, true);
    TickHash r;
    uint8_t mask = 0;
    for (uint32_t t = 1; t <= 50; ++t) Expect(c.Compare(MakeTick(t, 2), &r, &mask) == ReplayCore::Cmp::Match, "match");
    Expect(c.Compare(MakeTick(1002, 2), &r, &mask) == ReplayCore::Cmp::NotRecorded, "gap tick not compared");
    TickHash bad = MakeTick(51, 2);
    bad.c[kWorms] ^= 1;
    bad.c[kTasks] ^= 1;
    bad.engine ^= 1;
    Expect(c.Compare(bad, &r, &mask) == ReplayCore::Cmp::Mismatch && mask == ((1u << kWorms) | (1u << kTasks)),
           "mismatch names the components");
    TickHash modsOnly = MakeTick(52, 2);
    modsOnly.mods ^= 5;
    Expect(c.Compare(modsOnly, &r, &mask) == ReplayCore::Cmp::Mismatch && mask == 0, "mods-only mismatch");
    Expect(c.Count().compared == 52 && c.Count().matched == 50 && c.Count().firstDivergence == 51, "compare counters");

    c.Start(rec, true, false);
    Expect(c.Compare(modsOnly, &r, &mask) == ReplayCore::Cmp::Match, "mods ignored when contributors differ");
    c.Start(rec, false, false);
    Expect(c.Compare(bad, &r, &mask) == ReplayCore::Cmp::NotRecorded && c.Count().compared == 0,
           "other engine hash version: not compared");
}

void Clock() {
    VirtualClock k;
    Expect(k.Filter(1000, false) == 1000, "first call passes through");
    Expect(k.Filter(1016, false) == 1016 && k.Filter(1033, false) == 1033, "1x follows real time");
    k.SetPaused(true);
    Expect(k.Filter(1500, false) == 1033 && k.Filter(2000, false) == 1033, "paused: frozen");
    Expect(k.Filter(2100, true) == 1133, "the game's own pause moves with real time");
    k.SetPaused(false);
    Expect(k.Filter(2110, false) == 1143, "resumes from where it was");
    k.SetSpeedMilli(250);
    int v = 0;
    for (int i = 1; i <= 100; ++i) v = k.Filter(2110 + 7 * i, false);
    Expect(v == 1143 + 175, "0.25x over 700 ms: 175 ms, no drift from rounding");
    k.SetSpeedMilli(8000);
    const int before = k.Filter(2810, false);
    Expect(k.Filter(2826, false) == before + 128, "8x of a 16 ms frame");
    Expect(k.Filter(3826, false) == before + 128 + 1000, "8x of a 1 s hitch is capped at the frame's own time");
    Expect(k.Filter(3900, false) == before + 128 + 1000 + 320, "8x of 74 ms is capped at 320");
    k.SetSpeedMilli(1000);
    const int a = k.Filter(4000, false);
    Expect(k.Filter(3990, false) == a - 10, "real time going back is followed");
    k.SetFast(true);
    const int f = k.Filter(4000, false);
    Expect(f == a - 10 + 320, "fast-forward: a fixed step per frame");
    k.SetCap(true, f + 500);
    Expect(k.Filter(4016, false) == f + 320 && k.Filter(4032, false) == f + 500 && k.Filter(4048, false) == f + 500,
           "fast-forward stops at the cap");
    k.SetCap(true, f + 100);
    Expect(k.Filter(4064, false) == f + 500, "a cap below the clock never moves it back");
    k.Reset();
    Expect(k.Filter(9000, false) == 9000, "reset starts from real time again");
}

void Setup() {
    setup::Data d;
    d.level = "Multi.W3DCropCircle";
    d.landFile = "cropcircle-w3d.xan";
    d.landTheme = "Farm";
    d.dataBank = "cropcircle-w3d";
    d.timeOfDay = "DAY";
    d.lastScheme = "FETXT.Scheme.QuickGame";
    d.schemeName = "Quick \"Game\"";
    d.scheme = 0x1234;
    d.haveScheme = true;
    d.init = 0x5678;
    d.haveInit = true;
    d.teams = {{"Team \xc3\xa9", 4}, {"CPU", 4}};
    const std::string rec = setup::ToJson(d);
    std::string why;
    Expect(setup::Compare(rec, rec, &why) && why.empty(), "same setup");
    setup::Data e = d;
    e.landFile = "other.xan";
    Expect(!setup::Compare(rec, setup::ToJson(e), &why) && why.find("land file") != std::string::npos &&
               why.find("other.xan") != std::string::npos,
           "land file difference named");
    e = d;
    e.teams.pop_back();
    Expect(!setup::Compare(rec, setup::ToJson(e), &why) && why.find("teams") != std::string::npos, "team count");
    e = d;
    e.teams[1].worms = 3;
    Expect(!setup::Compare(rec, setup::ToJson(e), &why) && why.find("team 2 worms") != std::string::npos, "worm count");
    e = d;
    e.init = 1;
    Expect(!setup::Compare(rec, setup::ToJson(e), &why) && why.find("team setup") != std::string::npos, "init hash");
    e = d;
    e.haveScheme = false;
    Expect(!setup::Compare(rec, setup::ToJson(e), &why) && why.find("scheme settings") != std::string::npos,
           "missing live field");
    Expect(setup::Compare(setup::ToJson(e), rec, &why), "a field the recording lacks is not compared");
    Expect(setup::Compare(R"({"v":2,"landFile":"x"})", rec, &why) && why.find("newer") != std::string::npos,
           "newer fingerprint not compared");
    Expect(!setup::Compare("{", rec, &why), "unreadable recording");
    Expect(setup::Compare(R"({"landFile":"cropcircle-w3d.xan","extra":1})", rec, &why), "unknown recorded keys ignored");
}

// ---------------------------------------------------------------- record, then replay, a toy simulation
// Inputs are stored with a time, posted when logic time reaches it and fold into the state; the state is hashed at
// every tick end. The replay injects the recording's inputs through ReplayCore::Due before each logic time runs.
struct Toy {
    uint64_t state = 1;
    std::vector<rec::Input> queued;
    void Post(uint32_t t) {
        for (size_t i = 0; i < queued.size();) {
            if (queued[i].time == t) {
                state = FnvV(queued[i].a, FnvV(queued[i].id, state));
                queued.erase(queued.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
        state = FnvV(t, state);
    }
    TickHash Hash(uint32_t tick) const {
        TickHash h{};
        h.tick = tick;
        h.c[kWorms] = state;
        h.engine = Fnv(h.c, sizeof h.c);
        return h;
    }
};

void RoundTrip() {
    std::mt19937 rng(7);
    Build b = Sample();
    b.inputs.clear();
    b.ticks.clear();
    Toy live;
    for (uint32_t t = 10; t <= 20 * 3000; t += 10) {
        if (rng() % 9 == 0) {
            rec::Input i{rec::kSendInt, static_cast<uint16_t>(rng() % 50 + 1), static_cast<uint32_t>(rng()), 0,
                         (t / 20 + 1) * 20, t, 0x5057a0, ""};
            b.inputs.push_back(i);
            live.queued.push_back(i);
        }
        live.Post(t);
        if (t % 20 == 0) b.ticks.push_back(live.Hash(t / 20));
    }
    auto rec = std::make_shared<Recording>();
    std::string err;
    Expect(Load(WriteWsr(b), rec.get(), &err), "toy recording loads");

    auto replay = [&](uint16_t skipId, uint32_t skipTime, uint32_t* firstDiv) {
        ReplayCore c;
        c.Start(rec, true, true);
        c.SetSkip(skipId, skipTime);
        Toy sim;
        uint32_t div = 0;
        for (uint32_t t = 10; t <= 20 * 3000; t += 10) {
            c.Due(t, [&](const rec::Input& i) {
                sim.queued.push_back(i);
                return true;
            });
            sim.Post(t);
            if (t % 20) continue;
            TickHash r;
            uint8_t mask;
            if (c.Compare(sim.Hash(t / 20), &r, &mask) == ReplayCore::Cmp::Mismatch && !div) div = t / 20;
        }
        *firstDiv = div;
        return c.Count();
    };
    uint32_t div = 1;
    const auto n = replay(0, 0, &div);
    Expect(div == 0 && n.compared == 3000 && n.matched == 3000 && n.injected == b.inputs.size(),
           "replay matches every tick");
    const rec::Input& victim = b.inputs[b.inputs.size() / 2];
    const auto m = replay(victim.id, victim.time, &div);
    Expect(m.skipped == 1 && div == victim.time / kTickMs && m.matched == div - 1,
           "a dropped input diverges exactly at its tick, every earlier tick matching");
}
}  // namespace

int main() {
    Records();
    Recordings();
    Refusals();
    Forcing();
    Schedule();
    Compare();
    Clock();
    Setup();
    RoundTrip();
    printf("wormsign_player_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
