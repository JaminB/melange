#pragma once
// Pure helpers for scenery tints (tints.cpp, levels/scenery_tint.cpp). A level's scenery copy may carry a tint tag in its
// name, "_tint" and six hex digits (`VISIBLE_Horror5_dtl_tint3a302b`): the piece is drawn recoloured toward that colour
// by a GLSL replacement that declares `uniform vec4 mg_tint` (rgb the colour, a = 1 recolour; (1, 1, 1, 0) for every
// other draw, so a shader that reads it as "a > 0.5: recolour" leaves the rest untouched). A draw is matched to the tinted
// detail its model matrix places it at: the translation within a tolerance of the detail's world position and the matrix's
// scale within 4% of the detail's. Render state only: names are visual (scenery copies have no collision) and nothing here
// is read by the simulation. Offline self-tested in tests/shaders_selftest.cpp.
#include <cmath>
#include <cstdint>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace melange::mirage::tints::logic {
struct Entry {
    float pos[3] = {};    // world position of the detail, map units
    float scale = 1.f;    // mean axis scale of the detail's world matrix
    float rgba[4] = {1.f, 1.f, 1.f, 1.f};
};

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "_tint" + six hex digits, then the end of the name or '_'. The last such tag wins. False when there is none.
inline bool ParseTag(std::string_view name, float rgb[3]) {
    bool found = false;
    for (size_t at = name.find("_tint"); at != std::string_view::npos; at = name.find("_tint", at + 1)) {
        const size_t h = at + 5;
        if (h + 6 > name.size()) break;
        if (h + 6 < name.size() && name[h + 6] != '_') continue;
        int v[6];
        bool ok = true;
        for (int i = 0; i < 6 && ok; ++i) ok = (v[i] = HexDigit(name[h + i])) >= 0;
        if (!ok) continue;
        for (int i = 0; i < 3; ++i) rgb[i] = static_cast<float>(v[2 * i] * 16 + v[2 * i + 1]) / 255.f;
        found = true;
    }
    return found;
}

// Landscape tints: a scenery copy (any piece, anywhere: it is only a carrier) whose name carries "_ltint", the 16 hex
// digits of a landscape texture's digest (textags_logic.h's scheme, as Melange hashes the engine's upload) and six hex
// digits of colour, then the end of the name or '_' (`VISIBLE_Horror5_dtl_ltintbfb9ab5911ad8e293d3632`), recolours every
// landscape draw of that texture in the level
// toward the colour, a GLSL replacement that declares `uniform vec4 mg_landTint` doing the recolour. One level's
// texture, not a place: a theme's shared side texture (Prehistoric's grey P05 under path02 and grass01) turns basalt in
// that level only. Render state only, like the per-copy tint. "_ltint" never matches ParseTag ("_tint" is not in it).
inline bool ParseLandTag(std::string_view name, uint64_t* digest, float rgb[3]) {
    bool found = false;
    for (size_t at = name.find("_ltint"); at != std::string_view::npos; at = name.find("_ltint", at + 1)) {
        const size_t h = at + 6;
        if (h + 22 > name.size()) break;
        if (h + 22 < name.size() && name[h + 22] != '_') continue;
        int v[22];
        bool ok = true;
        for (int i = 0; i < 22 && ok; ++i) ok = (v[i] = HexDigit(name[h + i])) >= 0;
        if (!ok) continue;
        uint64_t d = 0;
        for (int i = 0; i < 16; ++i) d = (d << 4) | static_cast<uint64_t>(v[i]);
        *digest = d;
        for (int i = 0; i < 3; ++i) rgb[i] = static_cast<float>(v[16 + 2 * i] * 16 + v[16 + 2 * i + 1]) / 255.f;
        found = true;
    }
    return found;
}

// Mean length of the three linear rows of a Cg float4x4 as the engine sends it (four registers, rows; row 3 the
// translation for mul(float4(p, 1), model)).
inline float RowScale(const float* m) {
    float s = 0.f;
    for (int r = 0; r < 3; ++r) s += std::sqrt(m[r * 4] * m[r * 4] + m[r * 4 + 1] * m[r * 4 + 1] + m[r * 4 + 2] * m[r * 4 + 2]);
    return s / 3.f;
}

class Table {
  public:
    static constexpr float kCell = 8.f;
    // Units the engine's model matrices may use relative to map units (the engine's world is map units x 20).
    static constexpr float kUnits[2] = {1.f, 20.f};

    void Set(std::vector<Entry> e) {
        entries_ = std::move(e);
        grid_.clear();
        for (uint32_t i = 0; i < entries_.size(); ++i) grid_[Key(Cell(entries_[i].pos[0]), Cell(entries_[i].pos[2]))].push_back(i);
    }
    size_t Size() const { return entries_.size(); }
    const std::vector<Entry>& Entries() const { return entries_; }

    // The tinted detail a draw with this model matrix (16 floats, Cg rows) shows, or nullptr. Position tolerance:
    // 2% of the detail's scale plus 0.02 map units (a scenery mesh's root sits at the detail's origin); scale within 4%.
    const Entry* Find(const float* model) const {
        if (entries_.empty()) return nullptr;
        const float s = RowScale(model);
        for (float k : kUnits) {
            // The engine's world is map units x 20 in position, but its draw matrices keep the detail's own scale.
            const float t[3] = {model[12] / k, model[13] / k, model[14] / k};
            const int cx = Cell(t[0]), cz = Cell(t[2]);
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz) {
                    auto it = grid_.find(Key(cx + dx, cz + dz));
                    if (it == grid_.end()) continue;
                    for (uint32_t i : it->second) {
                        const Entry& e = entries_[i];
                        if (std::fabs(s - e.scale) > 0.04f * e.scale && std::fabs(s / k - e.scale) > 0.04f * e.scale) continue;
                        const float tol = 0.02f + 0.02f * e.scale;
                        const float d0 = t[0] - e.pos[0], d1 = t[1] - e.pos[1], d2 = t[2] - e.pos[2];
                        if (d0 * d0 + d1 * d1 + d2 * d2 <= tol * tol) return &e;
                    }
                }
        }
        return nullptr;
    }

  private:
    static int Cell(float v) { return static_cast<int>(std::floor(v / kCell)); }
    static uint64_t Key(int x, int z) { return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z); }
    std::vector<Entry> entries_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid_;
};
}  // namespace melange::mirage::tints::logic
