#include "wormsign/hash_engine.h"

#include <windows.h>

#include <cstring>

#include "core/mem.h"
#include "game/state/gamestate_internal.h"
#include "wormsign/detail.h"

namespace melange::wormsign {
namespace {
constexpr uintptr_t kTM = 0x96d030;
constexpr uintptr_t kRngLogicAddr = 0x96d034, kRng2Addr = 0x96d040;
constexpr uintptr_t kGetInt = 0x50b790;  // cdecl(const char** name, int* out)
constexpr uintptr_t kWormHandles = 0x95b4a8, kWormVt = 0x8747d4, kTeamHandles = 0x95b528, kTeamVt = 0x874e40;
constexpr uintptr_t kQueueBytesMax = 8 * 4096;
// CameraManagerService* (null outside a match); +0x28c m_uLogicalCamera, +0x2a0/+0x2a4 the Camera* vector's
// begin/end, as GetLogicalViewMatrix 0x562730 and the pos/target/up getters 0x547600/0x547640/0x547680 read them.
constexpr uintptr_t kCamMgr = 0x95c370;
constexpr uint32_t kMaxCameras = 64;

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

// Pure, fault-guarded reads; flags stay 0 unless the manager, the index and every camera field could be read.
void ReadCamera(detail::CameraDetail* c) {
    *c = detail::CameraDetail{};
    uintptr_t mgr = 0;
    if (!mem::SafeRead(kCamMgr, &mgr, sizeof mgr) || !mgr) return;
    uint32_t index = 0;
    uintptr_t vec[2] = {};
    if (!mem::SafeRead(mgr + 0x28c, &index, sizeof index) || !mem::SafeRead(mgr + 0x2a0, vec, sizeof vec)) return;
    if (vec[1] < vec[0] || (vec[1] - vec[0]) % 4) return;
    const uint32_t count = static_cast<uint32_t>((vec[1] - vec[0]) / 4);
    c->index = index;
    c->count = count;
    if (count > kMaxCameras || index >= count) return;
    uintptr_t cam = 0;
    if (!mem::SafeRead(vec[0] + index * 4, &cam, sizeof cam) || !cam) return;
    uint32_t f[11];  // +0x04 pos, +0x10 target, +0x1c up, +0x28 (unknown, not kept), +0x2c m_eView
    if (!mem::SafeRead(cam + 4, f, sizeof f)) return;
    memcpy(c->pos, f, 12);
    memcpy(c->target, f + 3, 12);
    memcpy(c->up, f + 6, 12);
    c->view = f[10];
    c->flags = detail::kCamPresent;
}

// The camera of the tick being hashed, read once by ComputeEngine and fed to the contributor right after it.
// Consumed once: tick numbers restart every session, so any later HashTick for the same tick number (another
// session, or a caller other than session::EndTick) reads the camera again instead of reusing a stale one.
detail::CameraDetail g_cam{};
uint32_t g_camTick = 0;
bool g_camValid = false;
int g_camHandle = 0;

void CameraContrib(Hasher& h, uint32_t tick, void*) {
    detail::CameraDetail c;
    if (g_camValid && g_camTick == tick)
        c = g_cam;
    else
        ReadCamera(&c);
    g_camValid = false;
    uint8_t b[detail::kCameraHashBytes];
    h.Bytes(b, detail::CameraHashBytes(c, b));
}

int GetInt(const char* name) {
    const char* n = name;
    int v = -99;
    __try {
        reinterpret_cast<void(__cdecl*)(const char**, int*)>(kGetInt)(&n, &v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -98;
    }
    return v;
}

// Worm container bytes: collision normal, aftertouch, position, velocity, rotation, state and energy fields.
size_t WormBytes(uintptr_t w, uint8_t* out) {
    uint8_t buf[0x128];
    if (!mem::SafeRead(w, buf, sizeof buf)) return 0;
    size_t n = 0;
    auto put = [&](size_t off, size_t len) { memcpy(out + n, buf + off, len), n += len; };
    put(0x20, 0x24);
    put(0x50, 0x0c);
    put(0x8c, 0x0c);
    put(0xcc, 2), put(0xd0, 4), put(0xd4, 1), put(0xd8, 4), put(0xe0, 4), put(0xe4, 2);
    put(0xf0, 4), put(0xf4, 4), put(0xf8, 2), put(0x10c, 1), put(0x110, 1), put(0x11e, 2);
    put(0x124, 1), put(0x127, 1);
    return n;
}

// Projectile classification per vtable, cached without allocation after the first sight.
struct VtSlot {
    uintptr_t vt;
    bool projectile;
};
VtSlot g_vts[512];
bool IsProjectile(uintptr_t vt) {
    const size_t mask = sizeof g_vts / sizeof g_vts[0] - 1;
    size_t i = (vt >> 2) * 2654435761u & mask;
    for (size_t k = 0; k <= mask; ++k, i = (i + 1) & mask) {
        if (g_vts[i].vt == vt) return g_vts[i].projectile;
        if (!g_vts[i].vt) {
            const bool p = gamestate::detail::Classify(vt).kind == gamestate::EntityKind::Projectile;
            g_vts[i] = {vt, p};
            return p;
        }
    }
    return gamestate::detail::Classify(vt).kind == gamestate::EntityKind::Projectile;
}

uint32_t g_queue[kQueueBytesMax / 4];

void HashTasks(uint64_t& ht, uint64_t& hp, const PoppedTask& popped, detail::DetailRec* rec) {
    const uintptr_t inner = Rd<uintptr_t>(Rd<uintptr_t>(kTM) + 0x1c);
    for (int cat = 1; cat <= 2 && inner; ++cat) {
        const uintptr_t q = Rd<uintptr_t>(inner + 0x28 + cat * 0xc);
        const uintptr_t tbl = Rd<uintptr_t>(inner + 0x28 + cat * 0xc + 8);
        const uintptr_t base = tbl ? Rd<uintptr_t>(tbl) : 0;
        if (!q || !base) continue;
        const uintptr_t b = Rd<uintptr_t>(q + 0xc), e = Rd<uintptr_t>(q + 0x10);
        if (e < b || e - b > kQueueBytesMax) continue;
        const size_t words = (e - b) / 4;
        if (words && !mem::SafeRead(b, g_queue, words * 4)) continue;
        ht = FnvV(cat, ht);
        auto one = [&](uint32_t time, uintptr_t obj) {
            const uintptr_t vt = obj ? Rd<uintptr_t>(obj) : 0;
            ht = FnvV(vt, FnvV(time, ht));
            if (vt && IsProjectile(vt)) {
                float pv[6];
                if (mem::SafeRead(obj + 0x28, pv, sizeof pv)) {
                    hp = Fnv(pv, sizeof pv, FnvV(vt, hp));
                    if (rec && rec->projCount < 64) {
                        detail::ProjDetail& d = rec->proj[rec->projCount++];
                        d.vt = static_cast<uint32_t>(vt), d.time = time, d.cat = static_cast<uint8_t>(cat);
                        memcpy(d.pv, pv, sizeof pv);
                    }
                }
            }
        };
        if (popped.cat == cat) one(popped.time, popped.obj);
        for (size_t i = 0; i + 1 < words; i += 2) {
            const uint32_t handle = g_queue[i], time = g_queue[i + 1];
            uintptr_t obj = 0;
            if (handle != 0xffffffff) {
                const uintptr_t ent = base + (handle & 0xfff) * 0x24;
                if (Rd<uint32_t>(ent + 0x14) == handle) obj = Rd<uintptr_t>(ent + 0xc);
            }
            one(time, obj);
        }
    }
}
}  // namespace

void ComputeEngine(uint32_t t, TickHash* out, const PoppedTask& popped, detail::DetailRec* rec) {
    out->tick = t / kTickMs;
    out->rngLogic = Rd<uint32_t>(kRngLogicAddr);
    out->rng2 = Rd<uint32_t>(kRng2Addr);
    out->c[kTimeRng] = FnvV(out->rngLogic, FnvV(t));
    const int cur = GetInt("CurrentTeamIndex"), act = GetInt("ActiveWormIndex");
    out->c[kTurn] = FnvV(act, FnvV(cur));
    if (rec) {
        rec->tick = out->tick;
        rec->rng = out->rngLogic, rec->rng2 = out->rng2;
        rec->curTeam = cur, rec->activeWorm = act;
    }

    uint64_t hw = kFnvBasis;
    for (int i = 0; i < 16; ++i) {
        const uintptr_t c = gamestate::detail::Container(kWormHandles, i, kWormVt);
        if (!c) continue;
        uint8_t b[128];
        const size_t n = WormBytes(c, b);
        hw = Fnv(b, n, FnvV(i, hw));
        if (rec && rec->wormCount < 16) {
            detail::WormDetail& d = rec->worms[rec->wormCount++];
            const bool whole = n == detail::kWormBytes;
            d.slot = static_cast<uint8_t>(whole ? i : i | detail::kSlotUnreadable);
            if (whole)
                memcpy(d.bytes, b, n);
            else
                memset(d.bytes, 0, sizeof d.bytes);
        }
    }
    out->c[kWorms] = hw;

    uint64_t ht = kFnvBasis, hp = kFnvBasis;
    HashTasks(ht, hp, popped, rec);
    out->c[kTasks] = ht;
    out->c[kProjectiles] = hp;

    uint64_t hteam = kFnvBasis;
    for (int i = 0; i < 4; ++i) {
        const uintptr_t c = gamestate::detail::Container(kTeamHandles, i, kTeamVt);
        if (!c) continue;
        const uint8_t active = Rd<uint8_t>(c + 0x68);
        const uint32_t score = Rd<uint32_t>(c + 0x48);
        hteam = FnvV(score, FnvV(active, FnvV(i, hteam)));
        if (rec) rec->teams[i] = {static_cast<uint8_t>(i), active, score};
    }
    out->c[kTeams] = hteam;

    uint64_t all = kFnvBasis;
    for (int i = 0; i < kEngineComps; ++i) all = FnvV(out->c[i], all);
    out->engine = all;

    // Not part of the engine hash (that would change kEngineHashVersion and end the exchange with older Melange):
    // the camera goes to the detail record and to the melange.camera contributor.
    ReadCamera(&g_cam);
    g_camTick = out->tick;
    g_camValid = true;
    if (rec) rec->cam = g_cam;
}

bool AddCameraContributor() {
    if (g_camHandle) return true;
    ContribOptions opt;
    opt.version = kCameraContribVersion;
    g_camHandle = AddContributor(kCameraContrib, &CameraContrib, nullptr, opt);
    return g_camHandle > 0;
}

void RemoveCameraContributor() {
    if (g_camHandle > 0) RemoveContributor(g_camHandle);
    g_camHandle = 0;
    g_camValid = false;
}
}  // namespace melange::wormsign
