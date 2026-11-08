// The level's mine factory in the match, build #1077 (docs/erg.md, The mine factory).
//
// MineFactoryLogicEntity's set-up (0x5d0170) first drops the factory from its knot (0x5cf710: a land sweep of at most
// 1000 ticks from the knot, the hit point copied from the sweep's globals whether or not it hit), then reads
// kMineFactoryData.NumTurnsInactive (+0x2d) into the idle countdown (+0x88); each GameLogic.StartMineFactory
// (0x5cfe40) decrements the countdown and, at 0 or under, fires min(NumMineActivation (+0x2c) - mines, 10) when fewer
// mines than the target are in the level (+0x8c: the fire state, +0x84: the mines to fire). registry writes the level's
// values into kMineFactoryData at level start; the hook on the drop writes them again just before the set-up reads
// them (the match's VM is open then, so the tweak store still puts the game's back when it closes) and records what it
// found there, so a writer between level start and the factory's placement shows in the report: in Tests on
// 2026-11-08 it held the game's 20 and 7 again by then, which is why the level's settings did not reach the factory.
// With keepKnot a drop that misses leaves the factory at its knot instead of the stale point. A miss is a sweep that
// reports no hit, or one whose point is not under the knot (the sweep is straight down; over a draped skin it reported
// a hit 63 map units off, the stale point).
#include "levels/factory.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <vector>

#include <safetyhook.hpp>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "lua/sim/sim_core.h"
#include "lua/sim/tweak.h"
#include "melange/gamestate.h"
#include "tools/json_mini.h"

namespace melange::levels::factory {
namespace {
constexpr uintptr_t kDrop = 0x5cf710;          // void __thiscall MineFactoryLogicEntity::Drop()
constexpr uintptr_t kSweepTicks = 0x952cfc;    // the last land sweep's hit tick; 1000 = no hit
constexpr int kNoHit = 1000;
constexpr float kUnderKnot = 20;               // world units: a hit further from the knot in x, z is not under it
constexpr uintptr_t kCreateMine = 0x4f9c40;    // void __thiscall GameLogicService::CreateMine(pos*, pos*, vel*)
constexpr uintptr_t kLogicService = 0x95aa14;  // GameLogicService*
constexpr uint32_t kPos = 0x2c, kNormal = 0x38, kExploded = 0x48, kCountdown = 0x88, kFireState = 0x8c;

struct Field {
    bool asked = false;
    int value = 0;
    std::optional<float> found;   // kMineFactoryData's value just before the set-up's write
    std::string result;           // the tweak store's answer
};
struct State {
    bool armed = false;
    std::string stem;
    erg::FactorySpec spec;
    // the last set-up
    bool reported = false;
    std::string reportStem;
    Field activation, inactive;
    float knot[3] = {}, settled[3] = {};
    bool hit = false, keptKnot = false;
};
std::mutex g_mx;
State g;
SafetyHookInline g_hDrop;

bool ReadVec(uintptr_t at, float out[3]) { return mem::SafeRead(at, out, sizeof(float) * 3); }

void ApplyField(const char* name, const std::optional<int>& v, Field* f) {
    *f = Field{};
    if (!v) return;
    f->asked = true;
    f->value = *v;
    float was = 0;
    if (tweak::Get("kMineFactoryData", name, &was) == tweak::TweakError::Ok) f->found = was;
    f->result = tweak::ToString(tweak::Set("kMineFactoryData", name, static_cast<float>(*v)));
}

void __fastcall HkDrop(uintptr_t self, void*) {
    State s;
    {
        std::lock_guard lk(g_mx);
        s = g;
    }
    if (!s.armed) {
        g_hDrop.thiscall<void>(self);
        return;
    }
    ApplyField("NumMineActivation", s.spec.activation, &s.activation);
    ApplyField("NumTurnsInactive", s.spec.inactiveTurns, &s.inactive);
    ReadVec(self + kPos, s.knot);
    g_hDrop.thiscall<void>(self);
    int ticks = kNoHit;
    mem::SafeRead(kSweepTicks, &ticks, sizeof ticks);
    ReadVec(self + kPos, s.settled);
    s.hit = ticks != kNoHit && std::hypot(s.settled[0] - s.knot[0], s.settled[2] - s.knot[2]) <= kUnderKnot;
    s.keptKnot = false;
    if (!s.hit && s.spec.keepKnot) {
        const float up[3] = {0, 1, 0};
        s.keptKnot = mem::Write(self + kPos, s.knot, sizeof s.knot) && mem::Write(self + kNormal, up, sizeof up);
    }
    ReadVec(self + kPos, s.settled);
    s.reported = true;
    s.reportStem = s.stem;
    auto fieldText = [](const Field& f) {
        if (!f.asked) return std::string("game's");
        char b[96];
        if (f.found) snprintf(b, sizeof b, "%d (was %g): %s", f.value, static_cast<double>(*f.found), f.result.c_str());
        else snprintf(b, sizeof b, "%d: %s", f.value, f.result.c_str());
        return std::string(b);
    };
    LOG_INFO("[levels] %s: the mine factory's set-up: kMineFactoryData.NumMineActivation %s, NumTurnsInactive %s; its drop "
             "from (%.1f, %.1f, %.1f) %s (%.1f, %.1f, %.1f)%s",
             s.stem.c_str(), fieldText(s.activation).c_str(), fieldText(s.inactive).c_str(), s.knot[0], s.knot[1], s.knot[2],
             s.hit ? "landed at" : "hit no land; it is at", s.settled[0], s.settled[1], s.settled[2],
             s.hit ? "" : s.keptKnot ? " (keepKnot: its knot)" : " (the stale point of the match's last sweep)");
    std::lock_guard lk(g_mx);
    State keep = g;
    g = s;
    g.armed = keep.armed;
    g.spec = keep.spec;
    g.stem = keep.stem;
}

// The match's one MineFactoryLogicEntity, or 0.
uintptr_t FindFactory() {
    std::vector<gamestate::Entity> ents(512);
    int n = gamestate::Entities(ents.data(), static_cast<int>(ents.size()));
    if (n > static_cast<int>(ents.size())) {
        ents.resize(static_cast<size_t>((std::min)(n, 4096)));
        n = gamestate::Entities(ents.data(), static_cast<int>(ents.size()));
    }
    for (int i = 0; i < (std::min)(n, static_cast<int>(ents.size())); ++i)
        if (std::strcmp(ents[static_cast<size_t>(i)].type, "MineFactoryLogicEntity") == 0) return ents[static_cast<size_t>(i)].object;
    return 0;
}

// wum.level.mine: a mine at pos (world units) with velocity vel, made as the factory makes its own.
const char* EngineMine(const float pos[3], const float vel[3]) {
    uintptr_t svc = 0;
    if (!mem::SafeRead(kLogicService, &svc, sizeof svc) || !svc) return "no game logic service";
    using CreateMine = void(__thiscall*)(uintptr_t, const float*, const float*, const float*);
    reinterpret_cast<CreateMine>(kCreateMine)(svc, pos, pos, vel);
    return nullptr;
}

// wum.level.fireFactory: the idle countdown to 0, so the next GameLogic.StartMineFactory fires when fewer mines than
// the target are in the level.
const char* EngineArmFactory() {
    const uintptr_t f = FindFactory();
    if (!f) return "the level has no mine factory (or it is gone)";
    uint8_t exploded = 0;
    int32_t fire = 0;
    if (!mem::SafeRead(f + kExploded, &exploded, 1) || !mem::SafeRead(f + kFireState, &fire, 4)) return "the factory is unreadable";
    if (exploded) return "the factory has exploded";
    if (fire != 0) return "the factory is firing";
    const int32_t zero = 0;
    return mem::Write(f + kCountdown, &zero, sizeof zero) ? nullptr : "the factory could not be written";
}

class LevelFactory final : public Module {
public:
    const char* Name() const override { return "LevelFactory"; }
    const char* Description() const override { return "the level's mine factory settings, drop and wum.level.mine"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 55; }   // after SimTweak (53)
    bool Install() override {
        using mem::Expect;
        // Drop: sub esp,0x18; fldz; push esi; fst [esp+0x10]; push edi. CreateMine: push -1; push 0x7cf488; mov eax,fs:[0].
        // StartMineFactory: sub esp,0xc; push esi; mov esi,ecx; cmp dword [esi+0x8c],0 (the fire state).
        if (!Expect(kDrop, {0x83, 0xec, 0x18, 0xd9, 0xee, 0x56, 0xd9, 0x54, 0x24, 0x10, 0x57}) ||
            !Expect(kCreateMine, {0x6a, 0xff, 0x68, 0x88, 0xf4, 0x7c, 0x00, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00}) ||
            !Expect(0x5cfe40, {0x83, 0xec, 0x0c, 0x56, 0x8b, 0xf1, 0x83, 0xbe, 0x8c, 0x00, 0x00, 0x00, 0x00}) ||
            !Expect(0x5cf76a, {0x81, 0x3d, 0xfc, 0x2c, 0x95, 0x00, 0xe8, 0x03, 0x00, 0x00})) {
            LOG_WARN("[levels] the mine factory's code differs from build #1077: LevelFactory stays inert");
            return true;
        }
        g_hDrop = safetyhook::create_inline(kDrop, &HkDrop);
        if (!g_hDrop) {
            LOG_ERROR("[levels] the mine factory hook failed: level factory settings apply at level start only");
            return true;
        }
        simcore::LevelEngine e;
        e.mine = &EngineMine;
        e.armFactory = &EngineArmFactory;
        simcore::SetLevelEngine(e);
        return true;
    }
    void Uninstall() override {
        simcore::SetLevelEngine(simcore::LevelEngine{});
        g_hDrop = {};
    }
};
}  // namespace

void Arm(const std::string& stem, const erg::FactorySpec& f) {
    std::lock_guard lk(g_mx);
    g.armed = true;
    g.stem = stem;
    g.spec = f;
}

void Disarm() {
    std::lock_guard lk(g_mx);
    g.armed = false;
}

std::string ReportJson() {
    State s;
    {
        std::lock_guard lk(g_mx);
        s = g;
    }
    if (!s.reported) return "null";
    auto field = [](const Field& f) {
        if (!f.asked) return std::string("null");
        jsonmini::Obj o;
        o.Int("asked", f.value).Str("write", f.result);
        if (f.found) o.Float("found", *f.found);
        return o.End();
    };
    auto vec = [](const float* v) {
        return jsonmini::Arr().Raw(std::to_string(v[0])).Raw(std::to_string(v[1])).Raw(std::to_string(v[2])).End();
    };
    return jsonmini::Obj().Str("stem", s.reportStem).Raw("activation", field(s.activation)).Raw("inactiveTurns", field(s.inactive))
        .Raw("knot", vec(s.knot)).Raw("settled", vec(s.settled)).Bool("landed", s.hit).Bool("keptKnot", s.keptKnot).End();
}

MELANGE_MODULE(LevelFactory);
}  // namespace melange::levels::factory
