// GameState: read-only worms, teams, match values, entities and data variables of build #1077 (melange/gamestate.h).
// Nothing here calls a setter or caches a container pointer; the only engine calls are the data store's getters
// (GetResource, Enumerate), the Rm.cpp GetInt wrapper inside a match and LandRay's land sweep (whose globals are put
// back afterwards), each behind a fault guard.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <float.h>
#include <mutex>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "game/state/gamestate_internal.h"
#include "lua/engine50.h"
#include "melange/bus.h"
#include "melange/gamestate.h"
#include "melange/sim.h"

namespace melange::gamestate {
namespace {
using detail::Rd;
constexpr uintptr_t kXomRoot = 0x966d98, kDrmIid = 0x888288, kDrmVt = 0x8887cc, kGetInt = 0x50b790;
constexpr detail::Layout kLayout{0x95b4a8, 0x95b528, 0x8747d4, 0x874e40, 0x96d030};

std::atomic<bool> g_installed{false};
std::atomic<bool> g_online{false};
std::atomic<uint32_t> g_wantNow{0};

// Main thread only.
uint64_t g_bucketStart = 0, g_lastPeriodic = 0;
uint32_t g_wantCur = 0, g_wantPrev = 0;
void* g_vm = nullptr;
bus::MsgId g_turnId = bus::kInvalidId, g_sdId = bus::kInvalidId;
uint32_t g_turnBase = 0, g_sdBase = 0, g_prevTurns = 0, g_prevSd = 0;
Snapshot g_cache{};
uint64_t g_cacheFrame = 0;
bool g_cacheOk = false, g_inVars = false;

SRWLOCK g_latestLock = SRWLOCK_INIT;
Snapshot g_latest{};
bool g_haveLatest = false;

bool Check() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        if (!game::IsKnownBuild()) return;
        ok = mem::Expect(0x50b790, {0xe8, 0x88, 0xe3, 0x12, 0x00, 0x8b, 0x08, 0x8b, 0x51, 0x54, 0x68, 0x88, 0x82, 0x88, 0x00}) &&
             mem::Expect(0x50be0e, {0x8b, 0x34, 0xb5, 0xa8, 0xb4, 0x95, 0x00, 0x8b, 0x06, 0x8b, 0x48, 0x10}) &&
             mem::Expect(0x50bf7e, {0x8b, 0x34, 0xb5, 0x28, 0xb5, 0x95, 0x00, 0x8b, 0x06, 0x8b, 0x48, 0x10}) &&
             mem::Expect(0x639b1d, {0x83, 0x3d, 0x98, 0x6d, 0x96, 0x00, 0x00}) &&
             mem::Expect(0x6a2340, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x00, 0x89, 0x44, 0x24, 0x04, 0xff, 0x25, 0x88, 0xee, 0x96, 0x00}) &&
             mem::Expect(0x888838, {0xa0, 0x3e, 0x6a, 0x00, -1, -1, -1, -1, 0xf0, 0x3e, 0x6a, 0x00}) &&
             mem::Expect(0x69d990, {0x8b, 0x44, 0x24, 0x04, 0x8b, 0x48, 0x04, 0x8b, 0x41, 0x14, 0xc2, 0x04, 0x00}) &&
             mem::Expect(0x69dab0, {0x8b, 0x49, 0x04, 0x8b, 0x41, 0x1c, 0xc3}) &&
             mem::Expect(0x68c375, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20, 0xc7, 0x45, 0xf4, 0x00, 0x80, 0x00, 0x00});
        // Each data-store descriptor class's GetType (slot 4) returns the type id the reader assumes for it.
        const struct { uintptr_t vt, fn; int id; } kTypes[] = {
            {0x887b74, 0x6b5380, 0}, {0x887bbc, 0x6a0110, 1}, {0x887c04, 0x69ebb0, 2}, {0x887c4c, 0x69dbf0, 3},
            {0x887cdc, 0x6c1230, 4}, {0x887d74, 0x6a63d0, 5}, {0x887d24, 0x6c1690, 6}, {0x887c94, 0x6b52d0, 7},
        };
        for (const auto& t : kTypes) {
            if (!ok) break;
            ok = Rd<uintptr_t>(t.vt + 0x10) == t.fn &&
                 (t.id ? mem::Expect(t.fn, {0xb8, t.id, 0x00, 0x00, 0x00, 0xc2, 0x04, 0x00})
                       : mem::Expect(t.fn, {0x33, 0xc0, 0xc2, 0x04, 0x00}));
        }
        if (!ok) LOG_WARN("[gamestate] reader layout differs from build #1077: game-state readers are off");
    });
    return ok;
}

template <class F>
F Method(uintptr_t obj, size_t off) {
    return reinterpret_cast<F>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off));
}

uintptr_t Drm() {
    const uintptr_t root = Rd<uintptr_t>(kXomRoot);
    if (!root) return 0;
    uintptr_t drm = 0;
    __try {
        drm = Method<uintptr_t(__stdcall*)(uintptr_t, uintptr_t)>(root, 0x54)(root, kDrmIid);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return drm && Rd<uintptr_t>(drm) == kDrmVt ? drm : 0;
}

void Release(uintptr_t desc) {
    __try {
        Method<uintptr_t(__stdcall*)(uintptr_t)>(desc, 0x8)(desc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

uintptr_t GetResource(const char* name) {
    const uintptr_t drm = Drm();
    if (!drm) return 0;
    const char* n = name;
    uintptr_t obj = 0;
    int rc = -1;
    __try {
        rc = Method<int(__stdcall*)(uintptr_t, const char**, uintptr_t*)>(drm, 0x74)(drm, &n, &obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    if (rc < 0 && obj) Release(obj);
    return rc >= 0 ? obj : 0;
}

bool Enumerate(detail::EnumCb cb, void* ctx) {
    const uintptr_t drm = Drm();
    if (!drm) return false;
    __try {
        Method<int(__stdcall*)(uintptr_t, detail::EnumCb, void*)>(drm, 0x6c)(drm, cb, ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

bool GetInt(const char* name, int32_t* out) {
    const char* n = name;
    __try {
        reinterpret_cast<void(__cdecl*)(const char**, int32_t*)>(kGetInt)(&n, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr detail::Engine kEngine{&GetInt, &GetResource, &Release, &Enumerate};

bool MainThread() { return events::FrameCount() > 0 && GetCurrentThreadId() == events::MainThreadId(); }

void LogRayStats();  // LandRay, below

uint32_t Count(bus::MsgId id) {
    return id == bus::kInvalidId ? 0 : bus::CountOf(id, bus::Path::Post) + bus::CountOf(id, bus::Path::Deliver);
}

// Turns and sudden death come from the bus's own message counters, rebased when a match VM appears: no handler.
void TrackMatch() {
    if (g_turnId == bus::kInvalidId && bus::Installed() && bus::RegistryReady()) {
        g_turnId = bus::IdOf("GameLogic.Turn.Started");
        g_sdId = bus::IdOf("GameLogic.ActivateSuddenDeath");
    }
    // The first turn can start in the very frame the VM appears, so rebase on the counts seen one frame earlier.
    const uint32_t turns = g_turnId == bus::kInvalidId ? 0 : bus::CountOf(g_turnId, bus::Path::Post), sd = Count(g_sdId);
    void* vm = lua50::MatchState();
    if (vm != g_vm) {
        g_vm = vm;
        if (vm) {
            g_turnBase = g_prevTurns;
            g_sdBase = g_prevSd;
        } else {
            LogRayStats();
        }
    }
    g_prevTurns = turns;
    g_prevSd = sd;
}

detail::MatchInfo Info() {
    detail::MatchInfo m{};
    m.inMatch = sim::InMatch();
    m.online = g_online.load();
    m.serial = sim::MatchSerial();
    if (m.inMatch && g_turnId != bus::kInvalidId) {
        m.turnsStarted = bus::CountOf(g_turnId, bus::Path::Post) - g_turnBase;
        m.suddenDeath = Count(g_sdId) != g_sdBase;
    }
    return m;
}

void Rotate(uint64_t now) {
    if (now - g_bucketStart < 1000) return;
    const bool adjacent = now - g_bucketStart < 2000;
    g_wantPrev = adjacent ? g_wantCur : 0;
    g_wantCur = 0;
    g_bucketStart = now;
}

void OnFrame() {
    TrackMatch();
    if (!g_wantNow.load(std::memory_order_relaxed)) return;
    const uint64_t now = GetTickCount64();
    Rotate(now);
    const uint32_t hz = (std::max)(g_wantCur, g_wantPrev);
    g_wantNow = hz;
    if (!hz || now - g_lastPeriodic < 1000 / hz) return;
    g_lastPeriodic = now;
    Snapshot s;
    Read(&s);
}

class GameState final : public Module {
  public:
    const char* Name() const override { return "GameState"; }
    const char* Description() const override { return "read-only worms, teams, match values and entities (Oasis, wum.game)"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 45; }
    bool Install() override {
        if (!Check()) return false;
        const uintptr_t base = game::Base();
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
        detail::SetRttiRange(base, base + nt->OptionalHeader.SizeOfImage);
        events::Subscribe(events::Event::Frame, &OnFrame);
        events::Subscribe(events::Event::MatchStart, [] { g_online = true; });
        events::Subscribe(events::Event::MatchEnd, [] { g_online = false; });
        g_installed = true;
        LOG_INFO("[gamestate] readers ready");
        return true;
    }
};
}  // namespace

bool Available() { return g_installed.load() && Check(); }

bool Read(Snapshot* out) {
    if (!out || !Available() || !MainThread()) return false;
    const uint64_t frame = events::FrameCount();
    if (g_cacheOk && g_cacheFrame == frame) {
        *out = g_cache;
        return true;
    }
    TrackMatch();
    Snapshot s;
    s.frame = frame;
    detail::FillSnapshot(kLayout, kEngine, Info(), &s);
    g_cache = s;
    g_cacheFrame = frame;
    g_cacheOk = true;
    AcquireSRWLockExclusive(&g_latestLock);
    g_latest = s;
    g_haveLatest = true;
    ReleaseSRWLockExclusive(&g_latestLock);
    *out = s;
    return true;
}

bool Latest(Snapshot* out) {
    if (!out) return false;
    AcquireSRWLockShared(&g_latestLock);
    const bool ok = g_haveLatest;
    if (ok) *out = g_latest;
    ReleaseSRWLockShared(&g_latestLock);
    return ok;
}

// A request lasts one to two seconds: callers renew it at least once a second, and the rate is the highest
// renewed in that window.
void Want(uint32_t hz) {
    if (!hz || !MainThread()) return;
    const uint64_t now = GetTickCount64();
    Rotate(now);
    g_wantCur = (std::max)(g_wantCur, (std::min)(hz, 10u));
    g_wantNow = (std::max)(g_wantCur, g_wantPrev);
}

int Entities(Entity* out, int max) {
    if (!out || max < 0 || !Available() || !MainThread() || !sim::InMatch()) return 0;
    return detail::WalkEntities(kLayout, out, max);
}

int Vars(Var* out, int max, const char* prefix) {
    if (max < 0 || !Available() || !MainThread() || g_inVars) return 0;
    g_inVars = true;
    const int n = detail::EnumerateVars(kEngine, out, max, prefix);
    g_inVars = false;
    return n < 0 ? 0 : n;
}

bool Var1(const char* name, Var* out) {
    if (!out || !Available() || !MainThread()) return false;
    return detail::ReadVar1(kEngine, name, out);
}

bool Peek(uintptr_t addr, void* out, uint32_t n) { return detail::PeekGuarded(addr, out, n); }

// ---------------------------------------------------------------- LandRay
// The engine's straight sweep, as its camera ray cast 0x51b150 runs it: 0x466ae0(origin, step, accel 0, 1000 ticks,
// 1, colliders 0, 0), then the hit's normal from the LandCollisionMessage getter 0x482010. The query lives in globals
// that the simulation can still read after the call (payload code dispatches a message between its sweep and reading
// the normal), and the broadphase leaves a frame bitmask in the landscape: all of it is saved and put back, so a mod's
// ray never changes what the sim sees. The sweep's call tree (0x473190, 0x462010, 0x46a070 and helpers) has no
// indirect calls and writes only those globals and that bitmask (checked in Ghidra).
namespace {
constexpr uintptr_t kSweep = 0x466ae0, kLandNormal = 0x482010, kLandscape = 0x955638, kHeightmap = 0x952ad4,
                    kFrames = 0x955740, kLandVt = 0x81c810, kHeightmapVt = 0x81babc, kFilter = 0x952d00,
                    kHitTick = 0x952cfc, kHitTime = 0x952cf8;
constexpr struct { uintptr_t at; uint32_t n; } kScratch[] = {
    {0x94ef8c, 0x4}, {0x952ab0, 0x78}, {0x952c28, 0x148}, {0x952f50, 0x16c}};
constexpr uint32_t kScratchBytes = 0x4 + 0x78 + 0x148 + 0x16c;
constexpr uint32_t kMaskAt = 0xc8, kMaskBytes = 0x80, kMaxFrames = kMaskBytes * 8;

bool g_landFaulted = false;  // main thread only, like everything below
detail::FrameBudget g_landBudget;
uint32_t g_rayCalls = 0;
uint64_t g_rayTicks = 0, g_rayMaxTicks = 0;

bool LandCheck() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        if (!Check()) return;
        ok = mem::Expect(0x466ae0, {0x83, 0xec, 0x30, 0x8b, 0x44, 0x24, 0x48, 0x8b, 0x4c, 0x24, 0x4c, 0x55}) &&
             // hit tick = ticks + 1 (no hit), hit time = that as a float
             mem::Expect(0x466f97, {0x8d, 0x4d, 0x01, 0x89, 0x0d, 0xfc, 0x2c, 0x95, 0x00, 0xdb, 0x05, 0xfc, 0x2c, 0x95, 0x00}) &&
             mem::Expect(0x466fb3, {0xd9, 0x1d, 0xf8, 0x2c, 0x95, 0x00}) &&
             // the landscape, then the heightmap, swept
             mem::Expect(0x466fed, {0x8b, 0x0d, 0x38, 0x56, 0x95, 0x00}) &&
             mem::Expect(0x46701d, {0xe8, 0x6e, 0xc1, 0x00, 0x00, 0x8b, 0x0d, 0xd4, 0x2a, 0x95, 0x00, 0x85, 0xc9, 0x74,
                                    0x05, 0xe8, 0xdf, 0xaf, 0xff, 0xff}) &&
             // the broadphase bitmask at landscape+0xc8, one bit per land frame
             mem::Expect(0x4731a1, {0xe8, 0xba, 0xf7, 0xff, 0xff, 0x8b, 0x35, 0x40, 0x57, 0x95, 0x00}) &&
             mem::Expect(0x472c39, {0x89, 0xb4, 0xba, 0xc8, 0x00, 0x00, 0x00}) &&
             // the normal getter returns &normal (0x952d64)
             mem::Expect(0x482010, {0x80, 0x3d, 0x4a, 0x2c, 0x95, 0x00, 0x00, 0x75, 0x24, 0xb8, 0xff, 0xff, 0x00, 0x00,
                                    0x66, 0x39, 0x05, 0xe8, 0x2c, 0x95, 0x00}) &&
             mem::Expect(0x48203d, {0xb8, 0x64, 0x2d, 0x95, 0x00, 0xc3}) &&
             mem::Expect(0x47330b, {0x8b, 0x15, 0x44, 0x57, 0x95, 0x00, 0x2b, 0x15, 0x40, 0x57, 0x95, 0x00});
        char name[64];
        bool payload = false;
        ok = ok && detail::Rtti(kLandVt, name, sizeof name, &payload) && strcmp(name, "LandscapeLogicEntity") == 0 &&
             detail::Rtti(kHeightmapVt, name, sizeof name, &payload) && strcmp(name, "HeightmapLogicEntity") == 0;
        if (!ok) LOG_WARN("[gamestate] land sweep differs from build #1077: wum.game.landRay is off");
    });
    return ok;
}

bool Poke(uintptr_t addr, const void* in, size_t n) {
    __try {
        memcpy(reinterpret_cast<void*>(addr), in, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using SweepFn = void(__cdecl*)(const float* origin, const float* step, const float* accel, int ticks, int flag,
                               int colliders, int arg);
using NormalFn = const float*(__cdecl*)();

// The raw engine calls; no C++ objects with destructors here (SEH).
bool CallSweep(const float* origin, const float* step, int ticks, int* hitTick, float* hitTime, float* normal) {
    static const float kZero[3] = {0.f, 0.f, 0.f};
    __try {
        *reinterpret_cast<volatile uint8_t*>(kFilter) = 0;  // no frame filter (some sim callers set it around a sweep)
        reinterpret_cast<SweepFn>(kSweep)(origin, step, kZero, ticks, 1, 0, 0);
        *hitTick = *reinterpret_cast<const volatile int32_t*>(kHitTick);
        *hitTime = *reinterpret_cast<const volatile float*>(kHitTime);
        if (*hitTick != ticks + 1) {
            const float* n = reinterpret_cast<NormalFn>(kLandNormal)();
            normal[0] = n[0], normal[1] = n[1], normal[2] = n[2];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EngineSweep(const Vec3& origin, const Vec3& step, int ticks, detail::RawLandHit* out) {
    const uintptr_t land = Rd<uintptr_t>(kLandscape), hm = Rd<uintptr_t>(kHeightmap);
    if (land) {
        const uintptr_t b = Rd<uintptr_t>(kFrames), e = Rd<uintptr_t>(kFrames + 4);
        if (Rd<uintptr_t>(land) != kLandVt || e < b || (e - b) % 4 || (e - b) / 4 > kMaxFrames || (e != b && !b))
            return false;
    }
    if (hm && Rd<uintptr_t>(hm) != kHeightmapVt) return false;
    static uint8_t saved[kScratchBytes], mask[kMaskBytes];
    uint32_t off = 0;
    for (const auto& s : kScratch) {
        if (!detail::Copy(s.at, saved + off, s.n)) return false;
        off += s.n;
    }
    if (land && !detail::Copy(land + kMaskAt, mask, kMaskBytes)) return false;
    const float o[3] = {origin.x, origin.y, origin.z}, st[3] = {step.x, step.y, step.z};
    int32_t tick = ticks + 1;
    float time = 0.f, n[3] = {0.f, 0.f, 0.f};
    unsigned int x87 = 0, sse = 0, unused = 0;
    __control87_2(0, 0, &x87, &sse);
    const bool ran = CallSweep(o, st, ticks, &tick, &time, n);
    if (!ran) {  // a fault mid-sweep can leave values on the x87 stack: empty it, keep the game's control words
        constexpr unsigned int kAll = _MCW_DN | _MCW_EM | _MCW_IC | _MCW_RC | _MCW_PC;
        _fpreset();
        __control87_2(x87, kAll, &unused, nullptr);
        __control87_2(sse, kAll, nullptr, &unused);
    }
    off = 0;
    bool restored = true;
    for (const auto& s : kScratch) {
        restored &= Poke(s.at, saved + off, s.n);
        off += s.n;
    }
    if (land) restored &= Poke(land + kMaskAt, mask, kMaskBytes);
    if (!ran || !restored) {
        g_landFaulted = true;
        LOG_WARN("[gamestate] the land sweep faulted (%s): wum.game.landRay is off for this session",
                 ran ? "restoring its globals" : "in the engine");
        return false;
    }
    *out = detail::RawLandHit{tick != ticks + 1, time, Vec3{n[0], n[1], n[2]}};
    return true;
}

void LogRayStats() {
    if (!g_rayCalls) return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double us = 1e6 / static_cast<double>(f.QuadPart);
    LOG_INFO("[gamestate] landRay: %u calls this match, %.1f us average, %.1f us max", g_rayCalls,
             static_cast<double>(g_rayTicks) * us / g_rayCalls, static_cast<double>(g_rayMaxTicks) * us);
    g_rayCalls = 0;
    g_rayTicks = g_rayMaxTicks = 0;
}
}  // namespace

LandRayResult LandRay(const Vec3& a, const Vec3& b, LandHit* out) {
    if (!out || !detail::InWorld(a) || !detail::InWorld(b)) return LandRayResult::Invalid;
    if (!Available() || !MainThread() || !LandCheck() || g_landFaulted) return LandRayResult::Unavailable;
    if (!sim::InMatch()) return LandRayResult::Miss;
    if (!g_landBudget.Take(events::FrameCount(), kLandRaysPerFrame)) return LandRayResult::Budget;
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const LandRayResult r = detail::SegmentLandRay(&EngineSweep, a, b, out);
    QueryPerformanceCounter(&t1);
    const uint64_t dt = static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
    ++g_rayCalls;
    g_rayTicks += dt;
    g_rayMaxTicks = (std::max)(g_rayMaxTicks, dt);
    return r;
}

MELANGE_MODULE(GameState);
}  // namespace melange::gamestate
