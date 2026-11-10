// level.test.explode, level.test.quit, level.test.endTurn, level.test.selectWeapon, level.test.fireFactory and level.test.dropMines: harness hooks for a running
// Test match (erg-mcp's test_explosion, test {restart}, game_mines {fire} and game_mines {probe}); level.test.probe answers the Test probe (levels/probe.cpp)
// and the mine factory's fires (levels/factory.cpp). Both send through the match's own Lua (simbridge::EvalMatch) from a sim tick hook, so the engine handles
// them in its deterministic phase as it does a level script's: explode sets the Explosion data container and sends
// Explosion.Construct, the game's lib_CreateExplosion, at a named detail of the level (a knot); quit sends
// GameLogic.QuitGame, which leaves the match for the frontend; endTurn sends Timer.TurnTimedOut, the turn timer's own
// message, which ends the turn playing now as the timer running out does (a tester's skip; GameLogic.EndTurn alone only
// deactivated the worm and the turn ran on to its timer: tried 2026-11-08). fireFactory calls the factory's own turn-start handler
// from the same tick (levels::factory::FireNow); dropMines makes mines at points in that tick (levels::factory::Mine), to find
// the ground a mine lands on and the ground it falls through. roundTime {hold} pauses the round clock
// (GameLogic.RoundTime.Pause, the message the turn hold sends; TimerLogicEntity then stops counting RoundTimeRemaining)
// and pauses it again at every turn start while held, so a long harness run never reaches the Quick Game's sudden death
// (30 minutes: in the Caldera v2 round-2 run it raised the water past the level's cap and set every worm's health to 1).
// Only while a Test the game started is playing, offline.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "levels/factory.h"
#include "levels/probe.h"
#include "melange/gamestate.h"
#include "lua/sim/bridge_internal.h"
#include "melange/levels.h"
#include "melange/sim.h"
#include "mods/lobby.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"
#include "weapons/engine.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

std::mutex g_mx;
std::string g_playing;            // the Test playing now ("" for none); main thread
std::deque<std::string> g_queue;  // engine Lua to run at the next tick; under g_mx
int g_hook = 0;
unsigned g_sent = 0;
int g_fire = -1;                  // a fire to start at the next tick (its count; -1 none); under g_mx
std::vector<std::array<float, 6>> g_drops;   // mines to make at the next tick (pos, vel); under g_mx
bool g_roundHeld = false;         // level.test.roundTime {hold: true}; under g_mx
uint32_t g_heldTurn = UINT32_MAX; // the turn the round clock was last paused in (sim thread)
unsigned g_pauses = 0;            // pauses sent while held

void OnState(levels::TestState s, const char* key, const char*, void*) {
    using TS = levels::TestState;
    if (s == TS::Playing && key && *key) g_playing = key;
    else if (s == TS::Ended || s == TS::Failed || s == TS::Idle) {
        g_playing.clear();
        std::lock_guard lk(g_mx);
        g_roundHeld = false;
    }
}

void OnTick(uint32_t tick, void*) {
    std::deque<std::string> q;
    int fire = -1;
    std::vector<std::array<float, 6>> drops;
    bool held = false;
    {
        std::lock_guard lk(g_mx);
        q.swap(g_queue);
        std::swap(fire, g_fire);
        drops.swap(g_drops);
        held = g_roundHeld;
    }
    // the round clock held: paused again once in each turn (a level script's turn hold resumes it before its turn starts)
    if (held && tick % 25 == 0) {
        gamestate::Snapshot snap;
        if (gamestate::Read(&snap) && snap.match.inMatch && snap.match.turnsStarted != g_heldTurn) {
            g_heldTurn = snap.match.turnsStarted;
            q.push_back("SendMessage('GameLogic.RoundTime.Pause')");
            ++g_pauses;
        }
    }
    int made = 0;
    for (const auto& d : drops) made += !levels::factory::Mine(d.data(), d.data() + 3);
    if (!drops.empty()) LOG_INFO("[erg] test: tick %u: %d of %zu mines dropped", tick, made, drops.size());
    if (fire >= 0) {
        std::string detail;
        const char* why = levels::factory::FireNow(fire, &detail);
        if (why) LOG_WARN("[erg] test: tick %u: the mine factory did not fire: %s (%s)", tick, why, detail.c_str());
    }
    for (const std::string& code : q) {
        const sandbox::EvalOut r = simbridge::EvalMatch(code);
        ++g_sent;
        if (!r.ok) LOG_WARN("[erg] test: tick %u: %s", tick, r.text.c_str());
        else LOG_INFO("[erg] test: tick %u: %s", tick, code.c_str());
    }
}

// The Test is playing, offline, and the match VM is there.
bool Ready(Result& r) {
    if (handshake::lobby::Current() != 0) return Fail(r, rpc::kRefused, "not in a lobby or an online match");
    if (!sim::InMatch()) return Fail(r, rpc::kNotInMatch, "no match is running");
    if (g_playing.empty()) return Fail(r, rpc::kRefused, "only in a Test match (level.test) while it plays");
    if (!g_hook) g_hook = sim::AddTickHook(&OnTick, nullptr, 50);
    return g_hook ? true : Fail(r, rpc::kRefused, "the sim tick hook is unavailable");
}

void Queue(std::string code) {
    std::lock_guard lk(g_mx);
    g_queue.push_back(std::move(code));
}

bool Num(const json::Value& p, const char* key, double lo, double hi, double def, double* out, Result& r) {
    const json::Value* v = p.Get(key);
    *out = def;
    if (!v || v->IsNull()) return true;
    if (!v->IsNumber() || !std::isfinite(v->number) || v->number < lo || v->number > hi) {
        char b[160];
        snprintf(b, sizeof b, "%s must be a number from %g to %g", key, lo, hi);
        return Fail(r, rpc::kBadParams, b);
    }
    *out = v->number;
    return true;
}

// level.test.explode {detail, landRadius, wormDamage?, wormRadius?, impulse?, impulseRadius?}: world units
void Explode(const Call& c, Result& r, void*) {
    json::Value p;
    std::string detail;
    if (!rpc::ParseParams(c, &p, r) || !rpc::Str(p, "detail", &detail, r)) return;
    if (detail.empty() || detail.size() > 63 ||
        !std::all_of(detail.begin(), detail.end(), [](char ch) { return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.'; })) {
        Fail(r, rpc::kBadParams, "detail: the name of a detail (knot) of the level, 1-63 of A-Z a-z 0-9 _ - .");
        return;
    }
    double land, damage, wormR, impulse, impulseR;
    if (!Num(p, "landRadius", 1, 2000, 60, &land, r) || !Num(p, "wormDamage", 0, 1000, 0, &damage, r) ||
        !Num(p, "wormRadius", 0, 2000, 0, &wormR, r) || !Num(p, "impulse", 0, 100, 0, &impulse, r) ||
        !Num(p, "impulseRadius", 0, 2000, 0, &impulseR, r) || !Ready(r))
        return;
    char code[640];
    snprintf(code, sizeof code,
             "SetData('Explosion.DetailObject', '%s'); SetData('Explosion.WormDamageMagnitude', %g); "
             "SetData('Explosion.ImpulseMagnitude', %g); SetData('Explosion.WormDamageRadius', %g); "
             "SetData('Explosion.LandDamageRadius', %g); SetData('Explosion.ImpulseRadius', %g); SendMessage('Explosion.Construct')",
             detail.c_str(), damage, impulse, wormR, land, impulseR);
    Queue(code);
    r.json = jsonmini::Obj().Bool("queued", true).Str("detail", detail).UInt("tick", sim::Tick()).End();
}

// level.test.quit {}: back to the frontend, as the pause menu's Quit (it writes the game's save, as that does);
// level.test can start the Test again there. GameLogic.GotoFrontEnd is not an alternative: sent from a match it
// crashed the game (2026-11-08).
void Quit(const Call&, Result& r, void*) {
    if (!Ready(r)) return;
    Queue("SendMessage('GameLogic.QuitGame')");
    r.json = jsonmini::Obj().Bool("queued", true).Str("key", g_playing).UInt("tick", sim::Tick()).End();
}
// level.test.endTurn {}: the turn playing now ends (Timer.TurnTimedOut, sent in the sim's tick), so a test tool need not
// fire a weapon or wait out the turn time to go on to the next worm (the next turn's intro came 11 s later in a Test).
void EndTurn(const Call&, Result& r, void*) {
    if (!Ready(r)) return;
    Queue("SendMessage('Timer.TurnTimedOut')");
    r.json = jsonmini::Obj().Bool("queued", true).UInt("tick", sim::Tick()).End();
}

// level.test.selectWeapon {weapon: id | "kWeaponBazooka" | "bazooka"}: the active worm takes that weapon, as the weapon
// panel's pick does: Weapon.DelayedPanelChanged <id> is sent in the sim's tick (the panel sends the same replicated at
// 0x600d0c, and every machine's WXWeaponPanelLogicEntity::HandleMessage 0x603b70 then sets the worm's weapon from it), so
// a test tool selects a weapon without the panel, its clicks or DirectInput: the Automation module's injected mouse
// reported polls=0 in the Caldera v2 round-3 baseline, no bazooka was ever taken and nothing fired. Offline matches
// only (Queue). Answers the id and the engine's name; wum.game.worms()[slot].weapon reads it back a tick later.
void SelectWeapon(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    const json::Value* w = p.Get("weapon");
    int id = -1;
    if (w && w->IsNumber()) {
        id = static_cast<int>(w->number);
    } else if (w && w->IsString()) {
        auto lower = [](std::string s) {
            for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return s;
        };
        const std::string want = lower(w->string);
        for (int i = 1; i < 0x45 && id < 0; ++i) {
            const char* n = weapons::engine::EnumName(i);
            if (!n) continue;
            const std::string name = lower(n);
            if (name == want || name == "kweapon" + want || name == "kutility" + want || name == "kmystery" + want) id = i;
        }
        if (id < 0) {
            Fail(r, rpc::kBadParams, "weapon: a weapon id (1-68) or the engine's name, kWeaponBazooka (or bazooka)");
            return;
        }
    } else {
        Fail(r, rpc::kBadParams, "weapon: a weapon id (1-68) or the engine's name, kWeaponBazooka (or bazooka)");
        return;
    }
    if (id < 1 || id >= 0x45) {
        Fail(r, rpc::kBadParams, "weapon: an id from 1 to 68");
        return;
    }
    if (!Ready(r)) return;
    char code[96];
    snprintf(code, sizeof code, "SendIntMessage('Weapon.DelayedPanelChanged', %d)", id);
    Queue(code);
    const char* name = weapons::engine::EnumName(id);
    r.json = jsonmini::Obj().Bool("queued", true).Int("weapon", id).Str("name", name ? name : "").UInt("tick", sim::Tick()).End();
}

// level.test.fireFactory {count?}: at the next sim tick the level's mine factory fires as at a turn start (its idle
// countdown set to 0 and its own StartMineFactory called); count 1-10 fires that many whatever the target.
// level.test.probe says whether it started (factory.trigger) and, once its mines rest, where (factory.fires).
void FireFactory(const Call& c, Result& r, void*) {
    json::Value p;
    double count = 0;
    if (!rpc::ParseParams(c, &p, r) || !Num(p, "count", 0, 10, 0, &count, r) || !Ready(r)) return;
    if (count != static_cast<int>(count)) {
        Fail(r, rpc::kBadParams, "count must be a whole number from 0 to 10");
        return;
    }
    {
        std::lock_guard lk(g_mx);
        g_fire = static_cast<int>(count);
    }
    r.json = jsonmini::Obj().Bool("queued", true).Int("count", static_cast<int>(count)).UInt("tick", sim::Tick()).End();
}

// level.test.dropMines {points: [[x, y, z], ...], vel?: [x, y, z]}: world units (vel per ms, default the factory's own
// (0, -0.3, 0)), 1-64 points: at the next sim tick a mine is made at each, as the factory makes its own.
void DropMines(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    auto vec = [](const json::Value* v, float* out, double lim) {
        if (!v || !v->IsArray() || v->items.size() != 3) return false;
        for (size_t k = 0; k < 3; ++k) {
            const json::Value& n = v->items[k];
            if (!n.IsNumber() || !std::isfinite(n.number) || std::fabs(n.number) > lim) return false;
            out[k] = static_cast<float>(n.number);
        }
        return true;
    };
    const json::Value* pts = p.Get("points");
    if (!pts || !pts->IsArray() || pts->items.empty() || pts->items.size() > 64) {
        Fail(r, rpc::kBadParams, "points: 1-64 [x, y, z] points, world units");
        return;
    }
    float vel[3] = {0.f, -0.3f, 0.f};
    const json::Value* v = p.Get("vel");
    if (v && !v->IsNull() && !vec(v, vel, 10)) {
        Fail(r, rpc::kBadParams, "vel: [x, y, z], world units per ms, each within 10");
        return;
    }
    std::vector<std::array<float, 6>> drops;
    for (const json::Value& pt : pts->items) {
        std::array<float, 6> d{};
        if (!vec(&pt, d.data(), 100000)) {
            Fail(r, rpc::kBadParams, "points: each [x, y, z] within 100000 world units");
            return;
        }
        std::copy(vel, vel + 3, d.begin() + 3);
        drops.push_back(d);
    }
    if (!Ready(r)) return;
    {
        std::lock_guard lk(g_mx);
        g_drops.insert(g_drops.end(), drops.begin(), drops.end());
    }
    r.json = jsonmini::Obj().Bool("queued", true).Int("count", static_cast<int>(drops.size())).UInt("tick", sim::Tick()).End();
}

// level.test.landRay {segments: [[x0, y0, z0, x1, y1, z1], ...]}: world units, 1-64 segments: the game's own land sweep
// along each (gamestate::LandRay, the straight sweep the camera ray cast uses), answered as the hit point and normal, or a
// miss, so the ground a thrown mine will meet can be read where it is meant to land (Caldera's mines fell through ground
// worms stood on). Main thread.
void LandRayOp(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    const json::Value* segs = p.Get("segments");
    if (!segs || !segs->IsArray() || segs->items.empty() || segs->items.size() > 64) {
        Fail(r, rpc::kBadParams, "segments: 1-64 [x0, y0, z0, x1, y1, z1], world units");
        return;
    }
    if (!sim::InMatch()) {
        Fail(r, rpc::kNotInMatch, "no match is running");
        return;
    }
    auto vec3 = [](float x, float y, float z) {
        char b[96];
        snprintf(b, sizeof b, "[%.17g,%.17g,%.17g]", static_cast<double>(x), static_cast<double>(y), static_cast<double>(z));
        return std::string(b);
    };
    jsonmini::Arr out;
    for (const json::Value& sv : segs->items) {
        float v[6];
        bool ok = sv.IsArray() && sv.items.size() == 6;
        for (size_t k = 0; ok && k < 6; ++k) {
            const json::Value& n = sv.items[k];
            ok = n.IsNumber() && std::isfinite(n.number) && std::fabs(n.number) <= 100000;
            if (ok) v[k] = static_cast<float>(n.number);
        }
        if (!ok) {
            Fail(r, rpc::kBadParams, "segments: each [x0, y0, z0, x1, y1, z1] within 100000 world units");
            return;
        }
        gamestate::LandHit h{};
        const gamestate::LandRayResult res = gamestate::LandRay({v[0], v[1], v[2]}, {v[3], v[4], v[5]}, &h);
        jsonmini::Obj o;
        switch (res) {
            case gamestate::LandRayResult::Hit:
                o.Bool("hit", true).Float("t", h.t)
                    .Raw("at", vec3(v[0] + (v[3] - v[0]) * h.t, v[1] + (v[4] - v[1]) * h.t, v[2] + (v[5] - v[2]) * h.t))
                    .Raw("normal", vec3(h.normal.x, h.normal.y, h.normal.z)).Int("frame", h.frame);   // 65535: the heightmap
                break;
            case gamestate::LandRayResult::Miss: o.Bool("hit", false); break;
            case gamestate::LandRayResult::Budget: o.Bool("hit", false).Str("error", "budget"); break;
            case gamestate::LandRayResult::Invalid: o.Bool("hit", false).Str("error", "invalid"); break;
            default: o.Bool("hit", false).Str("error", "unavailable"); break;
        }
        out.Raw(o.End());
    }
    r.json = jsonmini::Obj().Raw("rays", out.End()).UInt("tick", sim::Tick()).End();
}

// level.test.roundTime {hold: bool}: hold pauses the round clock now and again at each turn start until the Test ends
// (or hold: false, which resumes it), so the scheme's sudden death never comes; the turn clock runs as before.
void RoundTime(const Call& c, Result& r, void*) {
    json::Value p;
    if (!rpc::ParseParams(c, &p, r)) return;
    const json::Value* h = p.Get("hold");
    if (!h || !h->IsBool()) {
        Fail(r, rpc::kBadParams, "hold must be true or false");
        return;
    }
    if (!Ready(r)) return;
    {
        std::lock_guard lk(g_mx);
        g_roundHeld = h->boolean;
        if (!h->boolean) g_queue.push_back("SendMessage('GameLogic.RoundTime.Resume')");
    }
    if (h->boolean) g_heldTurn = UINT32_MAX;
    r.json = jsonmini::Obj().Bool("held", h->boolean).UInt("pausesSent", g_pauses).UInt("tick", sim::Tick()).End();
}

// level.test.probe {}: the Test probe (worm spawns at turn 1, deaths with their causes, explosions) and the mine
// factory's fires (each mine's spawn and rest) and its last on-demand trigger. Main thread.
void Probe(const Call&, Result& r, void*) {
    r.json = jsonmini::Obj().Raw("probe", levels::probe::ReportJson()).Raw("factory", levels::factory::FiresJson()).End();
}
}  // namespace

void InstallLevelTestOps() {
    levels::OnTestState(&OnState, nullptr);
    AddMethod("level.test.explode", &Explode, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.quit", &Quit, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.endTurn", &EndTurn, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.selectWeapon", &SelectWeapon, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.fireFactory", &FireFactory, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.dropMines", &DropMines, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.landRay", &LandRayOp, nullptr, kRpcGameOnly);
    AddMethod("level.test.probe", &Probe, nullptr, kRpcGameOnly);
    AddMethod("level.test.roundTime", &RoundTime, nullptr, kRpcMutating | kRpcGameOnly);
}
}  // namespace melange::oasis::providers
