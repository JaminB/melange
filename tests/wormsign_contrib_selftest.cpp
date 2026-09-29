// Offline self-test for the hash contributor registry (src/wormsign/contrib.cpp), the detail records and their codec
// (detail.cpp) and the FPU watch (fpu.cpp). Usage: wormsign_contrib_selftest [fuzzSeconds=5]. Exit code 0 = all passed.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "core/dllcall.h"
#include "core/log.h"
#include "melange/wormsign.h"
#include "tools/json_read.h"
#include "wormsign/contrib.h"
#include "wormsign/detail.h"
#include "wormsign/fpu.h"
#include "wormsign/hash_engine.h"

namespace ws = melange::wormsign;
namespace ct = melange::wormsign::contrib;
namespace dt = melange::wormsign::detail;
namespace json = melange::json;

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

std::vector<std::string> g_log;
void Tap(const char*, const char* msg) { g_log.push_back(msg); }
bool Logged(const std::string& needle) {
    for (auto& l : g_log)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

// ------------------------------------------------------------------ contributors
void FeedU32(ws::Hasher& h, uint32_t, void* user) { h.Val(*static_cast<uint32_t*>(user)); }
void FeedConst(ws::Hasher& h, uint32_t, void*) { h.Bytes("const", 5); }

int64_t g_fake = 0;
int64_t FakeQpc() { return g_fake; }
void Slow(ws::Hasher& h, uint32_t tick, void*) {
    g_fake += 500;  // 50 us at 10 MHz
    h.Val(tick);
}
void Fast(ws::Hasher& h, uint32_t tick, void*) {
    g_fake += 10;
    h.Val(tick);
}
void Spin50(ws::Hasher& h, uint32_t tick, void*) {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    do QueryPerformanceCounter(&b);
    while ((b.QuadPart - a.QuadPart) * 1000000 < 50 * f.QuadPart);
    h.Val(tick);
}
void Crash(ws::Hasher&, uint32_t tick, void*) {
    if (tick >= 3) {
        volatile int* p = nullptr;
        *p = 1;
    }
}
int g_selfHandle = 0, g_added = 0;
void RemovesSelf(ws::Hasher& h, uint32_t, void*) {
    h.Val(1);
    ws::RemoveContributor(g_selfHandle);
}
void AddsOther(ws::Hasher& h, uint32_t, void*) {
    h.Val(2);
    if (!g_added) g_added = ws::AddContributor("z.late", &FeedConst, nullptr);
}

std::string FirstDiff(uint32_t tick, const std::vector<ct::Entry>& other) {
    ct::Entry e[16];
    const size_t n = ct::HashesAt(tick, e, 16);
    for (size_t i = 0; i < n && i < other.size(); ++i)
        if (e[i].hash != other[i].hash) return e[i].name;
    return "";
}

void TestContrib() {
    Expect(ct::HashTick(1) == 0, "no contributors: mods 0");
    uint64_t all = 1, rep = 1;
    Expect(ct::ModsAt(1, &all, &rep) && all == 0 && rep == 0, "no contributors: ring holds 0");
    Expect(ws::AddContributor("", &FeedConst, nullptr) == 0, "empty name refused");
    Expect(ws::AddContributor("has space", &FeedConst, nullptr) == 0, "bad character refused");
    Expect(ws::AddContributor(std::string(64, 'a').c_str(), &FeedConst, nullptr) == 0, "64-character name refused");
    Expect(ws::AddContributor("x", nullptr, nullptr) == 0, "null function refused");

    uint32_t counter = 7;
    const int hb = ws::AddContributor("b.counter", &FeedU32, &counter);
    const int ha = ws::AddContributor("a.const", &FeedConst, nullptr);
    Expect(hb > 0 && ha > 0, "registered");
    Expect(ws::AddContributor("b.counter", &FeedConst, nullptr) == 0, "duplicate name refused");
    ct::Info info[8];
    size_t n = ct::List(info, 8);
    Expect(n == 2 && strcmp(info[0].name, "a.const") == 0 && strcmp(info[1].name, "b.counter") == 0, "name order");

    // A run of 20 ticks, then the same run with the counter different at tick 12 (a second peer).
    std::vector<uint64_t> runA, runB;
    std::vector<std::vector<ct::Entry>> compsA;
    for (uint32_t t = 1; t <= 20; ++t) {
        counter = t * 3;
        runA.push_back(ct::HashTick(t));
        ct::Entry e[16];
        const size_t k = ct::HashesAt(t, e, 16);
        compsA.emplace_back(e, e + k);
    }
    std::string culprit;
    uint32_t firstDiff = 0;
    for (uint32_t t = 1; t <= 20; ++t) {
        counter = t == 12 ? 999 : t * 3;
        runB.push_back(ct::HashTick(t));
        if (!firstDiff && runB.back() != runA[t - 1]) {
            firstDiff = t;
            culprit = FirstDiff(t, compsA[t - 1]);
        }
    }
    Expect(firstDiff == 12, "the peer's different value is flagged at its tick: " + std::to_string(firstDiff));
    Expect(culprit == "b.counter", "and names the contributor: " + culprit);
    Expect(runA[12] == runB[12], "same values again: same mods");
    Expect(runA[0] != 0 && runA[0] != runA[1], "the counter moves the mods hash");

    // Registration order does not matter, only names.
    ws::RemoveContributor(ha);
    ws::RemoveContributor(hb);
    Expect(ct::Count() == 0, "removed");
    const int hb2 = ws::AddContributor("b.counter", &FeedU32, &counter);
    const int ha2 = ws::AddContributor("a.const", &FeedConst, nullptr);
    counter = 3;
    Expect(ct::HashTick(1) == runA[0], "registration order is irrelevant");
    const uint64_t lh = ct::ListHash();

    // Contributors outside the replay compare.
    ws::ContribOptions noReplay;
    noReplay.inReplayCompare = false;
    const int hc = ws::AddContributor("c.local", &FeedConst, nullptr, noReplay);
    Expect(ct::ListHash() != lh, "the list hash follows the list");
    counter = 3;
    const uint64_t withC = ct::HashTick(1);
    Expect(ct::ModsAt(1, &all, &rep) && all == withC && rep == runA[0] && all != rep,
           "replay value leaves out inReplayCompare=false");
    ws::RemoveContributor(hc);
    Expect(ct::ListHash() == lh, "list hash back");

    ws::ContribOptions v2;
    v2.version = 2;
    ws::RemoveContributor(ha2);
    const int ha3 = ws::AddContributor("a.const", &FeedConst, nullptr, v2);
    Expect(ct::ListHash() != lh, "a version change changes the list hash");
    ws::RemoveContributor(ha3);
    ws::RemoveContributor(hb2);

    // Demotion with a fake clock: 50 us per call for 500 ticks.
    ct::SetClockForTest(&FakeQpc, 10000000);
    const int hs = ws::AddContributor("mod.slow.env", &Slow, nullptr);
    const int hf = ws::AddContributor("mod.fast.env", &Fast, nullptr);
    for (uint32_t t = 1; t <= 499; ++t) ct::HashTick(t);
    n = ct::List(info, 8);
    Expect(n == 2 && !info[0].demoted && !info[1].demoted, "not demoted before 500 ticks");
    ct::HashTick(500);
    n = ct::List(info, 8);
    const ct::Info& slow = strcmp(info[0].name, "mod.slow.env") == 0 ? info[0] : info[1];
    const ct::Info& fast = strcmp(info[0].name, "mod.slow.env") == 0 ? info[1] : info[0];
    Expect(slow.demoted && slow.demotedAt == 500 && slow.lastP95Us10 == 500, "slow contributor demoted at tick 500");
    Expect(!fast.demoted && fast.lastP95Us10 == 10, "fast contributor kept");
    Expect(ct::Describe(slow).find("hashed every 10 ticks") != std::string::npos, "report: " + ct::Describe(slow));
    Expect(ct::Describe(fast) == "hashed every tick", "report for the fast one");
    Expect(Logged("contributor mod.slow.env demoted at tick 500"), "demotion logged");
    ct::HashTick(501);
    ct::Entry e[8];
    n = ct::HashesAt(501, e, 8);
    bool slowComputed = true;
    for (size_t i = 0; i < n; ++i)
        if (strcmp(e[i].name, "mod.slow.env") == 0) slowComputed = e[i].computed;
    Expect(n == 2 && !slowComputed, "demoted: skipped at tick 501");
    ct::HashTick(510);
    n = ct::HashesAt(510, e, 8);
    for (size_t i = 0; i < n; ++i)
        if (strcmp(e[i].name, "mod.slow.env") == 0) slowComputed = e[i].computed;
    Expect(slowComputed, "demoted: hashed at tick 510");
    json::Value v;
    json::Error err;
    Expect(json::Parse(ct::NoteJson(), &v, &err) && v.Get("contributors") && v.Get("contributors")->items.size() == 2,
           "NOTE JSON parses");
    ws::RemoveContributor(hs);
    ws::RemoveContributor(hf);
    ct::SetClockForTest(nullptr, 0);

    // demoteEvery 1 never demotes; a real 50 us contributor is demoted after 500 ticks.
    ws::ContribOptions never;
    never.demoteEvery = 1;
    const int hn = ws::AddContributor("mod.spin1.env", &Spin50, nullptr, never);
    const int hr = ws::AddContributor("mod.spin.env", &Spin50, nullptr);
    for (uint32_t t = 1; t <= 500; ++t) ct::HashTick(t);
    n = ct::List(info, 8);
    Expect(n == 2 && info[0].demoted && !info[1].demoted, "real clock: 50 us demoted, demoteEvery=1 kept");
    Expect(ct::Describe(info[0]).find("hashed every 10 ticks") != std::string::npos, "real clock report");
    ws::RemoveContributor(hn);
    ws::RemoveContributor(hr);

    // A faulting contributor is switched off; the others keep going.
    const int hx = ws::AddContributor("x.crash", &Crash, nullptr);
    const int hk = ws::AddContributor("k.const", &FeedConst, nullptr);
    ct::HashTick(1);
    ct::HashTick(2);
    const uint64_t atFault = ct::HashTick(3);
    const uint64_t afterFault = ct::HashTick(4);
    n = ct::List(info, 8);
    const ct::Info& cr = strcmp(info[0].name, "x.crash") == 0 ? info[0] : info[1];
    Expect(cr.faulted && cr.faultedAt == 3, "fault caught and recorded");
    Expect(atFault == afterFault && atFault != 0, "the rest still hashes");
    Expect(ct::Describe(cr).find("faulted at tick 3") != std::string::npos, "fault report");
    ws::RemoveContributor(hx);
    ws::RemoveContributor(hk);

    // Changes from inside a callback apply after the tick.
    g_selfHandle = ws::AddContributor("r.self", &RemovesSelf, nullptr);
    const int hadd = ws::AddContributor("q.adder", &AddsOther, nullptr);
    ct::HashTick(1);
    n = ct::List(info, 8);
    Expect(n == 2 && strcmp(info[0].name, "q.adder") == 0 && strcmp(info[1].name, "z.late") == 0,
           "removal and addition inside a callback");
    ws::RemoveContributor(hadd);
    ws::RemoveContributor(g_added);

    ct::ResetSession();
    Expect(!ct::ModsAt(1, nullptr, nullptr), "session reset clears the ring");
    Expect(ct::Count() == 0, "all removed");
}

// ------------------------------------------------------------------ detail records
dt::DetailRec Random(std::mt19937& r, uint32_t tick, int worms, int proj) {
    dt::DetailRec d{};
    dt::Clear(&d);
    d.tick = tick;
    d.rng = r();
    d.rng2 = r();
    d.curTeam = static_cast<int32_t>(r() % 4);
    d.activeWorm = static_cast<int32_t>(r() % 16);
    d.wormCount = static_cast<uint8_t>(worms);
    for (int i = 0; i < worms; ++i) {
        d.worms[i].slot = static_cast<uint8_t>(i);
        for (auto& b : d.worms[i].bytes) b = static_cast<uint8_t>(r());
    }
    for (int i = 0; i < 2; ++i) d.teams[i] = {static_cast<uint8_t>(i), 1, r() % 100};
    d.projCount = static_cast<uint16_t>(proj);
    for (int i = 0; i < proj; ++i) {
        d.proj[i] = {0x870000u + (r() & 0xfff), r(), static_cast<uint8_t>(1 + (r() & 1)), {}};
        for (auto& p : d.proj[i].pv) p = r();
    }
    return d;
}

std::vector<uint8_t> Packed(const dt::DetailRec& d) {
    std::vector<uint8_t> b(dt::kMaxPacked);
    b.resize(dt::Pack(d, b.data(), b.size()));
    return b;
}

void SetF(dt::WormDetail& w, int pos, float f) { memcpy(w.bytes + pos, &f, 4); }
void SetU16(dt::WormDetail& w, int pos, uint16_t v) { memcpy(w.bytes + pos, &v, 2); }

void TestDetail(int fuzzSeconds) {
    std::mt19937 r(1234);
    const dt::DetailRec a = Random(r, 77, 8, 3);
    const auto pa = Packed(a);
    dt::DetailRec b;
    Expect(!pa.empty() && dt::Unpack(pa.data(), pa.size(), &b) && Packed(b) == pa, "pack round trip");
    Expect(pa.size() == 20 + 1 + 8 * 94 + 1 + 2 * 6 + 2 + 3 * 33, "packed size");
    dt::DetailRec big = Random(r, 1, 16, 64);
    big.teams[2] = {2, 1, 5};
    big.teams[3] = {3, 0, 6};
    Expect(Packed(big).size() == dt::kMaxPacked, "largest record is kMaxPacked");
    uint8_t small[16];
    Expect(dt::Pack(a, small, sizeof small) == 0, "pack refuses a short buffer");
    Expect(!dt::Unpack(pa.data(), pa.size() - 1, &b), "truncated record refused");
    auto bad = pa;
    bad[20] = 17;
    Expect(!dt::Unpack(bad.data(), bad.size(), &b), "17 worms refused");

    // Recompute follows the engine hash formulas.
    uint64_t c[ws::kEngineComps] = {};
    const uint8_t mask = dt::Recompute(a, c);
    Expect(mask == 0x37, "recompute covers every component but the tasks");
    Expect(c[ws::kTimeRng] == ws::FnvV(a.rng, ws::FnvV(a.tick * ws::kTickMs)), "c0 formula");
    uint64_t hw = ws::kFnvBasis;
    for (int i = 0; i < a.wormCount; ++i) hw = ws::Fnv(a.worms[i].bytes, dt::kWormBytes, ws::FnvV(int(a.worms[i].slot), hw));
    Expect(c[ws::kWorms] == hw, "c2 formula");
    dt::DetailRec un = a;
    un.worms[2].slot |= dt::kSlotUnreadable;
    uint64_t c2[ws::kEngineComps] = {};
    dt::Recompute(un, c2);
    Expect(c2[ws::kWorms] != c[ws::kWorms], "an unreadable worm hashes its slot only");

    // Ring: the last 512 ticks.
    dt::Reset();
    std::vector<std::vector<uint8_t>> ring;
    for (uint32_t t = 1; t <= 700; ++t) {
        const dt::DetailRec d = Random(r, t, 8, t % 5);
        ring.push_back(Packed(d));
        dt::Commit(d);
    }
    std::vector<uint8_t> got;
    Expect(!dt::GetPacked(700 - 512, &got), "older than 512 ticks: gone");
    Expect(dt::GetPacked(700 - 511, &got) && got == ring[700 - 512], "512th newest kept");
    Expect(dt::GetPacked(700, &got) && got == ring.back(), "newest kept");
    dt::DetailRec gd;
    Expect(dt::Get(650, &gd) && gd.tick == 650, "get unpacks");
    Expect(dt::RingBytes() <= 1200 * 1024, "ring within 1.2 MB: " + std::to_string(dt::RingBytes()));
    dt::Reset();
    for (uint32_t t = 1; t <= 512; ++t) dt::Commit(Random(r, t, 16, 64));
    int kept = 0;
    for (uint32_t t = 1; t <= 512; ++t) kept += dt::GetPacked(t, &got) ? 1 : 0;
    Expect(dt::GetPacked(512, &got) && kept >= 300 && kept < 512,
           "largest records: the arena keeps the newest " + std::to_string(kept));
    dt::Reset();
    Expect(!dt::GetPacked(512, &got), "reset empties the ring");

    // Delta codec over an evolving sequence.
    std::vector<uint8_t> stream, prev;
    std::vector<std::vector<uint8_t>> src;
    dt::DetailRec cur = Random(r, 1, 8, 1);
    for (uint32_t t = 1; t <= 300; ++t) {
        cur.tick = t;
        cur.rng = r();
        SetF(cur.worms[t % 8], 24, static_cast<float>(t));
        if (t % 50 == 0) cur.projCount = static_cast<uint16_t>((cur.projCount + 1) % 4);
        auto p = Packed(cur);
        dt::EncodeDelta(prev.empty() ? nullptr : prev.data(), prev.size(), p.data(), p.size(), &stream);
        src.push_back(p);
        prev = p;
    }
    size_t at = 0, i = 0, rawTotal = 0;
    std::vector<uint8_t> dec, last;
    bool ok = true;
    while (at < stream.size() && ok) {
        const size_t used = dt::DecodeDelta(last.empty() ? nullptr : last.data(), last.size(), stream.data() + at,
                                            stream.size() - at, &dec);
        ok = used && i < src.size() && dec == src[i];
        rawTotal += dec.size();
        at += used;
        last = dec;
        ++i;
    }
    Expect(ok && i == src.size(), "delta round trip over 300 ticks");
    Expect(stream.size() * 4 < rawTotal, "delta is smaller: " + std::to_string(stream.size()) + " of " +
                                             std::to_string(rawTotal));

    // Named fields.
    dt::DetailRec t0 = Random(r, 99, 6, 1), t1 = t0;
    t1.tick = 100;
    t0.worms[3].slot = 3;
    t1.worms[3].slot = 3;
    SetU16(t0.worms[3], 89, 100);
    SetU16(t1.worms[3], 89, 1);
    SetF(t0.worms[3], 24, 10.5f);
    SetF(t1.worms[3], 24, 12.25f);
    const std::string d = dt::Diff(t0, t1);
    Expect(d.find("worm[3].energy 100 -> 1\n") != std::string::npos, "diff names the energy: " + d);
    Expect(d.find("worm[3].pos.x 10.5 -> 12.25\n") != std::string::npos, "diff names the position");
    Expect(d.find("worm[2]") == std::string::npos, "unchanged worms are not in the diff");
    t1.wormCount = 5;
    Expect(dt::Diff(t0, t1).find("worm[5] present -> absent") != std::string::npos, "a missing worm");
    const std::string j = dt::ToJson(t0);
    json::Value v;
    json::Error err;
    const bool parsed = json::Parse(j, &v, &err);
    Expect(parsed, "detail JSON parses: " + err.text);
    if (parsed) {
        const auto* w = v.Get("worms");
        const auto* w3 = w && w->items.size() > 3 ? &w->items[3] : nullptr;
        Expect(w3 && w3->Get("energy") && w3->Get("energy")->number == 100, "JSON energy");
        Expect(w3 && w3->Get("pos") && w3->Get("pos")->items.size() == 3 && w3->Get("pos")->items[0].number == 10.5,
               "JSON pos");
        Expect(v.Get("projectiles") && v.Get("projectiles")->items.size() == 1, "JSON projectiles");
    }

    // Fuzz: mutated packed records and delta streams never fault and never read out of range.
    const DWORD until = GetTickCount() + static_cast<DWORD>(fuzzSeconds) * 1000;
    uint64_t runs = 0;
    std::vector<uint8_t> m;
    while (GetTickCount() < until) {
        for (int k = 0; k < 200; ++k, ++runs) {
            m = (runs & 1) ? stream : pa;
            const size_t cut = r() % (m.size() + 1);
            if (r() & 1) m.resize(cut);
            for (int f = 1 + r() % 8; f > 0 && !m.empty(); --f) m[r() % m.size()] = static_cast<uint8_t>(r());
            dt::DetailRec x;
            dt::Unpack(m.data(), m.size(), &x);
            std::vector<uint8_t> o;
            const size_t used = dt::DecodeDelta(src[0].data(), src[0].size(), m.data(), m.size(), &o);
            if (used > m.size()) Expect(false, "decode used past the end");
        }
    }
    Expect(runs > 0, "fuzz ran " + std::to_string(runs) + " cases");
}

// ------------------------------------------------------------------ FPU watch
void TestFpu() {
    namespace fp = melange::wormsign::fpu;
    fp::ResetForTest();
    g_log.clear();
    fp::Event ev[8];
    fp::Observe(1, 1, 0x027f, 0x1f80);
    fp::Observe(1, 2, 0x027f, 0x1f80 | 0x20);
    Expect(fp::Events(ev, 8) == 0, "expected word and MXCSR status flags: no event");
    melange::dllcall::Note("test!SomeCall");
    fp::Observe(1, 3, 0x037f, 0x1fa0);
    fp::Observe(1, 4, 0x037f, 0x1fa0);
    Expect(fp::Events(ev, 8) == 1 && ev[0].tick == 3 && ev[0].cw == 0x037f && ev[0].prevCw == 0x027f &&
               strcmp(ev[0].lastCall, "test!SomeCall") == 0,
           "one event per change, with the last DLL call");
    Expect(Logged("FPU control word 037f at tick 3") && Logged("test!SomeCall"), "warned with the tick and the call");
    fp::Observe(1, 5, 0x027f, 0x1fa0);
    Expect(fp::Events(ev, 8) == 2 && Logged("back to 027f at tick 5"), "restore logged");
    fp::Observe(1, 6, 0x027f, 0x9fc0);
    Expect(fp::Events(ev, 8) == 3 && fp::Mxcsr() == 0x9fc0, "MXCSR control change recorded");
    json::Value v;
    json::Error err;
    Expect(json::Parse(fp::NoteJson(), &v, &err) && v.Get("fpu") && v.Get("fpu")->Get("events") &&
               v.Get("fpu")->Get("events")->items.size() == 3,
           "FPU NOTE JSON parses");
    fp::ResetForTest();
    fp::Observe(2, 1, 0x007f, 0x1f80);
    Expect(fp::Events(ev, 8) == 1 && Logged("FPU control word 007f at tick 1"), "wrong from the first tick");
    fp::ResetForTest();
    for (uint32_t t = 0; t < 200; ++t) fp::Observe(3, t, (t & 1) ? 0x037f : 0x027f, 0x1f80);
    Expect(fp::Changes() == 199 && fp::Events(ev, 8) == 8, "flapping: every change counted, the first 64 kept");
}
}  // namespace

int main(int argc, char** argv) {
    const int fuzz = argc > 1 ? atoi(argv[1]) : 5;
    melange::log::SetTap(&Tap);
    TestContrib();
    TestDetail(fuzz);
    TestFpu();
    printf("wormsign_contrib_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
