// Offline self-test for the event bus (component B, docs/m0-design.md §3.B). No game needed: it points the
// registry reader at a fake name table, builds fake engine messages (with the arena size header at msg-4), and
// drives the same dispatch code the inline hooks use (detail::RunPost / RunDeliver) with a fake "original".
//
// Run: scripts\selftest.ps1   (builds the bus_selftest target and runs it; exit code 0 = all checks passed)
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/bus_internal.h"
#include "core/log.h"

using namespace melange::bus;

namespace {
int g_checks = 0, g_failed = 0;
#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failed;                                                         \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
        }                                                                       \
    } while (0)

void Section(const char* s) { printf("[%s]\n", s); }

// ---------------------------------------------------------------- fake engine globals
constexpr uint32_t kCap = 1300;
std::vector<std::string> g_nameStore;
const char* g_table[kCap] = {};
uintptr_t g_tableVar = 0;  // *0x96d094
uint32_t g_sizeVar = kCap;  // *0x96d08c
struct FakeService {
    uint8_t pad[0x14];
    int32_t handle;
} g_svc = {{}, 7};
uintptr_t g_svcVar = reinterpret_cast<uintptr_t>(&g_svc);  // *0x96d090

constexpr uint32_t kSlotCamera = 5, kSlotTurn = 6, kSlotExplosion = 7, kSlotEmpty = 9, kSlotLate = 11;
MsgId Id(uint32_t slot) { return static_cast<MsgId>(0x8000 | slot); }

void FillRegistry() {
    g_nameStore.resize(kCap);
    for (uint32_t i = 0; i < kCap; ++i) {
        if (i % 7 == 2 || i == kSlotEmpty || i == kSlotLate || i >= 1227) continue;  // some empty slots
        char b[32];
        snprintf(b, sizeof(b), "Test.Msg%u", i);
        g_nameStore[i] = b;
    }
    g_nameStore[kSlotCamera] = "Camera.HasUpdated";
    g_nameStore[kSlotTurn] = "GameLogic.Turn.Started";
    g_nameStore[kSlotExplosion] = "Explosion";
    for (uint32_t i = 0; i < kCap; ++i) g_table[i] = g_nameStore[i].empty() ? nullptr : g_nameStore[i].c_str();
}

// ---------------------------------------------------------------- fake messages
struct FakeMsg {
    alignas(8) uint8_t buf[256] = {};
    uint8_t* raw() { return buf + 4; }
    FakeMsg(uintptr_t vtable, MsgId id, uint32_t objBytes) {
        uint32_t hdr = ((objBytes + 3) & ~3u) + 4;
        memcpy(buf, &hdr, 4);
        uint32_t vt = static_cast<uint32_t>(vtable);
        memcpy(raw(), &vt, 4);
        memcpy(raw() + 4, &id, 2);
    }
    template <class T> void Put(uint32_t off, T v) { memcpy(raw() + off, &v, sizeof(T)); }
    void PutVec(uint32_t off, const float (&v)[3]) { memcpy(raw() + off, v, sizeof(v)); }
    void SetHeader(uint32_t h) { memcpy(buf, &h, 4); }
};

// ---------------------------------------------------------------- recording subscribers
struct Seen {
    MsgId id;
    Path path;
    int32_t handle;
    bool fromPost;
    uint16_t depth;
    uintptr_t caller;
    uint64_t seq;
    uint32_t size;
    std::string name, cls;
};
struct Recorder {
    std::vector<Seen> seen;
};
void Record(const MessageView& m, void* user) {
    static_cast<Recorder*>(user)->seen.push_back(
        {m.id, m.path, m.handle, m.fromPost, m.depth, m.caller, m.seq, m.size, m.name, m.className});
}

int* volatile g_null = nullptr;
int g_faultCalls = 0;
void Faulter(const MessageView&, void*) {
    ++g_faultCalls;
    *g_null = 1;
}

SubId g_selfSub = 0;
int g_selfCalls = 0;
void SelfUnsub(const MessageView&, void*) {
    ++g_selfCalls;
    Unsubscribe(g_selfSub);
}

Recorder g_lateRec;
SubId g_lateSub = 0;
int g_spawnCalls = 0;
void Spawner(const MessageView& m, void*) {
    ++g_spawnCalls;
    if (!g_lateSub) g_lateSub = Subscribe(m.id, Path::Post, &Record, &g_lateRec);
}

// Fake originals: Post fans out into 3 Deliver calls of the same object, like handle 7 does in the engine.
int DeliverOrig(void*, void*, int, char, void*) { return 0; }
int PostFanOut(void* msg, void*) {
    for (int h = 1; h <= 3; ++h) detail::RunDeliver(nullptr, msg, 100 + h, 0, 0x2222, &DeliverOrig, nullptr);
    return 42;
}
int PostNoop(void*, void*) { return 1; }

// ---------------------------------------------------------------- JsonOut collector
class Collect final : public JsonOut {
public:
    std::map<std::string, double> num;
    std::map<std::string, std::string> str;
    std::map<std::string, std::vector<float>> vec;
    void Int(const char* k, int64_t v) override { num[k] = static_cast<double>(v); }
    void Uint(const char* k, uint64_t v) override { num[k] = static_cast<double>(v); }
    void Hex(const char* k, uint64_t v) override { num[k] = static_cast<double>(v); }
    void Float(const char* k, double v) override { num[k] = v; }
    void Str(const char* k, std::string_view v) override { str[k] = std::string(v); }
    void Vec3(const char* k, const float v[3]) override { vec[k] = {v[0], v[1], v[2]}; }
};

MessageView ViewOf(FakeMsg& fm) {
    MessageView m{};
    m.raw = fm.raw();
    memcpy(&m.id, fm.raw() + 4, 2);
    m.size = detail::ObjectSize(fm.raw());
    uint32_t vt;
    memcpy(&vt, fm.raw(), 4);
    m.vtable = vt;
    m.name = NameOf(m.id);
    m.className = detail::ClassNameOf(vt);
    return m;
}
}  // namespace

int main() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = exe;
    logPath = logPath.substr(0, logPath.find_last_of(L"\\/")) + L"\\bus_selftest.log";
    melange::log::Init(logPath);

    FillRegistry();
    detail::SetRegistrySource({reinterpret_cast<uintptr_t>(&g_tableVar), reinterpret_cast<uintptr_t>(&g_sizeVar),
                               reinterpret_cast<uintptr_t>(&g_svcVar)});

    // ------------------------------------------------------------ registry not ready yet
    Section("registry: before the engine fills it");
    CHECK(!RegistryReady());
    CHECK(IdOf("Camera.HasUpdated") == kInvalidId);
    Recorder camRec;
    SubId camSub = SubscribeName("Camera.HasUpdated", Path::Post, &Record, &camRec);
    CHECK(camSub != 0);
    CHECK(detail::PendingNames() == 1);
    CHECK(Subscribe(1, Path::Post, nullptr) == 0);
    CHECK(Subscribe(kInvalidId, Path::Post, &Record) == 0);
    CHECK(SubscribeName("", Path::Post, &Record) == 0);

    // ------------------------------------------------------------ registry ready
    Section("registry: names and ids");
    g_tableVar = reinterpret_cast<uintptr_t>(g_table);
    CHECK(RegistryReady());
    CHECK(Capacity() == kCap);
    detail::Tick();
    CHECK(detail::PendingNames() == 0);
    CHECK(IdOf("Camera.HasUpdated") == Id(kSlotCamera));
    CHECK(IdOf("GameLogic.Turn.Started") == Id(kSlotTurn));
    CHECK(IdOf("GameLogic.Turn") == kInvalidId);         // prefix is not a match
    CHECK(IdOf("GameLogic.Turn.Started.X") == kInvalidId);
    CHECK(IdOf("No.Such.Message") == kInvalidId);
    CHECK(strcmp(NameOf(Id(kSlotExplosion)), "Explosion") == 0);

    size_t used = 0, bad = 0;
    for (uint32_t i = 0; i < kCap; ++i) used += g_table[i] != nullptr;
    size_t visited = 0;
    ForEachName([&](MsgId id, const char* name) {
        ++visited;
        if (IdOf(NameOf(id)) != id || NameOf(id) != name) ++bad;
    });
    CHECK(visited == used);
    CHECK(bad == 0);
    CHECK(detail::UsedSlots() == used);
    printf("  %zu names in %u slots, round trip ok for all\n", visited, kCap);

    // empty slot: placeholder name, not cached as a registry name; a later registration is picked up
    const char* ph = NameOf(Id(kSlotLate));
    CHECK(strcmp(ph, "?800b") == 0);
    CHECK(NameOf(Id(kSlotLate)) == ph);  // static lifetime, stable
    CHECK(IdOf(ph) == kInvalidId);
    CHECK(strcmp(NameOf(Id(2000)), "?87d0") == 0);  // beyond capacity
    Recorder lateNameRec;
    SubId lateNameSub = SubscribeName("Mod.LateMessage", Path::Post, &Record, &lateNameRec);
    CHECK(detail::PendingNames() == 1);
    g_nameStore[kSlotLate] = "Mod.LateMessage";
    g_table[kSlotLate] = g_nameStore[kSlotLate].c_str();
    CHECK(strcmp(NameOf(Id(kSlotLate)), "Mod.LateMessage") == 0);
    detail::Tick();
    CHECK(detail::PendingNames() == 0);

    // system ids
    CHECK(strcmp(NameOf(0x103), "WindowLoseFocus") == 0);
    CHECK(strcmp(NameOf(0x104), "WindowGainFocus") == 0);
    CHECK(strcmp(NameOf(0x1004), "Win32MouseEvent") == 0);
    CHECK(strcmp(NameOf(0x40), "sys:0x40") == 0);
    CHECK(NameOf(0x40) == NameOf(0x40));
    CHECK(IdOf("sys:0x1004") == 0x1004);
    CHECK(IdOf("sys:0x40") == 0x40);
    CHECK(IdOf("WindowGainFocus") == 0x104);
    CHECK(IdOf("sys:0xzz") == kInvalidId);
    for (MsgId id : {MsgId{0x40}, MsgId{0x41}, MsgId{0x42}, MsgId{0x103}, MsgId{0x104}, MsgId{0x1004}, MsgId{0x7ffe}})
        CHECK(IdOf(NameOf(id)) == id);

    // ------------------------------------------------------------ dispatch: Post fan-out, fromPost, depth, handle
    Section("dispatch: Post -> 3 Deliveries of the same object");
    const MsgId turn = Id(kSlotTurn);
    Recorder postRec, delRec;
    SubId postSub = Subscribe(turn, Path::Post, &Record, &postRec);
    SubId delSub = Subscribe(turn, Path::Deliver, &Record, &delRec);
    CHECK(postSub && delSub && postSub != delSub);
    FakeMsg turnMsg(0x81aa14, turn, 8);
    const uint32_t postsBefore = CountOf(turn, Path::Post), delsBefore = CountOf(turn, Path::Deliver);
    int r = detail::RunPost(turnMsg.raw(), 0x1111, &PostFanOut, nullptr);
    CHECK(r == 42);
    CHECK(postRec.seen.size() == 1);
    if (postRec.seen.size() == 1) {
        const Seen& s = postRec.seen[0];
        CHECK(s.path == Path::Post);
        CHECK(s.handle == 7);  // *(*(0x96d090) + 0x14)
        CHECK(s.depth == 1);
        CHECK(!s.fromPost);
        CHECK(s.caller == 0x1111);
        CHECK(s.size == 8);
        CHECK(s.name == "GameLogic.Turn.Started");
        CHECK(s.cls == "Message");
    }
    CHECK(delRec.seen.size() == 3);
    for (size_t i = 0; i < delRec.seen.size(); ++i) {
        const Seen& s = delRec.seen[i];
        CHECK(s.path == Path::Deliver);
        CHECK(s.fromPost);
        CHECK(s.depth == 2);
        CHECK(s.handle == static_cast<int32_t>(101 + i));
        CHECK(s.caller == 0x2222);
        CHECK(s.seq > postRec.seen[0].seq);
        if (i) CHECK(s.seq > delRec.seen[i - 1].seq);
    }
    CHECK(CountOf(turn, Path::Post) == postsBefore + 1);
    CHECK(CountOf(turn, Path::Deliver) == delsBefore + 3);

    // a queued delivery (not inside a Post) has fromPost = false, depth 1, handle as passed (-1 = root)
    delRec.seen.clear();
    detail::RunDeliver(nullptr, turnMsg.raw(), -1, 1, 0x3333, &DeliverOrig, nullptr);
    CHECK(delRec.seen.size() == 1 && !delRec.seen[0].fromPost && delRec.seen[0].depth == 1 &&
          delRec.seen[0].handle == -1);

    // other ids do not reach these subscribers; the lazily resolved name subscriptions work
    FakeMsg camMsg(0x81aa14, Id(kSlotCamera), 8);
    postRec.seen.clear();
    detail::RunPost(camMsg.raw(), 0, &PostNoop, nullptr);
    CHECK(postRec.seen.empty());
    CHECK(camRec.seen.size() == 1 && camRec.seen[0].name == "Camera.HasUpdated");
    FakeMsg lateMsg(0x81aa14, Id(kSlotLate), 8);
    detail::RunPost(lateMsg.raw(), 0, &PostNoop, nullptr);
    CHECK(lateNameRec.seen.size() == 1);
    Unsubscribe(lateNameSub);

    // Unsubscribe stops delivery
    Unsubscribe(delSub);
    delRec.seen.clear();
    detail::RunPost(turnMsg.raw(), 0, &PostFanOut, nullptr);
    CHECK(delRec.seen.empty());
    Unsubscribe(delSub);  // twice: harmless
    Unsubscribe(0);

    // ------------------------------------------------------------ subscribe / unsubscribe inside a handler
    Section("dispatch: (un)subscribe from inside a handler");
    FakeMsg exMsg(0x854184, Id(kSlotExplosion), 0x4c);
    g_selfSub = Subscribe(Id(kSlotExplosion), Path::Post, &SelfUnsub);
    SubId spawner = Subscribe(Id(kSlotExplosion), Path::Post, &Spawner);
    detail::RunPost(exMsg.raw(), 0, &PostNoop, nullptr);
    CHECK(g_selfCalls == 1);
    CHECK(g_spawnCalls == 1);
    CHECK(g_lateSub != 0);
    CHECK(g_lateRec.seen.empty());  // takes effect for the next message
    detail::RunPost(exMsg.raw(), 0, &PostNoop, nullptr);
    CHECK(g_selfCalls == 1);  // unsubscribed itself
    CHECK(g_spawnCalls == 2);
    CHECK(g_lateRec.seen.size() == 1);
    Unsubscribe(spawner);
    Unsubscribe(g_lateSub);
    detail::Tick();  // frees retired tables (no dispatch in flight)

    // ------------------------------------------------------------ faults
    Section("faults: a handler that faults 3 times is disabled");
    const Stats s0 = GetStats();
    Recorder survivor;
    SubId faultSub = Subscribe(Id(kSlotCamera), Path::Post, &Faulter);
    SubId survSub = Subscribe(Id(kSlotCamera), Path::Post, &Record, &survivor);
    for (int i = 0; i < 6; ++i) detail::RunPost(camMsg.raw(), 0, &PostNoop, nullptr);
    const Stats s1 = GetStats();
    CHECK(g_faultCalls == 3);
    CHECK(s1.handlerFaults - s0.handlerFaults == 3);
    CHECK(survivor.seen.size() == 6);
    Unsubscribe(faultSub);
    Unsubscribe(survSub);

    // ------------------------------------------------------------ SubscribeAll
    Section("SubscribeAll");
    Recorder allRec;
    SubId allSub = SubscribeAll(Path::Deliver, &Record, &allRec);
    detail::RunPost(camMsg.raw(), 0, &PostFanOut, nullptr);
    detail::RunDeliver(nullptr, exMsg.raw(), 5, 0, 0, &DeliverOrig, nullptr);
    CHECK(allRec.seen.size() == 4);
    Unsubscribe(allSub);
    allRec.seen.clear();
    detail::RunDeliver(nullptr, exMsg.raw(), 5, 0, 0, &DeliverOrig, nullptr);
    CHECK(allRec.seen.empty());
    Unsubscribe(camSub);

    // ------------------------------------------------------------ MessageView::Read bounds
    Section("MessageView::Read bounds and size header");
    {
        FakeMsg m(0x8850d4, Id(20), 12);  // IntMessage: +8 i32
        m.Put<int32_t>(8, -5);
        MessageView v = ViewOf(m);
        CHECK(v.size == 12);
        int32_t x = 0;
        CHECK(v.Get(8, x) && x == -5);
        CHECK(!v.Get(9, x));        // crosses the end
        CHECK(!v.Get(12, x));       // past the end
        CHECK(!v.Read(0xffffffff, &x, 4));
        CHECK(v.Read(12, &x, 0));   // empty read at the end
        m.SetHeader(3);             // bogus header
        CHECK(detail::ObjectSize(m.raw()) == 0);
        m.SetHeader(0x100000);
        CHECK(detail::ObjectSize(m.raw()) == 0);
        v = ViewOf(m);
        CHECK(!v.Get(8, x));
    }

    // ------------------------------------------------------------ decoders
    Section("decoders");
    {
        const float dmg[3] = {1.5f, 2.5f, -3.0f}, imp[3] = {4, 5, 6}, dir[3] = {0, 1, 0};
        exMsg.PutVec(0x08, dmg);
        exMsg.PutVec(0x14, imp);
        exMsg.PutVec(0x20, dir);
        exMsg.Put(0x2c, 50.0f);
        exMsg.Put(0x30, 7.0f);
        exMsg.Put(0x34, 8.0f);
        exMsg.Put(0x38, 9.0f);
        exMsg.Put(0x3c, 10.0f);
        MessageView v = ViewOf(exMsg);
        CHECK(v.size == 0x4c);
        CHECK(strcmp(v.className, "ExplosionMessage") == 0);
        Collect c;
        CHECK(Decode(v, c));
        CHECK(c.vec["damageEpicentre"] == std::vector<float>({1.5f, 2.5f, -3.0f}));
        CHECK(c.vec["impulseEpicentre"] == std::vector<float>({4, 5, 6}));
        CHECK(c.vec["dir"] == std::vector<float>({0, 1, 0}));
        CHECK(c.num["wormDamage"] == 50.0);
        CHECK(c.num["impulse"] == 7.0 && c.num["wormDamageRadius"] == 8.0 && c.num["landDamageRadius"] == 9.0 &&
              c.num["impulseRadius"] == 10.0);
        for (auto& [k, f] : c.num) CHECK(std::isfinite(f));

        FakeMsg task(0x851f24, Id(30), 12);
        task.Put<uint32_t>(8, 0xdeadbeef);
        Collect ct;
        CHECK(Decode(ViewOf(task), ct) && ct.num["value"] == static_cast<double>(0xdeadbeef));

        static const char kHello[] = "hello";
        FakeMsg str(0x884fb4, Id(31), 16);  // TwoStringMessage
        str.Put<uint32_t>(8, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kHello)));
        str.Put<uint32_t>(0xc, 0x10);  // unreadable pointer: must not crash
        Collect cs;
        CHECK(Decode(ViewOf(str), cs));
        CHECK(cs.str["a"] == "hello");
        CHECK(cs.str.count("b") && cs.str["b"].empty());

        FakeMsg pe(0x85be94, Id(32), 52);
        pe.Put<uint32_t>(8, 77);
        pe.Put(0xc, 1.25f);
        Collect cp;
        CHECK(Decode(ViewOf(pe), cp) && cp.num["payloadId"] == 77 && cp.num["time"] == 1.25 && cp.vec.count("pos") &&
              cp.vec.count("vel"));

        FakeMsg bare(0x81aa14, Id(33), 8);
        Collect cb;
        CHECK(Decode(ViewOf(bare), cb) && cb.num.empty() && cb.str.empty());

        FakeMsg unk(0x123456, Id(34), 16);
        Collect cu;
        CHECK(strcmp(ViewOf(unk).className, "?") == 0);
        CHECK(!Decode(ViewOf(unk), cu));
        CHECK(RegisterDecoder(0x123456, "MyModMessage", [](const MessageView& m, JsonOut& o) {
            uint32_t x;
            if (m.Get(8, x)) o.Uint("x", x);
        }));
        unk.Put<uint32_t>(8, 99);
        CHECK(strcmp(ViewOf(unk).className, "MyModMessage") == 0);
        CHECK(Decode(ViewOf(unk), cu) && cu.num["x"] == 99);
        // replacing keeps working, a faulting decoder is contained
        CHECK(RegisterDecoder(0x123456, "MyModMessage", [](const MessageView&, JsonOut&) { *g_null = 2; }));
        CHECK(!Decode(ViewOf(unk), cu));
        CHECK(!RegisterDecoder(0, "x", nullptr));
    }

    // ------------------------------------------------------------ hot path cost with no subscribers
    Section("performance: hot path with nothing subscribed");
    {
        FakeMsg quiet(0x81aa14, Id(40), 8);
        const int n = 2000000;
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        for (int i = 0; i < n; ++i) detail::RunPost(quiet.raw(), 0, &PostNoop, nullptr);
        QueryPerformanceCounter(&b);
        const double ns = static_cast<double>(b.QuadPart - a.QuadPart) * 1e9 / static_cast<double>(f.QuadPart) / n;
        const Stats st = GetStats();
        printf("  RunPost, no subscribers: %.1f ns/call (incl. the fake original); Stats.avgHookUs=%.4f; posts=%llu "
               "deliveries=%llu handlerCalls=%llu handlerFaults=%llu\n",
               ns, st.avgHookUs, static_cast<unsigned long long>(st.posts), static_cast<unsigned long long>(st.deliveries),
               static_cast<unsigned long long>(st.handlerCalls), static_cast<unsigned long long>(st.handlerFaults));
        CHECK(ns < 100.0);
        CHECK(st.avgHookUs > 0.0 && st.avgHookUs < 0.5);
        CHECK(CountOf(Id(40), Path::Post) == static_cast<uint32_t>(n));
    }

    detail::Tick();
    printf("%s: %d checks, %d failed\n", g_failed ? "FAIL" : "PASS", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
