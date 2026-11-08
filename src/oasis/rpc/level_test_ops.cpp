// level.test.explode and level.test.quit: harness hooks for a running Test match (erg-mcp's test_explosion and test
// {restart}). Both send through the match's own Lua (simbridge::EvalMatch) from a sim tick hook, so the engine handles
// them in its deterministic phase as it does a level script's: explode sets the Explosion data container and sends
// Explosion.Construct, the game's lib_CreateExplosion, at a named detail of the level (a knot); quit sends
// GameLogic.QuitGame, which leaves the match for the frontend. Only while a Test the game started is playing, offline.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

#include "core/log.h"
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

void OnState(levels::TestState s, const char* key, const char*, void*) {
    using TS = levels::TestState;
    if (s == TS::Playing && key && *key) g_playing = key;
    else if (s == TS::Ended || s == TS::Failed || s == TS::Idle) g_playing.clear();
}

void OnTick(uint32_t tick, void*) {
    std::deque<std::string> q;
    {
        std::lock_guard lk(g_mx);
        q.swap(g_queue);
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
}  // namespace

void InstallLevelTestOps() {
    levels::OnTestState(&OnState, nullptr);
    AddMethod("level.test.explode", &Explode, nullptr, kRpcMutating | kRpcGameOnly);
    AddMethod("level.test.quit", &Quit, nullptr, kRpcMutating | kRpcGameOnly);
}
}  // namespace melange::oasis::providers
