// wum.sim.carve / carveLine. No engine addresses here: the Backend is the game (LandCarve module) or the self-test's
// fake. Lua 5.0 rules: the trampolines keep only plain data alive and raise after every helper has returned.
#include "lua/sim/sim_land.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"

namespace melange::simland {
namespace {
namespace l5 = lua50;
Backend* g_backend = nullptr;
Limits g_limits;
char g_err[160];

struct Budget {
    uint32_t tick = ~0u, turn = ~0u;
    int inTick = 0, inTurn = 0;
} g_budget;
uint64_t g_digest = 0xcbf29ce484222325ull;
Stats g_stats{};

void Fold(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        g_digest ^= b[i];
        g_digest *= 0x100000001b3ull;
    }
}

uint32_t Bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

int Raise(l5::State* L) {
    l5::A().pushstring(L, g_err);
    l5::A().error(L);
    return 0;
}

int NilReason(l5::State* L, const char* why) {
    l5::A().pushnil(L);
    l5::A().pushstring(L, why);
    return 2;
}

// Budget: CarvesPerTick a tick and CarvesPerTurn a turn (the turn counter of wum.sim.turn()).
bool Spend(int n) {
    const uint32_t tick = sim::Tick(), turn = simcore::g.turns;
    if (tick != g_budget.tick) {
        g_budget.tick = tick;
        g_budget.inTick = 0;
    }
    if (turn != g_budget.turn) {
        g_budget.turn = turn;
        g_budget.inTurn = 0;
    }
    if (g_budget.inTick + n > g_limits.perTick || g_budget.inTurn + n > g_limits.perTurn) return false;
    g_budget.inTick += n;
    g_budget.inTurn += n;
    return true;
}

bool Finite(float f) { return std::isfinite(f) != 0; }

int RunOne(const float p[3], float r, const char** why) {
    CarveOut out;
    if (!g_backend->Carve(p, r, &out)) {
        *why = out.why ? out.why : "unavailable";
        ++g_stats.refused;
        return -1;
    }
    const uint32_t rec[6] = {sim::Tick(), Bits(p[0]), Bits(p[1]), Bits(p[2]), Bits(r), static_cast<uint32_t>(out.removed)};
    Fold(rec, sizeof rec);
    ++g_stats.carves;
    g_stats.removed += static_cast<uint32_t>(out.removed);
    if (g_limits.log)
        LOG_INFO("[land] carve (%.2f, %.2f, %.2f) r=%.2f removed=%d tick %u", p[0], p[1], p[2], r, out.removed, sim::Tick());
    return out.removed;
}

// A number argument at `i` as a float32 (the Lua number already is one; this also refuses NaN and infinities).
bool Num(l5::State* L, int i, float* out) {
    const auto& a = l5::A();
    if (a.type(L, i) != l5::kTNumber) return false;
    *out = a.tonumber(L, i);
    return Finite(*out);
}
}  // namespace

void SetBackend(Backend* b) { g_backend = b; }
void SetLimits(const Limits& l) { g_limits = l; }

void ResetMatch() {
    g_budget = Budget{};
    g_digest = 0xcbf29ce484222325ull;
    g_stats = Stats{};
}

size_t HashBytes(uint8_t* out, size_t cap) {
    if (cap < kHashBytes) return 0;
    memcpy(out, &g_digest, 8);
    memcpy(out + 8, &g_stats.carves, 4);
    return kHashBytes;
}

Stats GetStats() { return g_stats; }

int Carve(float x, float y, float z, float radius, const char** why) {
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    if (!sim::InMatch()) return *why = "not in a match", -1;
    if (!g_backend) return *why = "unavailable", -1;
    if (!Finite(x) || !Finite(y) || !Finite(z) || !Finite(radius) || radius < kMinRadius || radius > kMaxRadius)
        return *why = "bad arguments", -1;
    if (!Spend(1)) {
        ++g_stats.refused;
        return *why = "budget", -1;
    }
    const float p[3] = {x, y, z};
    return RunOne(p, radius, why);
}

// wum.sim.carve(x, y, z, radius) -> removed | nil, reason
int __cdecl LCarve(l5::State* L) {
    float v[4];
    for (int i = 0; i < 4; ++i)
        if (!Num(L, i + 1, &v[i])) {
            snprintf(g_err, sizeof g_err, "wum.sim.carve(x, y, z, radius): four finite numbers expected");
            return Raise(L);
        }
    const char* why = nullptr;
    const int n = Carve(v[0], v[1], v[2], v[3], &why);
    if (n < 0) return NilReason(L, why ? why : "failed");
    l5::A().pushnumber(L, static_cast<float>(n));
    return 1;
}

// wum.sim.carveLine(x0, y0, z0, x1, y1, z1, radius [, step]) -> removed, spheres | nil, reason
// Spheres every `step` (default radius/2, at least radius/4) from the start to the end inclusive, at most 16; the
// whole stroke is refused up front when the budget cannot take every sphere, so a stroke is never cut short.
int __cdecl LCarveLine(l5::State* L) {
    const auto& a = l5::A();
    float v[8];
    for (int i = 0; i < 7; ++i)
        if (!Num(L, i + 1, &v[i])) {
            snprintf(g_err, sizeof g_err, "wum.sim.carveLine(x0, y0, z0, x1, y1, z1, radius [, step]): finite numbers expected");
            return Raise(L);
        }
    const float r = v[6];
    float step = r * 0.5f;
    const int st = a.type(L, 8);
    if (st != l5::kTNone && st != l5::kTNil) {
        if (!Num(L, 8, &v[7])) {
            snprintf(g_err, sizeof g_err, "wum.sim.carveLine: step must be a number");
            return Raise(L);
        }
        step = v[7];
    }
    if (!sim::InMatch()) return NilReason(L, "not in a match");
    if (!g_backend) return NilReason(L, "unavailable");
    if (r < kMinRadius || r > kMaxRadius) return NilReason(L, "bad arguments");
    if (step < r * 0.25f) step = r * 0.25f;
    const float d[3] = {v[3] - v[0], v[4] - v[1], v[5] - v[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    int n = static_cast<int>(std::ceil(len / step)) + 1;
    if (len <= 0.f) n = 1;
    if (n > kMaxLineSpheres) return NilReason(L, "too long");
    if (!Spend(n)) {
        ++g_stats.refused;
        return NilReason(L, "budget");
    }
    int total = 0;
    const char* why = nullptr;
    for (int k = 0; k < n; ++k) {
        const float t = n == 1 ? 0.f : static_cast<float>(k) / static_cast<float>(n - 1);
        const float p[3] = {v[0] + d[0] * t, v[1] + d[1] * t, v[2] + d[2] * t};
        const int got = RunOne(p, r, &why);
        if (got < 0) {
            if (k == 0) return NilReason(L, why ? why : "failed");
            break;  // the land became unavailable mid-stroke: report what was removed
        }
        total += got;
    }
    a.pushnumber(L, static_cast<float>(total));
    a.pushnumber(L, static_cast<float>(n));
    return 2;
}

bool AddSimFunctions() {
    bool ok = simbridge::AddSimFunction("carve", &LCarve);
    ok = simbridge::AddSimFunction("carveLine", &LCarveLine) && ok;
    return ok;
}
}  // namespace melange::simland
