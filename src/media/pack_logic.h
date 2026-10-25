#pragma once
// The media pack's pure rules (Melange 0.12.0, C4a): the manifest a build accepts, pack.json and the yt-dlp lane
// manifest, which URLs may be fetched, which zip entries may be extracted, and the lane's no-rollback serial. No
// Windows, no disk, no game: media_selftest runs every rule, and scripts/gen-media-manifest.py writes what CheckManifest
// accepts.
//
// pack.json (inside the zip, written by scripts/media-pack.ps1 in CI; gen-media-manifest.py is the reference reader):
//   {"format": "melange-media-pack-1", "version": "1",
//    "files": [{"path": "holo-media.exe", "size": 123, "sha256": "<64 lowercase hex>"}, ...]}
//   `files` lists every file of the pack except pack.json itself; the compiled manifest lists pack.json too.
// media-ytdlp.json (the rolling release media-ytdlp, published by CI, unsigned; section 7 of the design):
//   {"format": "melange-media-ytdlp-1", "version": "2026.10.21", "url": "https://github.com/JaminB/melange/releases/
//    download/media-ytdlp/yt-dlp-2026.10.21.exe", "size": 123, "sha256": "<hex>", "serial": 7}
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "tools/json_read.h"

namespace melange::media::pack::logic {
inline constexpr char kPackFormat[] = "melange-media-pack-1";
inline constexpr char kLaneFormat[] = "melange-media-ytdlp-1";
inline constexpr size_t kMaxFiles = 64;
inline constexpr uint64_t kMaxFileBytes = 512ull << 20;   // one file (libmpv is about 100 MB unpacked)
inline constexpr uint64_t kMaxPackBytes = 1ull << 30;     // the whole pack, unpacked
inline constexpr uint64_t kMaxZipBytes = 768ull << 20;
inline constexpr uint64_t kMaxLaneExeBytes = 96ull << 20;  // yt-dlp.exe is about 18 MB
inline constexpr uint64_t kMaxManifestBytes = 64 << 10;   // pack.json and media-ytdlp.json
inline constexpr uint64_t kFreeReserveBytes = 512ull << 20;
// The files the helper launch needs; a manifest without them is refused.
inline constexpr const char* kRequired[] = {"holo-media.exe", "libmpv-2.dll", "deno.exe", "pack.json"};

struct File {
    std::string path;  // '/' separated, relative, see SafePackPath
    uint64_t size = 0;
    std::string sha256;  // lowercase hex
};
struct Manifest {
    std::string version;  // also the folder name, Documents\Melange\media\<version>
    std::string zipUrl;
    uint64_t zipSize = 0;
    std::string zipSha256;
    std::string laneUrl;  // media-ytdlp.json
    bool dev = false;     // a locally assembled pack (file:///, scripts/make-dev-pack.ps1)
    std::vector<File> files;
    uint64_t UnpackedBytes() const {
        uint64_t n = 0;
        for (const File& f : files) n += f.size;
        return n;
    }
    const File* Find(std::string_view path) const;
};

// ---------------------------------------------------------------- character rules
inline char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool EqualNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}
inline bool ValidSha256Hex(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
// Folders of Documents\Melange\media that are not pack versions.
inline bool ReservedFolder(std::string_view s) { return EqualNoCase(s, "ytdlp") || EqualNoCase(s, "dl") || EqualNoCase(s, "cache"); }
// A pack version and folder name: ^[A-Za-z0-9][A-Za-z0-9._-]{0,31}$, not ending in '.', without "..", not reserved.
inline bool ValidVersion(std::string_view s) {
    if (s.empty() || s.size() > 32 || s.back() == '.' || s.find("..") != std::string_view::npos || ReservedFolder(s)) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && (i == 0 || (c != '.' && c != '_' && c != '-'))) return false;
    }
    return true;
}
// A yt-dlp version: digits and dots only, e.g. "2026.10.21" or "2026.10.21.1", 6..24 chars.
inline bool ValidLaneVersion(std::string_view s) {
    if (s.size() < 6 || s.size() > 24 || s.front() == '.' || s.back() == '.' || s.find("..") != std::string_view::npos)
        return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    return true;
}
// One path segment of a pack file: [A-Za-z0-9._-], 1..64 bytes, no leading or trailing '.', not a DOS device name.
inline bool SafeSegment(std::string_view s) {
    if (s.empty() || s.size() > 64 || s.front() == '.' || s.back() == '.') return false;
    for (char c : s) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                        c == '-';
        if (!ok) return false;
    }
    std::string_view stem = s.substr(0, s.find('.'));
    static constexpr const char* kDevices[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5",
                                               "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5",
                                               "lpt6", "lpt7", "lpt8", "lpt9", "conin$", "conout$"};
    for (const char* d : kDevices)
        if (EqualNoCase(stem, d)) return false;
    return true;
}
// "holo-media.exe", "LICENSES/mpv.txt": 1..3 safe segments joined by '/', at most 160 bytes. No '\', ':', "..",
// absolute paths, empty segments or a trailing '/'.
inline bool SafePackPath(std::string_view p) {
    if (p.empty() || p.size() > 160) return false;
    int segments = 0;
    size_t start = 0;
    for (;;) {
        const size_t slash = p.find('/', start);
        const std::string_view seg = p.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
        if (!SafeSegment(seg) || ++segments > 3) return false;
        if (slash == std::string_view::npos) return true;
        start = slash + 1;
    }
}

inline const File* Manifest::Find(std::string_view path) const {
    for (const File& f : files)
        if (EqualNoCase(f.path, path)) return &f;
    return nullptr;
}

// ---------------------------------------------------------------- URLs
// The pack and the lane come only from the allowed https prefix (cfg::kAllowedUrlPrefix); after it, only
// [A-Za-z0-9._/-] (no query, fragment, user info, "..", "//" or '\'). file:/// only where `allowFile` (a dev pack, a
// [Media] PackUrl test override, or a lane manifest that itself came from file:///).
inline bool AllowedUrl(std::string_view url, std::string_view prefix, bool allowFile) {
    if (url.size() > 2048) return false;
    for (unsigned char c : url)
        if (c <= 0x20 || c >= 0x7f || c == '\\') return false;
    if (allowFile && url.size() > 8 && EqualNoCase(url.substr(0, 8), "file:///")) return url.find("..") == std::string_view::npos;
    if (prefix.empty() || url.size() <= prefix.size() || url.substr(0, prefix.size()) != prefix) return false;
    const std::string_view rest = url.substr(prefix.size());
    if (rest.find("..") != std::string_view::npos || rest.find("//") != std::string_view::npos) return false;
    for (char c : rest) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                        c == '-' || c == '/';
        if (!ok) return false;
    }
    return true;
}
inline bool IsFileUrl(std::string_view url) { return url.size() > 8 && EqualNoCase(url.substr(0, 8), "file:///"); }

// [Media] PackUrl: "" keeps the compiled URL; otherwise only file:/// or the allowed prefix. The zip must still match
// the compiled size and SHA-256, so an override can only change where the same bytes come from.
inline bool ChoosePackUrl(std::string_view override_, const Manifest& m, std::string_view prefix, std::string* out,
                          std::string* why) {
    if (override_.empty()) {
        if (!AllowedUrl(m.zipUrl, prefix, m.dev)) {
            *why = "the compiled pack URL is not allowed";
            return false;
        }
        *out = m.zipUrl;
        return true;
    }
    if (!AllowedUrl(override_, prefix, true)) {
        *why = "[Media] PackUrl must be file:/// or start with " + std::string(prefix);
        return false;
    }
    *out = std::string(override_);
    return true;
}

// ---------------------------------------------------------------- the manifest
// The rules every accepted manifest meets (compiled, dev or a test's): a valid version, URL and hash, 1..kMaxFiles
// unique safe paths (case-insensitive, and no file is also another's folder), sizes within the caps, the required files.
inline bool CheckManifest(const Manifest& m, std::string* why) {
    auto fail = [&](std::string w) {
        *why = std::move(w);
        return false;
    };
    if (!ValidVersion(m.version)) return fail("bad pack version");
    if (m.zipSize == 0 || m.zipSize > kMaxZipBytes) return fail("bad zip size");
    if (!ValidSha256Hex(m.zipSha256)) return fail("bad zip sha256");
    if (m.files.empty() || m.files.size() > kMaxFiles) return fail("bad file count");
    uint64_t total = 0;
    for (size_t i = 0; i < m.files.size(); ++i) {
        const File& f = m.files[i];
        if (!SafePackPath(f.path)) return fail("unsafe path " + f.path);
        if (f.size > kMaxFileBytes) return fail(f.path + ": too large");
        if (!ValidSha256Hex(f.sha256)) return fail(f.path + ": bad sha256");
        total += f.size;
        for (size_t j = 0; j < i; ++j) {
            const std::string& o = m.files[j].path;
            if (EqualNoCase(o, f.path)) return fail("duplicate path " + f.path);
            const std::string& shortP = o.size() < f.path.size() ? o : f.path;
            const std::string& longP = o.size() < f.path.size() ? f.path : o;
            if (longP.size() > shortP.size() && longP[shortP.size()] == '/' && EqualNoCase(longP.substr(0, shortP.size()), shortP))
                return fail("a file is also a folder: " + shortP);
        }
    }
    if (total > kMaxPackBytes) return fail("the pack is too large");
    for (const char* r : kRequired)
        if (!m.Find(r)) return fail(std::string("the pack has no ") + r);
    return true;
}

namespace detail {
inline const std::string* Str(const json::Value& o, const char* k) {
    const json::Value* v = o.Get(k);
    return v && v->IsString() ? &v->string : nullptr;
}
inline bool UInt(const json::Value& o, const char* k, uint64_t* out) {
    const json::Value* v = o.Get(k);
    if (!v || !v->IsInteger() || v->number < 0) return false;
    *out = static_cast<uint64_t>(v->number);
    return true;
}
}  // namespace detail

// pack.json: format, version and the file list (pack.json itself excluded). Strict: unknown members are allowed (a
// later CI may add provenance), but every file must be well formed.
struct PackJson {
    std::string version;
    std::vector<File> files;
};
inline bool ParsePackJson(std::string_view text, PackJson* out, std::string* why) {
    json::Value v;
    json::Error e;
    if (text.size() > kMaxManifestBytes || !json::Parse(text, &v, &e) || !v.IsObject()) {
        *why = "pack.json is not a JSON object";
        return false;
    }
    const std::string* fmt = detail::Str(v, "format");
    const std::string* ver = detail::Str(v, "version");
    const json::Value* files = v.Get("files");
    if (!fmt || *fmt != kPackFormat) {
        *why = "pack.json has an unknown format";
        return false;
    }
    if (!ver || !ValidVersion(*ver) || !files || !files->IsArray() || files->items.size() > kMaxFiles) {
        *why = "pack.json has no valid version or file list";
        return false;
    }
    out->version = *ver;
    out->files.clear();
    for (const json::Value& f : files->items) {
        File x;
        const std::string* p = f.IsObject() ? detail::Str(f, "path") : nullptr;
        const std::string* h = f.IsObject() ? detail::Str(f, "sha256") : nullptr;
        if (!p || !h || !detail::UInt(f, "size", &x.size) || !SafePackPath(*p) || !ValidSha256Hex(*h) || x.size > kMaxFileBytes ||
            EqualNoCase(*p, "pack.json")) {
            *why = "pack.json has a bad file entry";
            return false;
        }
        x.path = *p;
        x.sha256 = *h;
        out->files.push_back(std::move(x));
    }
    return true;
}

// The extracted pack.json must describe exactly the compiled manifest (minus pack.json itself): the same version and
// the same files with the same sizes and hashes. It ties CI's file to the build's.
inline bool PackJsonMatches(const PackJson& pj, const Manifest& m, std::string* why) {
    if (pj.version != m.version) {
        *why = "pack.json is for version " + pj.version + ", this Melange expects " + m.version;
        return false;
    }
    size_t want = 0;
    for (const File& f : m.files) {
        if (EqualNoCase(f.path, "pack.json")) continue;
        ++want;
        const File* g = nullptr;
        for (const File& x : pj.files)
            if (EqualNoCase(x.path, f.path)) g = &x;
        if (!g || g->size != f.size || g->sha256 != f.sha256) {
            *why = "pack.json disagrees about " + f.path;
            return false;
        }
    }
    if (pj.files.size() != want) {
        *why = "pack.json lists files this Melange does not expect";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- the zip
struct ZipEntry {
    std::string name;
    uint64_t size = 0;      // uncompressed, from the central directory
    bool dir = false;       // a folder entry ("LICENSES/")
    bool encrypted = false;
    int method = 0;         // 0 stored, 8 deflate
};
// The extraction plan: for each entry, the index of its manifest file, or -1 for a folder entry that is the parent of
// a manifest file. Refused: any name not in the manifest (exact bytes, so no case or slash games), a second entry of
// the same name, an encrypted entry, a method other than stored or deflate, a central-directory size that differs from
// the manifest, and a manifest file missing from the zip.
inline bool PlanExtract(const std::vector<ZipEntry>& entries, const Manifest& m, std::vector<int>* plan, std::string* why) {
    plan->assign(entries.size(), -1);
    std::vector<bool> seen(m.files.size(), false);
    if (entries.size() > kMaxFiles * 4) {
        *why = "the zip has too many entries";
        return false;
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        const ZipEntry& e = entries[i];
        if (e.encrypted) {
            *why = e.name + ": encrypted";
            return false;
        }
        if (e.dir) {
            std::string_view d = e.name;
            if (d.empty() || d.back() != '/') {
                *why = "bad folder entry";
                return false;
            }
            d.remove_suffix(1);
            bool parent = false;
            for (const File& f : m.files)
                if (f.path.size() > d.size() && f.path.compare(0, d.size(), d) == 0 && f.path[d.size()] == '/') parent = true;
            if (!parent || e.size != 0) {
                *why = e.name + ": not in the manifest";
                return false;
            }
            continue;
        }
        int idx = -1;
        for (size_t k = 0; k < m.files.size(); ++k)
            if (m.files[k].path == e.name) idx = static_cast<int>(k);
        if (idx < 0) {
            *why = (SafePackPath(e.name) ? e.name : std::string("an unsafe name")) + ": not in the manifest";
            return false;
        }
        if (seen[static_cast<size_t>(idx)]) {
            *why = e.name + ": appears twice";
            return false;
        }
        if (e.method != 0 && e.method != 8) {
            *why = e.name + ": unsupported compression";
            return false;
        }
        if (e.size != m.files[static_cast<size_t>(idx)].size) {
            *why = e.name + ": size differs from the manifest";
            return false;
        }
        seen[static_cast<size_t>(idx)] = true;
        (*plan)[i] = idx;
    }
    for (size_t k = 0; k < m.files.size(); ++k)
        if (!seen[k]) {
            *why = m.files[k].path + ": missing from the zip";
            return false;
        }
    return true;
}

// ---------------------------------------------------------------- the yt-dlp lane
struct Lane {
    std::string version, url, sha256;
    uint64_t size = 0, serial = 0;
};
// media-ytdlp.json. The exe URL must be allowed like the pack's (file:/// only when the manifest itself came from
// file:///, a dev lane).
inline bool ParseLane(std::string_view text, std::string_view prefix, bool allowFile, Lane* out, std::string* why) {
    json::Value v;
    json::Error e;
    if (text.size() > kMaxManifestBytes || !json::Parse(text, &v, &e) || !v.IsObject()) {
        *why = "the yt-dlp manifest is not a JSON object";
        return false;
    }
    const std::string* fmt = detail::Str(v, "format");
    const std::string* ver = detail::Str(v, "version");
    const std::string* url = detail::Str(v, "url");
    const std::string* sha = detail::Str(v, "sha256");
    Lane l;
    if (!fmt || *fmt != kLaneFormat) {
        *why = "the yt-dlp manifest has an unknown format";
        return false;
    }
    if (!ver || !ValidLaneVersion(*ver) || !url || !AllowedUrl(*url, prefix, allowFile) || !sha || !ValidSha256Hex(*sha) ||
        !detail::UInt(v, "size", &l.size) || l.size == 0 || l.size > kMaxLaneExeBytes || !detail::UInt(v, "serial", &l.serial) ||
        l.serial == 0) {
        *why = "the yt-dlp manifest has a bad version, url, size, sha256 or serial";
        return false;
    }
    l.version = *ver;
    l.url = *url;
    l.sha256 = *sha;
    *out = std::move(l);
    return true;
}

// What to do with an offered lane, given what is installed (`have.serial` 0 = nothing installed).
enum class LaneAction : uint8_t {
    Install,   // a higher serial with a new file
    Record,    // a higher serial for the very file and version already installed: just remember the serial
    Current,   // the same serial and file
    Rollback,  // a lower serial: refused (no going back to an older, exploitable yt-dlp)
    Conflict,  // the same serial for a different file: refused, the manifest was edited without a bump
};
inline LaneAction DecideLane(const Lane& have, const Lane& offer) {
    if (have.serial == 0) return LaneAction::Install;
    if (offer.serial < have.serial) return LaneAction::Rollback;
    const bool same = offer.sha256 == have.sha256 && offer.size == have.size && offer.version == have.version;
    if (offer.serial == have.serial) return same ? LaneAction::Current : LaneAction::Conflict;
    return same ? LaneAction::Record : LaneAction::Install;
}
inline const char* LaneActionName(LaneAction a) {
    switch (a) {
        case LaneAction::Install: return "install";
        case LaneAction::Record: return "record";
        case LaneAction::Current: return "current";
        case LaneAction::Rollback: return "rollback";
        case LaneAction::Conflict: return "conflict";
    }
    return "?";
}
// Lane checks: at most once per 24 h; a forced check (yt-dlp reported a YouTube change) at most once per 10 minutes.
inline constexpr int64_t kLaneIntervalS = 24 * 3600;
inline constexpr int64_t kLaneForceIntervalS = 600;
inline bool LaneCheckDue(int64_t now, int64_t lastCheck, bool force) {
    if (lastCheck <= 0 || now < lastCheck) return true;
    return now - lastCheck >= (force ? kLaneForceIntervalS : kLaneIntervalS);
}

// ---------------------------------------------------------------- downloads
// Free space for an install: the unpacked pack, the zip and a 512 MiB floor (design 1.1.1).
inline uint64_t NeedFreeBytes(const Manifest& m) { return m.UnpackedBytes() + m.zipSize + kFreeReserveBytes; }
// The throttle: how long to sleep after `bytes` arrived `elapsedMs` into a window, to stay at `kbps` KiB/s (0 = none).
inline uint32_t ThrottleSleepMs(uint64_t bytes, uint64_t elapsedMs, uint32_t kbps) {
    if (!kbps) return 0;
    const uint64_t dueMs = bytes * 1000 / (static_cast<uint64_t>(kbps) * 1024);
    return dueMs > elapsedMs ? static_cast<uint32_t>((dueMs - elapsedMs) > 1000 ? 1000 : dueMs - elapsedMs) : 0;
}
// Folders under Documents\Melange\media that a sweep may delete: "<version>.tmp" (a dead install), "*.del-<hex>.tmp"
// (an uninstall or replaced pack), and pack folders of another version (they hold a pack.json; the caller checks).
inline bool IsTmpFolder(std::string_view name) {
    return name.size() > 4 && EqualNoCase(name.substr(name.size() - 4), ".tmp") && ValidVersion(name.substr(0, name.size() - 4));
}
}  // namespace melange::media::pack::logic
