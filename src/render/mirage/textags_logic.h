#pragma once
// Pure logic for MirageTexTags (texture tags): the digest scheme that keys a texture by its base level, the plugin's
// tag table (JSON) and how several mods' tables merge, and the per-name/per-unit bookkeeping that maps the texture
// bound on a unit to its tag. No GL, no windows.h beyond what the hash needs: offline self-tested in
// tests/textags_selftest.cpp. tools/tag_digest.py is the reference implementation of the digest.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "tools/hash.h"
#include "tools/json_read.h"

namespace melange::mirage::textags::logic {
// The digest scheme's name, as a table's "digest" field must spell it.
//
// digest = the first 8 bytes of SHA-256(header || pixels), written as 16 lowercase hex digits (big-endian, i.e. the
// first 16 hex digits of the SHA-256 hex string), where
//   header = 5 little-endian uint32: width, height, internalformat, format, type -- the glTexImage2D arguments of the
//            texture's level 0 as the engine uploads it (glCompressedTexImage2D: format = type = 0);
//   pixels = level 0's bytes in upload order, rows tightly packed (no GL_UNPACK_ALIGNMENT/ROW_LENGTH padding), first
//            byte = first texel of GL row 0 (texture v = 0). For a compressed level, the imageSize bytes as given.
// The engine (#1077, 0x79dc50) uploads XImage format 0 as (GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE) and formats 1 and 2 as
// (GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE), straight from the XImage's bottom-up rows, so a top-down image (PNG) is
// flipped first. Bytes changed by an upload patcher (assets/upload.h) are hashed as patched.
inline constexpr const char* kScheme = "gl-level0-sha256-64";
inline constexpr uint32_t kRGB8 = 0x8051, kRGBA8 = 0x8058, kRGB = 0x1907, kRGBA = 0x1908, kUByte = 0x1401;
inline constexpr int kTagFloats = 8;  // two vec4: mg_texTag[0], mg_texTag[1]
inline constexpr size_t kMaxTags = 65536;

using Tag = std::array<float, kTagFloats>;

inline void Header(uint8_t out[20], uint32_t w, uint32_t h, uint32_t internalFormat, uint32_t format, uint32_t type) {
    const uint32_t v[5] = {w, h, internalFormat, format, type};
    for (int i = 0; i < 5; ++i)
        for (int b = 0; b < 4; ++b) out[i * 4 + b] = static_cast<uint8_t>(v[i] >> (8 * b));
}

// 16 hex digits -> value; false on anything else (length, non-hex). Upper case is accepted.
inline bool ParseHex64(std::string_view s, uint64_t* out) {
    if (s.size() != 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | static_cast<uint64_t>(d);
    }
    *out = v;
    return true;
}

inline std::string Hex64(uint64_t v) {
    static const char* k = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i, v >>= 4) s[static_cast<size_t>(i)] = k[v & 15];
    return s;
}

// Bytes per pixel of an uncompressed (format, type) the digest supports with row repacking; 0 for anything else.
inline int BytesPerPixel(uint32_t format, uint32_t type) {
    if (type == kUByte) {
        switch (format) {
        case kRGB: case 0x80E0 /*BGR*/: return 3;
        case kRGBA: case 0x80E1 /*BGRA*/: return 4;
        case 0x1906 /*ALPHA*/: case 0x1909 /*LUMINANCE*/: case 0x1903 /*RED*/: return 1;
        case 0x190A /*LUMINANCE_ALPHA*/: return 2;
        default: return 0;
        }
    }
    if ((type == 0x8366 || type == 0x8034 || type == 0x8033 || type == 0x8365) && (format == kRGBA || format == 0x80E1)) return 2;
    if (type == 0x8363 && format == kRGB) return 2;
    return 0;
}

// Source row stride under GL_UNPACK_ALIGNMENT/GL_UNPACK_ROW_LENGTH.
inline size_t SourceStride(int w, int bpp, int alignment, int rowLength) {
    size_t row = static_cast<size_t>(rowLength > 0 ? rowLength : w) * static_cast<size_t>(bpp);
    if (alignment > 1) row = (row + static_cast<size_t>(alignment) - 1) / static_cast<size_t>(alignment) * static_cast<size_t>(alignment);
    return row;
}

// For the glGetTexImage read-back: the upload (format, type) an engine-uploaded internal format came from.
inline bool ReadbackFormat(uint32_t internalFormat, uint32_t* format, uint32_t* type) {
    if (internalFormat == kRGB8) *format = kRGB, *type = kUByte;
    else if (internalFormat == kRGBA8) *format = kRGBA, *type = kUByte;
    else return false;
    return true;
}

// The digest of one level: the header, then the rows (tightly packed whatever the source stride).
// `rows` rows of `rowBytes` each, `stride` apart in memory.
inline bool DigestRows(const uint8_t hdr[20], const uint8_t* src, size_t rows, size_t rowBytes, size_t stride, uint64_t* out) {
    if (!src || !rows || !rowBytes || stride < rowBytes) return false;
    hashutil::Sha256 sha;
    bool ok = sha.Update(hdr, 20);
    if (stride == rowBytes) ok = ok && sha.Update(src, rowBytes * rows);
    else
        for (size_t y = 0; ok && y < rows; ++y) ok = sha.Update(src + stride * y, rowBytes);
    std::string hex = ok ? sha.FinishHex() : std::string();
    return hex.size() >= 16 && ParseHex64(std::string_view(hex).substr(0, 16), out);
}

inline bool Digest(uint32_t w, uint32_t h, uint32_t internalFormat, uint32_t format, uint32_t type, const uint8_t* src,
                   size_t rowBytes, size_t stride, uint64_t* out) {
    if (!w || !h) return false;
    uint8_t hdr[20];
    Header(hdr, w, h, internalFormat, format, type);
    return DigestRows(hdr, src, h, rowBytes, stride, out);
}

// A compressed level: imageSize bytes as given, format = type = 0 in the header.
inline bool DigestCompressed(uint32_t w, uint32_t h, uint32_t internalFormat, const uint8_t* src, size_t imageSize, uint64_t* out) {
    if (!w || !h) return false;
    uint8_t hdr[20];
    Header(hdr, w, h, internalFormat, 0, 0);
    return DigestRows(hdr, src, 1, imageSize, imageSize, out);
}

struct Table {
    std::unordered_map<uint64_t, uint32_t> index;  // digest -> row in tags
    std::vector<Tag> tags;
    std::vector<std::string> sources;              // mod ids, in merge order
};

// Parses one table file. Rows: "<16 hex>": [1..8 finite numbers] (missing trailing values are 0). A digest twice in
// the same file is an error (strict JSON already refuses duplicate keys). Errors name the row.
inline bool ParseTable(const json::Value& v, Table* out, std::string* err) {
    auto fail = [&](std::string s) {
        *err = std::move(s);
        return false;
    };
    if (!v.IsObject()) return fail("the table must be a JSON object");
    const json::Value* ver = v.Get("version");
    if (!ver || !ver->IsInteger() || ver->number != 1) return fail("\"version\" must be 1");
    const json::Value* dg = v.Get("digest");
    if (!dg || !dg->IsString() || dg->string != kScheme) return fail(std::string("\"digest\" must be \"") + kScheme + "\"");
    const json::Value* tags = v.Get("tags");
    if (!tags || !tags->IsObject()) return fail("\"tags\" must be an object");
    if (tags->members.size() > kMaxTags) return fail("more than 65536 tags");
    out->index.reserve(tags->members.size());
    out->tags.reserve(tags->members.size());
    for (const auto& [key, row] : tags->members) {
        uint64_t d = 0;
        if (!ParseHex64(key, &d)) return fail("tag key \"" + key + "\" is not 16 hex digits");
        if (!row.IsArray() || row.items.empty() || row.items.size() > kTagFloats)
            return fail("tag \"" + key + "\" must be an array of 1 to 8 numbers");
        Tag t{};
        for (size_t i = 0; i < row.items.size(); ++i) {
            if (!row.items[i].IsNumber() || !std::isfinite(row.items[i].number) || std::fabs(row.items[i].number) > 1e30)
                return fail("tag \"" + key + "\" has a value that is not a finite number");
            t[i] = static_cast<float>(row.items[i].number);
        }
        if (!out->index.emplace(d, static_cast<uint32_t>(out->tags.size())).second) return fail("tag \"" + key + "\" appears twice");
        out->tags.push_back(t);
    }
    return true;
}

inline bool ParseTableText(std::string_view text, Table* out, std::string* err) {
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, 16u << 20)) {
        *err = "line " + std::to_string(e.line) + ": " + e.text;
        return false;
    }
    return ParseTable(v, out, err);
}

// Adds `from`'s rows that `into` does not have yet: the first declared table wins a digest. Returns the rows added;
// *dupes counts the ones ignored.
inline size_t Merge(Table* into, const Table& from, const std::string& source, size_t* dupes) {
    size_t added = 0;
    *dupes = 0;
    for (const auto& [d, row] : from.index) {
        if (into->index.size() >= kMaxTags || into->index.count(d)) {
            ++*dupes;
            continue;
        }
        into->index.emplace(d, static_cast<uint32_t>(into->tags.size()));
        into->tags.push_back(from.tags[row]);
        ++added;
    }
    into->sources.push_back(source);
    return added;
}

// GL texture name -> what Melange knows about its level 0, plus the 2D binding of each texture unit.
class Tracker {
public:
    static constexpr uint32_t kUnknown = 0xFFFFFFFFu;
    static constexpr int kUnits = 32;
    static constexpr size_t kMaxNames = 65536;
    struct Entry {
        uint64_t digest = 0;
        int32_t row = -1;            // tag row, -1 = untagged
        uint16_t w = 0, h = 0;
        bool hashed = false;         // digest is valid
        bool dynamic = false;        // re-specified outside the engine loader: never read back, never tagged
        bool drawnTagged = false, drawnUntagged = false;
    };

    Tracker() {
        for (uint32_t& b : bound_) b = kUnknown;
    }

    // State known from the start (hooks installed before the context exists): every unit has texture 0.
    void ResetBindings(bool known) {
        for (uint32_t& b : bound_) b = known ? 0 : kUnknown;
        active_ = 0;
        activeKnown_ = known;
    }
    // A unit past the tracked ones may be a real unit (GL has up to 192) or an invalid enum GL refuses, keeping the old
    // unit: the next bind cannot be placed either way, so it forgets every binding (each is read back when needed).
    void ActiveTexture(uint32_t unit) {
        if (unit < kUnits) active_ = unit;
        activeKnown_ = unit < kUnits;
    }
    void Bind(uint32_t name) {
        if (activeKnown_) bound_[active_] = name;
        else ResetBindings(false);
    }
    uint32_t Bound(uint32_t unit) const { return unit < kUnits ? bound_[unit] : kUnknown; }
    void SetBound(uint32_t unit, uint32_t name) {
        if (unit < kUnits) bound_[unit] = name;
    }
    uint32_t Active() const { return active_; }
    bool ActiveKnown() const { return activeKnown_; }

    // A hashed level 0 from the engine's loader (or a read-back).
    void Hashed(uint32_t name, uint64_t digest, uint16_t w, uint16_t h, const Table& t) {
        if (!name) return;
        Entry* e = Slot(name);
        if (!e) return;
        *e = Entry{};
        e->digest = digest;
        e->hashed = true;
        e->w = w, e->h = h;
        auto it = t.index.find(digest);
        e->row = it != t.index.end() ? static_cast<int32_t>(it->second) : -1;
        ++gen_;
    }
    // Level 0 re-specified by anything else (or not hashable): the name stays untagged and is not read back.
    void Dynamic(uint32_t name) {
        if (!name) return;
        Entry* e = Slot(name);
        if (!e || (e->dynamic && !e->hashed)) return;  // already: no new generation for a texture updated every frame
        *e = Entry{};
        e->dynamic = true;
        ++gen_;
    }
    // Drops what is known about a name without touching the bindings (it may be read back later).
    void Forget(uint32_t name) {
        if (names_.erase(name)) ++gen_;
    }
    // glDeleteTextures: the name is gone, and every unit that had it bound now has texture 0.
    void Delete(uint32_t name) {
        if (names_.erase(name)) ++gen_;
        for (uint32_t& b : bound_)
            if (b == name) b = 0;
    }
    // A new table: recompute every hashed name's row.
    void Retag(const Table& t) {
        for (auto& [n, e] : names_) {
            if (!e.hashed) continue;
            auto it = t.index.find(e.digest);
            e.row = it != t.index.end() ? static_cast<int32_t>(it->second) : -1;
        }
        ++gen_;
    }
    const Entry* Find(uint32_t name) const {
        auto it = names_.find(name);
        return it != names_.end() ? &it->second : nullptr;
    }
    Entry* FindMut(uint32_t name) {
        auto it = names_.find(name);
        return it != names_.end() ? &it->second : nullptr;
    }
    uint32_t Gen() const { return gen_; }
    size_t Size() const { return names_.size(); }
    const std::unordered_map<uint32_t, Entry>& Names() const { return names_; }
    void Clear() {
        names_.clear();
        ++gen_;
    }

private:
    Entry* Slot(uint32_t name) {
        auto it = names_.find(name);
        if (it != names_.end()) return &it->second;
        if (names_.size() >= kMaxNames) return nullptr;
        return &names_[name];
    }
    std::unordered_map<uint32_t, Entry> names_;
    uint32_t bound_[kUnits];
    uint32_t active_ = 0;
    bool activeKnown_ = false;
    uint32_t gen_ = 1;
};
}  // namespace melange::mirage::textags::logic
