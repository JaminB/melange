#pragma once
// wum.media.classify and the URL rules every media path applies: which links are accepted, their canonical form, and
// magnet canonicalisation (Critique S5). Pure and header-only; compiled into the x86 core and the x64 helper, which
// both re-classify every URL they are given, so a raw string from Lua or a peer never reaches yt-dlp, mpv or
// libtorrent. Frozen for Melange 0.12.0.
//
// Accepted:
//   https://(www.|m.|music.)youtube.com/watch?v=<id>[&list=<list>]   https://(...)youtube.com/playlist?list=<list>
//   https://(...)youtube.com/shorts/<id>   https://(...)youtube.com/live/<id>   https://youtu.be/<id>[?list=<list>]
//   magnet:?xt=urn:btih:<40 hex | 32 base32>  or  magnet:?xt=urn:btmh:1220<64 hex>
// Refused: every other scheme (http:, file:, ...), host, port, user info and path.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "media/media_proto.h"

namespace melange::media::url {
enum class Kind : uint8_t { None, YouTube, Magnet };

struct YouTube {
    std::string id;        // 11 chars, or empty for a playlist link
    std::string playlist;  // list id, or empty
    bool shorts = false, live = false;
};

struct Magnet {
    std::string infohash;  // lowercase hex: 40 chars (v1, btih) or 64 (v2, btmh sha2-256)
    std::string name;      // dn, at most kMaxNameBytes, control characters removed
    std::vector<std::string> trackers;  // udp/http/https only, at most kMaxTrackers
    bool v2() const { return infohash.size() == 64; }
};

struct Classified {
    Kind kind = Kind::None;
    std::string url;  // canonical: the only form stored, sent or given to the helper
    YouTube yt;
    Magnet bt;
};

// ---------------------------------------------------------------- character rules
inline bool IdChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}
inline bool ValidVideoId(std::string_view s) {  // ^[A-Za-z0-9_-]{11}$
    if (s.size() != 11) return false;
    for (char c : s)
        if (!IdChar(c)) return false;
    return true;
}
inline bool ValidPlaylistId(std::string_view s) {  // ^[A-Za-z0-9_-]{10,64}$
    if (s.size() < 10 || s.size() > 64) return false;
    for (char c : s)
        if (!IdChar(c)) return false;
    return true;
}
inline bool IsHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
inline char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
inline bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}
inline bool IStarts(std::string_view s, std::string_view pre) { return s.size() >= pre.size() && IEquals(s.substr(0, pre.size()), pre); }

// A lowercase 40- or 64-char hex infohash (the form bt.* messages carry).
inline bool ValidInfohash(std::string_view s) {
    if (s.size() != 40 && s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Audio and subtitle language codes: ^[a-z]{2,3}(-[A-Za-z0-9]{2,8})?$ (checked in core and in the helper; anything
// else is dropped, never passed on to yt-dlp or mpv).
inline bool ValidLang(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && s[i] >= 'a' && s[i] <= 'z') ++i;
    if (i < 2 || i > 3) return false;
    if (i == s.size()) return true;
    if (s[i] != '-') return false;
    const size_t tail = s.size() - i - 1;
    if (tail < 2 || tail > 8) return false;
    for (size_t j = i + 1; j < s.size(); ++j) {
        const char c = s[j];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

inline std::string_view Trim(std::string_view s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (!s.empty() && ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && ws(s.back())) s.remove_suffix(1);
    return s;
}

// Percent-decoding of one query value; false on a broken escape. plusIsSpace for dn (form encoding).
inline bool PercentDecode(std::string_view in, std::string* out, bool plusIsSpace) {
    out->clear();
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size() || !IsHex(in[i + 1]) || !IsHex(in[i + 2])) return false;  // "%4" at the end too
            auto nib = [](char h) { return h <= '9' ? h - '0' : Lower(h) - 'a' + 10; };
            out->push_back(static_cast<char>(nib(in[i + 1]) * 16 + nib(in[i + 2])));
            i += 2;
        } else if (c == '+' && plusIsSpace) {
            out->push_back(' ');
        } else {
            out->push_back(c);
        }
    }
    return true;
}

// RFC 3986 unreserved characters stay, everything else becomes %XX (uppercase).
inline std::string PercentEncode(std::string_view in) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size());
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

// Splits "a=1&b=2" into pairs (raw, still encoded). Empty segments are skipped.
inline std::vector<std::pair<std::string_view, std::string_view>> SplitQuery(std::string_view q) {
    std::vector<std::pair<std::string_view, std::string_view>> out;
    while (!q.empty()) {
        const size_t amp = q.find('&');
        const std::string_view part = q.substr(0, amp);
        q = amp == std::string_view::npos ? std::string_view{} : q.substr(amp + 1);
        if (part.empty()) continue;
        const size_t eq = part.find('=');
        if (eq == std::string_view::npos) out.emplace_back(part, std::string_view{});
        else out.emplace_back(part.substr(0, eq), part.substr(eq + 1));
        if (out.size() > 64) break;  // a link with more parameters than this is not one we accept anyway
    }
    return out;
}

// ---------------------------------------------------------------- torrent file types (Critique S3)
inline std::string_view Extension(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    return dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
}
// Files the picker may offer and the helper may open: mkv, mp4, m4v, webm, avi, mov, ts, ogv.
inline bool IsVideoFile(std::string_view path) {
    const std::string_view e = Extension(path);
    for (const char* x : {"mkv", "mp4", "m4v", "webm", "avi", "mov", "ts", "ogv"})
        if (IEquals(e, x)) return true;
    return false;
}
// Playlist-like files that could make mpv open other files or URLs: never opened, never listed as video.
inline bool IsRefusedFile(std::string_view path) {
    const std::string_view e = Extension(path);
    for (const char* x : {"m3u", "m3u8", "pls", "edl", "cue", "url", "txt", "xspf", "asx", "wpl"})
        if (IEquals(e, x)) return true;
    return false;
}

// ---------------------------------------------------------------- canonical forms
inline std::string CanonicalVideoUrl(std::string_view id) { return "https://www.youtube.com/watch?v=" + std::string(id); }
inline std::string CanonicalPlaylistUrl(std::string_view list) {
    return "https://www.youtube.com/playlist?list=" + std::string(list);
}

// Drops trackers from the end, then shortens dn, until the result fits kMaxMagnetBytes (it always fits without
// both, since an infohash link is under 100 bytes).
inline std::string CanonicalMagnet(Magnet* m) {
    auto build = [&] {
        std::string s = m->v2() ? "magnet:?xt=urn:btmh:1220" + m->infohash : "magnet:?xt=urn:btih:" + m->infohash;
        if (!m->name.empty()) s += "&dn=" + PercentEncode(m->name);
        for (const std::string& t : m->trackers) s += "&tr=" + PercentEncode(t);
        return s;
    };
    std::string s = build();
    while (s.size() > proto::kMaxMagnetBytes && !m->trackers.empty()) {
        m->trackers.pop_back();
        s = build();
    }
    while (s.size() > proto::kMaxMagnetBytes && !m->name.empty()) {
        size_t n = m->name.size() > 16 ? m->name.size() - 16 : 0;
        while (n > 0 && (static_cast<unsigned char>(m->name[n]) & 0xC0) == 0x80) --n;  // whole UTF-8 sequences only
        m->name.resize(n);
        s = build();
    }
    return s;
}

// ---------------------------------------------------------------- parts
// Cuts a name to at most `cap` bytes on a UTF-8 boundary and removes control characters.
inline std::string CleanName(std::string_view in, size_t cap) {
    std::string out;
    for (unsigned char c : in)
        if (c >= 0x20 && c != 0x7F) out.push_back(static_cast<char>(c));
    if (out.size() > cap) {
        size_t n = cap;
        while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
        out.resize(n);
    }
    return out;
}

inline bool Base32ToHex(std::string_view b32, std::string* hex) {  // 32 chars -> 20 bytes -> 40 hex
    if (b32.size() != 32) return false;
    uint8_t bytes[20] = {};
    uint64_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (char ch : b32) {
        const char c = Lower(ch);
        int v;
        if (c >= 'a' && c <= 'z') v = c - 'a';
        else if (c >= '2' && c <= '7') v = c - '2' + 26;
        else return false;
        acc = (acc << 5) | static_cast<uint64_t>(v);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            bytes[n++] = static_cast<uint8_t>(acc >> bits);
        }
    }
    static const char kHex[] = "0123456789abcdef";
    hex->clear();
    for (uint8_t b : bytes) {
        hex->push_back(kHex[b >> 4]);
        hex->push_back(kHex[b & 15]);
    }
    return n == 20;
}

inline bool ValidTracker(std::string_view t) {
    if (t.empty() || t.size() > proto::kMaxTrackerBytes) return false;
    if (!IStarts(t, "udp://") && !IStarts(t, "http://") && !IStarts(t, "https://")) return false;
    for (unsigned char c : t)
        if (c <= 0x20 || c == 0x7F || c == '"' || c == '\\' || c == '<' || c == '>') return false;
    return true;
}

inline bool ParseMagnet(std::string_view text, Magnet* out) {
    if (!IStarts(text, "magnet:?")) return false;
    Magnet m;
    std::string v;
    for (const auto& [k, raw] : SplitQuery(text.substr(8))) {
        const bool xt = IEquals(k, "xt") || (k.size() > 3 && IStarts(k, "xt."));
        if (xt) {
            if (!PercentDecode(raw, &v, false)) continue;
            if (IStarts(v, "urn:btih:") && (m.infohash.empty() || m.v2())) {
                const std::string_view h = std::string_view(v).substr(9);
                std::string hex;
                if (h.size() == 40) {
                    bool ok = true;
                    for (char c : h) ok = ok && IsHex(c);
                    if (!ok) continue;
                    for (char c : h) hex.push_back(Lower(c));
                } else if (!Base32ToHex(h, &hex)) {
                    continue;
                }
                m.infohash = hex;  // v1 wins over v2 for a hybrid link: every client can join the v1 swarm
            } else if (IStarts(v, "urn:btmh:") && m.infohash.empty()) {
                const std::string_view h = std::string_view(v).substr(9);
                if (h.size() != 68 || !IStarts(h, "1220")) continue;
                std::string hex;
                bool ok = true;
                for (char c : h.substr(4)) {
                    ok = ok && IsHex(c);
                    hex.push_back(Lower(c));
                }
                if (ok) m.infohash = hex;
            }
        } else if (IEquals(k, "dn")) {
            if (m.name.empty() && PercentDecode(raw, &v, true)) m.name = CleanName(v, proto::kMaxNameBytes);
        } else if (IEquals(k, "tr")) {
            if (m.trackers.size() >= proto::kMaxTrackers || !PercentDecode(raw, &v, false) || !ValidTracker(v)) continue;
            bool dup = false;
            for (const std::string& t : m.trackers) dup = dup || t == v;
            if (!dup) m.trackers.push_back(v);
        }
        // Everything else is dropped: ws, as, xs (HTTP fetches), x.pe (direct peers), so, kt, mt, ...
    }
    if (m.infohash.empty()) return false;
    *out = std::move(m);
    return true;
}

inline bool ParseYouTube(std::string_view text, YouTube* out) {
    if (!IStarts(text, "https://")) return false;
    std::string_view rest = text.substr(8);
    if (const size_t hash = rest.find('#'); hash != std::string_view::npos) rest = rest.substr(0, hash);
    const size_t slash = rest.find_first_of("/?");
    const std::string_view host = rest.substr(0, slash);
    std::string_view pathq = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    const bool shortHost = IEquals(host, "youtu.be");
    if (!shortHost && !IEquals(host, "youtube.com") && !IEquals(host, "www.youtube.com") && !IEquals(host, "m.youtube.com") &&
        !IEquals(host, "music.youtube.com"))
        return false;  // also refuses user info ("a@youtube.com") and ports ("youtube.com:443")
    const size_t q = pathq.find('?');
    const std::string_view path = pathq.substr(0, q);
    const std::string_view query = q == std::string_view::npos ? std::string_view{} : pathq.substr(q + 1);
    std::string v, list, dec;
    for (const auto& [k, raw] : SplitQuery(query)) {
        if (k == "v" && v.empty() && PercentDecode(raw, &dec, false)) v = dec;
        if (k == "list" && list.empty() && PercentDecode(raw, &dec, false)) list = dec;
    }
    YouTube y;
    auto seg = [&](std::string_view pre) -> std::string_view {
        std::string_view s = path.substr(pre.size());
        if (!s.empty() && s.back() == '/') s.remove_suffix(1);
        return s;
    };
    if (shortHost) {
        if (path.size() < 2) return false;
        y.id = std::string(seg("/"));
    } else if (path == "/watch" || path == "/watch/") {
        y.id = v;
        if (v.empty() && !ValidPlaylistId(list)) return false;
    } else if (path == "/playlist" || path == "/playlist/") {
        if (!ValidPlaylistId(list)) return false;
    } else if (IStarts(path, "/shorts/")) {
        y.id = std::string(seg("/shorts/"));
        y.shorts = true;
    } else if (IStarts(path, "/live/")) {
        y.id = std::string(seg("/live/"));
        y.live = true;
    } else {
        return false;
    }
    if (!y.id.empty() && !ValidVideoId(y.id)) return false;
    if (path.rfind("/playlist", 0) == 0) y.id.clear();
    if (ValidPlaylistId(list)) y.playlist = list;  // an invalid list next to a valid video is ignored
    if (y.id.empty() && y.playlist.empty()) return false;
    *out = std::move(y);
    return true;
}

// The whole rule: trims, refuses more than kMaxUrlBytes, and fills `out` (url = the canonical form). For a video
// link that also names a list, url is the video and yt.playlist the list (CanonicalPlaylistUrl builds that link).
inline bool Classify(std::string_view text, Classified* out) {
    text = Trim(text);
    if (text.empty() || text.size() > proto::kMaxUrlBytes) return false;
    Classified c;
    if (ParseYouTube(text, &c.yt)) {
        c.kind = Kind::YouTube;
        c.url = c.yt.id.empty() ? CanonicalPlaylistUrl(c.yt.playlist) : CanonicalVideoUrl(c.yt.id);
    } else if (ParseMagnet(text, &c.bt)) {
        c.kind = Kind::Magnet;
        c.url = CanonicalMagnet(&c.bt);
    } else {
        return false;
    }
    *out = std::move(c);
    return true;
}
}  // namespace melange::media::url
