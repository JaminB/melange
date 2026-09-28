// Deterministic random streams for sim mods and C++ callers. They never touch the engine's RNGs.
#include <cmath>

#include "lua/sim/sim_internal.h"
#include "melange/sim.h"

namespace melange::simcore {
uint32_t Fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    return h;
}

uint32_t LcgNext(uint32_t& state) {
    state = state * 0x41c64e6du + 0x3039u;
    return state;
}

uint32_t Draw24(uint32_t& state) { return LcgNext(state) >> 8; }

bool RandomRange(uint32_t& state, double m, double n, double* out, const char** err) {
    m = std::floor(m);
    n = std::floor(n);
    if (!(m <= n)) {
        *err = "interval is empty";
        return false;
    }
    const double span = n - m + 1;
    if (span > 16777216.0 || std::fabs(m) > 16777216.0 || std::fabs(n) > 16777216.0) {
        *err = "interval is too large (the limit is 2^24)";
        return false;
    }
    const uint64_t r = (static_cast<uint64_t>(Draw24(state)) * static_cast<uint64_t>(span)) >> 24;
    *out = m + static_cast<double>(r);
    return true;
}
}  // namespace melange::simcore

namespace melange::sim {
uint32_t Random(uint32_t streamKey) {
    using namespace simcore;
    if (!g.L) return 0;
    auto [it, fresh] = g.cppStreams.try_emplace(streamKey, 0u);
    if (fresh) {
        uint32_t k = streamKey ^ 0x9e3779b9u;
        k = (k ^ (k >> 16)) * 0x85ebca6bu;
        it->second = g.seed ^ k ^ (k >> 13);
    }
    const uint32_t hi = LcgNext(it->second) >> 16;
    const uint32_t lo = LcgNext(it->second) >> 16;
    return hi << 16 | lo;
}
}  // namespace melange::sim
