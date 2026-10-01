#pragma once
// Pure bookkeeping for the GLSL replacement runtime toggle (L0 "runtime enable/disable per mod GLSL program"):
// the persisted [MirageShaders] GlslDisabled list, as "file:entry" pairs. No GL, no Cg: offline self-tested in
// tests/shaders_selftest.cpp.
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace melange::mirage::shaders::glsl::logic {
inline bool IEquals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(),
                                               [](char x, char y) { return tolower(static_cast<unsigned char>(x)) == tolower(static_cast<unsigned char>(y)); });
}

using Pair = std::pair<std::string, std::string>;  // file, entry

// File names compare case-insensitively (Windows paths); Cg/GLSL entry point names compare exactly (they are
// case-sensitive symbols), matching shaders_glsl.cpp's own Key() convention.
inline bool Contains(const std::vector<Pair>& list, const std::string& file, const std::string& entry) {
    return std::any_of(list.begin(), list.end(), [&](const Pair& p) { return IEquals(p.first, file) && p.second == entry; });
}

// "File.cg:Entry,Other.cg:Entry2" (ini-persisted form). Malformed entries (no ':', or an empty side) are skipped.
inline std::vector<Pair> Decode(const std::string& s) {
    std::vector<Pair> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        std::string item = s.substr(p, q - p);
        size_t c = item.find(':');
        if (c != std::string::npos && c > 0 && c + 1 < item.size()) out.push_back({item.substr(0, c), item.substr(c + 1)});
        if (q == s.size()) break;
        p = q + 1;
    }
    return out;
}

inline std::string Encode(const std::vector<Pair>& list) {
    std::string out;
    for (const Pair& p : list) {
        if (!out.empty()) out += ',';
        out += p.first + ':' + p.second;
    }
    return out;
}

// Returns a new list with (file, entry) added (if `on` is false) or removed (if `on` is true); idempotent.
inline std::vector<Pair> SetEnabled(std::vector<Pair> list, const std::string& file, const std::string& entry, bool on) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Pair& p) { return IEquals(p.first, file) && p.second == entry; }),
               list.end());
    if (!on) list.push_back({file, entry});
    return list;
}
}  // namespace melange::mirage::shaders::glsl::logic
