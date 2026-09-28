// GameState: read-only worms, teams, match values, entities and data variables of build #1077 (melange/gamestate.h).
// Nothing here calls a setter or caches a container pointer; the only engine calls are the data store's getters
// (GetResource, Enumerate) and the Rm.cpp GetInt wrapper inside a match, each behind a fault guard.
#include <windows.h>

#include <algorithm>
#include <atomic>
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
uint32_t g_turnBase = 0, g_sdBase = 0;
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

uint32_t Count(bus::MsgId id) {
    return id == bus::kInvalidId ? 0 : bus::CountOf(id, bus::Path::Post) + bus::CountOf(id, bus::Path::Deliver);
}

// Turns and sudden death come from the bus's own message counters, rebased when a match VM appears: no handler.
void TrackMatch() {
    if (g_turnId == bus::kInvalidId && bus::Installed() && bus::RegistryReady()) {
        g_turnId = bus::IdOf("GameLogic.Turn.Started");
        g_sdId = bus::IdOf("GameLogic.ActivateSuddenDeath");
    }
    void* vm = lua50::MatchState();
    if (vm == g_vm) return;
    g_vm = vm;
    if (!vm) return;
    g_turnBase = g_turnId == bus::kInvalidId ? 0 : bus::CountOf(g_turnId, bus::Path::Post);
    g_sdBase = Count(g_sdId);
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

MELANGE_MODULE(GameState);
}  // namespace melange::gamestate
