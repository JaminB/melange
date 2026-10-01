#pragma once
// Pure logic for MirageTextures (L1 "texture clarity"): merging enabled mods' spice.json "graphics" requests with
// the [MirageTextures] ini override into one effective setting, and parsing the ini's "auto" sentinel strings.
// No GL, no windows.h: offline self-tested in tests/textures_selftest.cpp.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace melange::mirage::textures::logic {
struct ModRequest {
    bool present = false, trilinear = false, lodBiasSet = false;
    int anisotropy = 0;
    float lodBias = 0.f;
};

struct Effective {
    int anisotropy = 0;    // 0 = off (vanilla)
    bool trilinear = false;
    float lodBias = 0.f;
    bool anyModRequest = false;
};

// Ini sentinels: -1 means "auto" (follow the mods' merged request, or vanilla if none ask for it).
struct IniOverride {
    int anisotropy = -1;
    int trilinear = -1;  // -1 auto, 0 off, 1 on
    bool lodBiasSet = false;
    float lodBias = 0.f;
};

// "auto" (case-insensitive) or an integer 0..maxAniso. False on anything else (caller should warn and keep auto).
inline bool ParseAnisotropy(const std::string& s, int maxAniso, int* out) {
    if (_stricmp(s.c_str(), "auto") == 0) {
        *out = -1;
        return true;
    }
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end || v < 0 || v > maxAniso) return false;
    *out = static_cast<int>(v);
    return true;
}

// "auto" | "on" | "off" (case-insensitive).
inline bool ParseTriState(const std::string& s, int* out) {
    if (_stricmp(s.c_str(), "auto") == 0) *out = -1;
    else if (_stricmp(s.c_str(), "on") == 0) *out = 1;
    else if (_stricmp(s.c_str(), "off") == 0) *out = 0;
    else return false;
    return true;
}

// "auto" or a float in [-lim, lim].
inline bool ParseLodBias(const std::string& s, float lim, bool* set, float* out) {
    if (_stricmp(s.c_str(), "auto") == 0) {
        *set = false;
        return true;
    }
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (end == s.c_str() || *end || v < -lim || v > lim) return false;
    *set = true;
    *out = static_cast<float>(v);
    return true;
}

// Merges every present mod request (max anisotropy, OR'd trilinear, the sharpest/most negative lodBias), then lets
// a non-auto ini value override that merged result field by field. The result is clamped to [0, maxAniso].
inline Effective Merge(const IniOverride& ini, const std::vector<ModRequest>& mods, int maxAniso) {
    Effective want;
    for (const ModRequest& r : mods) {
        if (!r.present) continue;
        want.anyModRequest = true;
        want.anisotropy = std::max(want.anisotropy, r.anisotropy);
        want.trilinear = want.trilinear || r.trilinear;
        if (r.lodBiasSet) want.lodBias = std::min(want.lodBias, r.lodBias);
    }
    Effective e = want;
    if (ini.anisotropy >= 0) e.anisotropy = ini.anisotropy;
    if (ini.trilinear == 0) e.trilinear = false;
    else if (ini.trilinear == 1) e.trilinear = true;
    if (ini.lodBiasSet) e.lodBias = ini.lodBias;
    e.anisotropy = std::clamp(e.anisotropy, 0, maxAniso);
    return e;
}
}  // namespace melange::mirage::textures::logic
