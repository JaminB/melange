#pragma once
// Pure logic for MirageSupersample: merging enabled mods' supersampling requests with the [MirageSupersample] ini
// override, and the engine's /SSAA switch. No GL, no windows.h: offline self-tested in tests/textures_selftest.cpp.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace melange::mirage::supersample::logic {
constexpr int kMaxModSamples = 4;

// /SSAA:<n> sample counts.
inline bool ValidSamples(int n) { return n == 1 || n == 2 || n == 4 || n == 8 || n == 16; }
inline bool ValidModSamples(int n) { return n == 2 || n == 4; }

// The engine's anti-aliasing state: the scale factors (wide x high), its FXAA flag, and whether /SSAA may use
// multisampled renderbuffers (hardware AA) instead of larger targets.
struct EngineAa {
    int x = 1, y = 1;
    bool fxaa = false, hardware = true;
};
inline bool Same(const EngineAa& a, const EngineAa& b) {
    if (a.x != b.x || a.y != b.y || a.fxaa != b.fxaa) return false;
    return a.x * a.y == 1 || a.hardware == b.hardware;
}

// 2 -> 1x2, 4 -> 2x2, 8 -> 2x4, 16 -> 4x4, as /SSAA sets them.
inline void Factors(int samples, int* x, int* y) {
    switch (samples) {
    case 2: *x = 1, *y = 2; break;
    case 4: *x = 2, *y = 2; break;
    case 8: *x = 2, *y = 4; break;
    case 16: *x = 4, *y = 4; break;
    default: *x = 1, *y = 1; break;
    }
}

// "auto" -> -1 (follow the mods), "vanilla" -> 0 (never touch the engine's own /SSAA), or a sample count (1 = off).
inline bool ParseIni(const std::string& s, int* out) {
    if (_stricmp(s.c_str(), "auto") == 0) {
        *out = -1;
        return true;
    }
    if (_stricmp(s.c_str(), "vanilla") == 0) {
        *out = 0;
        return true;
    }
    if (_stricmp(s.c_str(), "off") == 0) {
        *out = 1;
        return true;
    }
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end || !ValidSamples(static_cast<int>(v))) return false;
    *out = static_cast<int>(v);
    return true;
}

struct Effective {
    int samples = 0;  // 0 = vanilla: the engine keeps its own /SSAA setting
    bool anyModRequest = false;
};

// The largest request among enabled mods (2 or 4), unless the ini names a count or "vanilla".
inline Effective Merge(int ini, const std::vector<int>& requests) {
    Effective e;
    for (int r : requests)
        if (ValidModSamples(r)) {
            e.anyModRequest = true;
            e.samples = std::max(e.samples, std::min(r, kMaxModSamples));
        }
    if (ini >= 0) e.samples = ini;
    return e;
}

// What the engine should hold: vanilla, or true supersampling at the effective count with FXAA off.
inline EngineAa Target(const Effective& e, const EngineAa& vanilla) {
    if (e.samples <= 0) return vanilla;
    EngineAa t;
    Factors(e.samples, &t.x, &t.y);
    t.hardware = false;
    t.fxaa = e.samples == 1 && vanilla.x * vanilla.y == 1 && vanilla.fxaa;
    return t;
}

// BeginScene's switch (the engine's DEBUG.ChangeSSAA): 1x1 -> 1x2 -> 2x2 -> 2x4 -> 4x4 -> FXAA -> 1x1.
inline EngineAa Step(EngineAa s) {
    if (s.fxaa) return {1, 1, false, s.hardware};
    if (s.x == 1 && s.y == 1) return {1, 2, false, s.hardware};
    if (s.x == 1 && s.y == 2) return {2, 2, false, s.hardware};
    if (s.x == 2 && s.y == 2) return {2, 4, false, s.hardware};
    if (s.x == 2 && s.y == 4) return {4, 4, false, s.hardware};
    return {1, 1, true, s.hardware};
}

// The state to write so that one Step lands on `t`.
inline EngineAa Before(const EngineAa& t) {
    EngineAa b{4, 4, false, t.hardware};
    if (t.fxaa) return b;
    if (t.x == 1 && t.y == 1) b = {1, 1, true, t.hardware};
    else if (t.x == 1 && t.y == 2) b = {1, 1, false, t.hardware};
    else if (t.x == 2 && t.y == 2) b = {1, 2, false, t.hardware};
    else if (t.x == 2 && t.y == 4) b = {2, 2, false, t.hardware};
    else b = {2, 4, false, t.hardware};
    return b;
}
}  // namespace melange::mirage::supersample::logic
