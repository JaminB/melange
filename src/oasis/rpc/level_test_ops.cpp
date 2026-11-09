// level.test.explode, level.test.quit, level.test.endTurn and level.test.fireFactory: harness hooks for a running Test match (erg-mcp's
// test_explosion, test {restart} and game_mines {fire}); level.test.probe answers the Test probe (levels/probe.cpp)
// and the mine factory's fires (levels/factory.cpp). Both send through the match's own Lua (simbridge::EvalMatch) from a sim tick hook, so the engine handles
// them in its deterministic phase as it does a level script's: explode sets the Explosion data container and sends
// Explosion.Construct, the game's lib_CreateExplosion, at a named detail of the level (a knot); quit sends
// GameLogic.QuitGame, which leaves the match for the frontend; endTurn sends Timer.TurnTimedOut, the turn timer's own
// message, which ends the turn playing now as the timer running out does (a tester's skip; GameLogic.EndTurn alone only
// deactivated the worm and the turn ran on to its timer: tried 2026-11-08). fireFactory calls the factory's own turn-start handler
// from the same tick (levels::factory::FireNow). Only while a Test the game started is playing, offline.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "core/log.h"
#include "levels/factory.h"
#include "levels/probe.h"
#include "lua/sim/bridge_internal.h"
#include "melange/levels.h"
#include "melange/sim.h"
#include "mods/lobby.h"
#include "oasis/providers.h"
#include "oasis/rpc/params.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
using rpc::Fail;

std::mutex g_mx;
std::string g_playing;            // the Test playing now ("" for none); main thread
std::deque<std::string> g_queue;  // engine Lua to run at the next tick; under g_mx
int g_hook = 0;
unsigned g_sent = 0;
int g_fire = -1;                  // a fire to start at the next tick (its count; -1 none); under g_mx

void OnState(levels::TestState s, const char* key, const char*, void*) {
    using TS = levels::TestState;
    if (s == TS::Playing && key && *key) g_playing = key;
    else if (s == TS::Ended || s == TS::Failed || s == TS::Idle) g_playing.clear();
}

void OnTick(uint32_t tick, void*) {
    std::deque<std::string> q;
    int fire = -1;
    {
        std::lock_guard lk(g_mx);
        q.swap(g_queue);
        std::swap(fire, g_fire);
    }
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
    AddMethod("level.test.fireFactory", &FireFactory, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.probe", &Probe, nullptr, kRpcGameOnly);
}
}  // namespace melange::oasis::providers
