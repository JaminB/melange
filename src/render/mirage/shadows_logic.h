#pragma once
// Pure logic for MirageShadows: merging enabled mods' shadow-map size requests with the [MirageShadows] ini
// override. No GL, no windows.h: offline self-tested in tests/textures_selftest.cpp.
#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace melange::mirage::shadows::logic {
constexpr int kMaxSize = 4096;

inline bool ValidSize(int s) { return s == 512 || s == 1024 || s == 2048 || s == 4096; }

// A mod's request: its runtime one (Lua) when set, else its manifest's. -1 = no runtime request.
inline int ModRequest(int manifest, int runtime) { return runtime >= 0 ? runtime : manifest; }

// "auto" -> -1 (follow the mods), "vanilla" -> 0 (never touch the engine's own size), or a valid size.
inline bool ParseIni(const std::string& s, int* out) {
    if (_stricmp(s.c_str(), "auto") == 0) {
        *out = -1;
        return true;
    }
    if (_stricmp(s.c_str(), "vanilla") == 0) {
        *out = 0;
        return true;
    }
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end || !ValidSize(static_cast<int>(v))) return false;
    *out = static_cast<int>(v);
    return true;
}

struct Effective {
    int size = 0;  // 0 = vanilla: the engine keeps the size from its own cfg files
    bool anyModRequest = false;
};

// The largest request among enabled mods, unless the ini names a size or "vanilla". Capped at kMaxSize.
inline Effective Merge(int ini, const std::vector<int>& requests) {
    Effective e;
    for (int r : requests)
        if (r > 0) {
            e.anyModRequest = true;
            e.size = std::max(e.size, std::min(r, kMaxSize));
        }
    if (ini >= 0) e.size = std::min(ini, kMaxSize);
    return e;
}

// The value the engine field should hold: the effective size, or the cfg's own value when vanilla.
inline int Target(const Effective& e, int vanilla) { return e.size > 0 ? e.size : vanilla; }
}  // namespace melange::mirage::shadows::logic
