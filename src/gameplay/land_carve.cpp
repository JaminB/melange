// LandCarve: the engine side of wum.sim.carve (src/lua/sim/sim_land.cpp) for build #1077. Notes:
// docs/gravespade-re-carve.md.
//
// Every hole in the land is made by LandscapeLogicEntity::OnExplosion 0x473530 (thiscall(landscape; ExplosionMessage*),
// ret 4), on the one landscape *(0x955638) (vtable 0x81c810). It reads only the damage epicentre (+0x08) and the land
// damage radius (+0x38) of the message, removes every destructible voxel whose centre is in that sphere from every
// voxel frame (PERM frames, flag 0x10, never) and lowers the heightmap, then plays the vanilla debris and posts
// Land.NewShape so resting worms and objects re-check their footing. Water is a plane, not voxels: it is never carved.
//
// The carve calls that handler directly with a zeroed ExplosionMessage on the stack (vtable 0x854184; worm damage,
// impulse and their radii 0), so nothing is posted on the bus as an Explosion: worms, mines, drums, crates, the
// camera (the FlyCam post-explosion hold), the AI and level scripts see only Land.NewShape. The message's
// constructor 0x518ce0 is not called (it writes the global Explosion.* data values and applies DoubleDamage).
//
// "removed" is the drop of the per-frame u16 voxel counts at 0x955570 (index = frame +0x14), plus the heightmap cells
// under the sphere that went down by 1 world unit or more (Ground below; Land.Changed is set for any sphere inside
// the heightmap's box, so it is no measure). Explosion.HitIndestructible, which a carve touching a PERM
// frame sets, is put back so the spade never leaks it into a level script's next explosion check.
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "gameplay/engine_data.h"
#include "lua/engine50.h"
#include "lua/sim/sim_land.h"
#include "melange/bus.h"
#include "melange/gamestate.h"
#include "melange/sim.h"
#include "melange/wormsign.h"

namespace {
namespace sl = melange::simland;
namespace gd = melange::gameplay::data;
using melange::gamestate::detail::Rd;

constexpr uintptr_t kOnExplosion = 0x473530, kFrameCarve = 0x46d290, kBroadphase = 0x473060;
constexpr uintptr_t kLandscape = 0x955638, kLandscapeVt = 0x81c810, kExplosionVt = 0x854184;
constexpr uintptr_t kVoxelCounts = 0x955570;
constexpr int kCountSlots = (kLandscape - kVoxelCounts) / 2;  // 100: the array ends where the landscape pointer is

int g_contrib = 0, g_context = 0;
bool g_ready = false;
melange::bus::MsgId g_explosionId = melange::bus::kInvalidId;
uint32_t g_idSerial = ~0u;

bool CallOnExplosion(uintptr_t land, uint8_t* msg) {
    __try {
        reinterpret_cast<void(__thiscall*)(uintptr_t, uint8_t*)>(kOnExplosion)(land, msg);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The heightmap (HeightmapLogicEntity *(0x952ad4)): heights at +0x2c (float*, +0x30 the count), the box's extent at
// +0x64/+0x68/+0x6c, its min at 0x90c440, the grid at 0x90c42c (W) / 0x90c430 (H); +0x78 bit 2 is a second carve
// mode (0x461bb0) this does not model. Its carve 0x463c90 sets Land.Changed for any sphere inside the box and moves a
// cell half way to the sphere's floor, so `removed` counts the cells that actually went down (sim_land.h).
constexpr uintptr_t kHeightmap = 0x952ad4, kGridW = 0x90c42c, kGridH = 0x90c430, kGridMin = 0x90c440;
bool g_groundOk = false;  // Check() passed for the heightmap reads

struct Ground {
    bool absent = false;  // no heightmap on this level
    uintptr_t cells = 0;
    sl::GroundWindow win;
    float extentY = 0.f;
    float before[sl::kGroundWindowMax + 8][sl::kGroundWindowMax + 8];

    bool Snapshot(const float pos[3], float radius) {
        const uintptr_t hm = Rd<uintptr_t>(kHeightmap);
        if (!hm) {
            absent = true;
            return true;  // nothing to lower: the voxels are the whole answer
        }
        if (!g_groundOk || (Rd<uint8_t>(hm + 0x78) & 2)) return false;
        sl::HeightGrid g;
        g.w = Rd<int>(kGridW);
        g.h = Rd<int>(kGridH);
        cells = Rd<uintptr_t>(hm + 0x2c);
        const int count = Rd<int>(hm + 0x30);
        if (!cells || g.w <= 0 || g.h <= 0 || g.w > 4096 || g.h > 4096 || count < g.w * g.h) return false;
        if (!melange::gamestate::detail::Copy(kGridMin, g.min, sizeof g.min) ||
            !melange::gamestate::detail::Copy(hm + 0x64, g.extent, sizeof g.extent))
            return false;
        extentY = g.extent[1];
        // Off the grid or too wide to measure: the flag fallback (off the grid the engine leaves the flag unset).
        if (!sl::GroundFootprint(g, pos, radius, &win)) return false;
        for (int z = win.z0; z <= win.z1; ++z)
            if (!Row(g.w, z, before[z - win.z0])) return false;
        gridW = g.w;
        return true;
    }
    int Lowered() {
        int n = 0;
        float after[sl::kGroundWindowMax + 8];
        for (int z = win.z0; z <= win.z1; ++z)
            if (Row(gridW, z, after)) n += sl::CountLowered(before[z - win.z0], after, win.Width(), extentY);
        return n;
    }

private:
    int gridW = 0;
    bool Row(int w, int z, float* out) const {
        return melange::gamestate::detail::Copy(cells + 4u * (static_cast<uintptr_t>(z) * w + win.x0), out,
                                                 sizeof(float) * win.Width());
    }
};

class EngineLand final : public sl::Backend {
public:
    bool Carve(const float pos[3], float radius, sl::CarveOut* out) override {
        const uintptr_t land = Rd<uintptr_t>(kLandscape);
        if (!g_ready || !land || Rd<uintptr_t>(land) != kLandscapeVt) {
            out->why = "unavailable";
            return false;
        }
        if (gd::Flag("Land.Indestructable")) {
            out->why = "indestructible";
            return false;
        }
        const uint32_t serial = melange::sim::MatchSerial();
        if (serial != g_idSerial) {
            g_idSerial = serial;
            g_explosionId = melange::bus::IdOf("Explosion");
        }
        if (g_explosionId == melange::bus::kInvalidId) {
            out->why = "unavailable";
            return false;
        }
        uint16_t before[kCountSlots], after[kCountSlots];
        if (!melange::gamestate::detail::Copy(kVoxelCounts, before, sizeof before)) {
            out->why = "unavailable";
            return false;
        }
        Ground ground;
        const bool measured = ground.Snapshot(pos, radius);
        uintptr_t hitAt = 0;
        uint8_t hitWas[4] = {};
        const bool hitSaved = gd::Snapshot("Explosion.HitIndestructible", &hitAt, hitWas);

        alignas(4) uint8_t msg[0x50] = {};
        const uint32_t vt = kExplosionVt;
        memcpy(msg + 0x00, &vt, 4);
        memcpy(msg + 0x04, &g_explosionId, 2);
        memcpy(msg + 0x08, pos, 12);  // damage epicentre
        memcpy(msg + 0x14, pos, 12);  // impulse epicentre (unused: impulse 0)
        memcpy(msg + 0x38, &radius, 4);
        const bool ok = CallOnExplosion(land, msg);
        if (hitSaved) gd::Restore(hitAt, hitWas);
        if (!ok) {
            LOG_ERROR("[land] OnExplosion faulted at (%.1f, %.1f, %.1f) r=%.1f", pos[0], pos[1], pos[2], radius);
            out->why = "unavailable";
            return false;
        }
        int removed = 0;
        if (melange::gamestate::detail::Copy(kVoxelCounts, after, sizeof after))
            for (int i = 0; i < kCountSlots; ++i)
                if (after[i] < before[i]) removed += before[i] - after[i];
        if (measured)
            removed += ground.Lowered();
        else if (!removed && gd::Flag("Land.Changed") && !ground.absent)
            removed = 1;  // a heightmap this build could not measure: the flag is all there is
        out->removed = removed;
        return true;
    }
};
EngineLand g_backend;

void Contrib(melange::wormsign::Hasher& h, uint32_t, void*) {
    uint8_t b[sl::kHashBytes];
    h.Bytes(b, sl::HashBytes(b, sizeof b));
}

void OnContext(bool created, melange::lua50::State*, void*) {
    if (!created) {
        const sl::Stats s = sl::GetStats();
        if (s.carves || s.refused)
            LOG_INFO("[land] match end: %u carve(s), %u voxel(s) removed, %u refused", s.carves, s.removed, s.refused);
    }
    sl::ResetMatch();
    g_idSerial = ~0u;
}

bool Check() {
    using melange::mem::Expect;
    // OnExplosion: push -1; push handler; mov eax, fs:[0]
    return Expect(kOnExplosion, {0x6a, 0xff, 0x68, 0xab, 0x45, 0x7d, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00}) &&
           // its reads: the epicentre getter 0x518ee0 and the radius getter 0x527980, the broadphase, the frame carve
           Expect(0x473612, {0xe8}) && Expect(0x473619, {0xe8}) && Expect(0x473629, {0xe8}) &&
           Expect(0x47365d, {0xe8}) &&
           Expect(kFrameCarve, {0x6a, 0xff, 0x68, 0x0b, 0x82, 0x7c, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00}) &&
           Expect(kBroadphase, {0x83, 0xec, 0x1c, 0x53, 0x55, 0x8b, 0xa9, 0x9c, 0x00, 0x00, 0x00, 0x56}) &&
           // the per-voxel count decrement: add word [eax*2+0x955570], cx
           Expect(0x46d9ec, {0x66, 0x01, 0x0c, 0x45, 0x70, 0x55, 0x95, 0x00}) &&
           // the radius getter: fld dword [ecx+0x38]
           Expect(0x527980, {0xd9, 0x41, 0x38});
}

// The heightmap reads Ground relies on (optional: without them `removed` falls back to the Land.Changed flag).
bool CheckGround() {
    using melange::mem::Expect;
    return Expect(0x463e0c, {0x3b, 0x5f, 0x30}) &&                    // cmp ebx, [edi+0x30]   (the count)
           Expect(0x463e2b, {0x8b, 0x4f, 0x2c, 0xd9, 0x04, 0x99}) &&  // mov ecx, [edi+0x2c]; fld [ecx+ebx*4]
           Expect(0x463f64, {0xd9, 0x14, 0xb0, 0xd9, 0x47, 0x68}) &&  // fst [eax+esi*4]; fld [edi+0x68] (extent.y)
           Expect(0x460efa, {0xd8, 0x25, 0x40, 0xc4, 0x90, 0x00}) &&  // fsub [0x90c440]       (min.x)
           Expect(0x460f1c, {0xd8, 0x71, 0x64}) &&                    // fdiv [ecx+0x64]       (extent.x)
           Expect(0x461ace, {0xda, 0x0d, 0x2c, 0xc4, 0x90, 0x00}) &&  // fimul [0x90c42c]      (W)
           Expect(0x461aff, {0xda, 0x0d, 0x30, 0xc4, 0x90, 0x00}) &&  // fimul [0x90c430]      (H)
           Expect(0x463ce3, {0xf6, 0x47, 0x78, 0x02});                // test byte [edi+0x78], 2
}

class LandCarve final : public melange::Module {
public:
    const char* Name() const override { return "LandCarve"; }
    const char* Description() const override { return "wum.sim.carve: deterministic terrain removal for sim scripts"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 55; }
    bool Install() override {
        // The budget is the compile-time default (sim_land.h): an ini value would let peers carve differently.
        sl::Limits lim;
        lim.log = Bool("Log", false);
        sl::SetLimits(lim);
        if (!melange::gamestate::Available() || !Check()) {
            LOG_WARN("[land] layout differs from build #1077: wum.sim.carve answers \"unavailable\"");
            sl::SetBackend(nullptr);
        } else {
            g_ready = true;
            g_groundOk = CheckGround();
            if (!g_groundOk) LOG_WARN("[land] heightmap layout differs: its carves count 1 when anything changed");
            sl::SetBackend(&g_backend);
        }
        if (!sl::AddSimFunctions()) LOG_WARN("[land] could not add wum.sim.carve/carveLine");
        if (melange::lua50::Check() && melange::lua50::Track()) g_context = melange::lua50::OnContext(&OnContext, nullptr);
        g_contrib = melange::wormsign::AddContributor("melange.land", &Contrib, nullptr);
        LOG_INFO("[land] installed: wum.sim.carve (%d call(s) a tick, %d sphere(s) a turn)%s", lim.perTick, lim.perTurn,
                 g_ready ? "" : " (inert)");
        return true;
    }
    void Uninstall() override {
        g_ready = g_groundOk = false;
        sl::SetBackend(nullptr);
        if (g_context) melange::lua50::RemoveOnContext(g_context);
        if (g_contrib) melange::wormsign::RemoveContributor(g_contrib);
        g_context = g_contrib = 0;
    }
};
}  // namespace

MELANGE_MODULE(LandCarve);
