#pragma once
// The torrent engine's pure rules, kept apart from libtorrent so src/bt/tests/bt_test.cpp checks them without a swarm:
// which tracker addresses are allowed (Critique S5), the piece math behind the stream's read-ahead window, the
// "holobt://<ih>/<index>" stream URL, file listing rules (S3) and the cache folder names the sweep may delete (T2).
// Header-only, no Windows or libtorrent headers.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "bt/bt.h"
#include "media/media_proto.h"
#include "media/url_logic.h"

namespace holo::bt::logic {
namespace url = melange::media::url;
namespace proto = melange::media::proto;

// ---------------------------------------------------------------- addresses (S5: no trackers on the LAN)
// True when an IPv4 address (network byte order, a[0] first) may be contacted as a tracker: not "this network",
// loopback, RFC 1918, link-local, CGNAT shared space, multicast, reserved or broadcast.
inline bool PublicV4(const uint8_t a[4]) {
    if (a[0] == 0 || a[0] == 10 || a[0] == 127) return false;
    if (a[0] == 169 && a[1] == 254) return false;
    if (a[0] == 172 && (a[1] & 0xF0) == 16) return false;
    if (a[0] == 192 && a[1] == 168) return false;
    if (a[0] == 100 && (a[1] & 0xC0) == 64) return false;  // 100.64.0.0/10
    if (a[0] == 192 && a[1] == 0 && a[2] == 0) return false;  // 192.0.0.0/24 protocol assignments
    if (a[0] == 198 && (a[1] & 0xFE) == 18) return false;     // 198.18.0.0/15 benchmarking
    if (a[0] >= 224) return false;                           // multicast, reserved, broadcast
    return true;
}
// The same for IPv6 (16 bytes): not unspecified, loopback, link-local, unique-local, site-local or multicast; a
// v4-mapped, v4-compatible or NAT64 address is judged by its IPv4 part.
inline bool PublicV6(const uint8_t a[16]) {
    bool zero10 = true;
    for (int i = 0; i < 10; ++i) zero10 = zero10 && a[i] == 0;
    if (zero10 && a[10] == 0xFF && a[11] == 0xFF) return PublicV4(a + 12);  // ::ffff:a.b.c.d
    if (zero10 && a[10] == 0 && a[11] == 0) {
        if (a[12] == 0 && a[13] == 0 && a[14] == 0 && a[15] <= 1) return false;  // :: and ::1
        return PublicV4(a + 12);                                                // ::a.b.c.d (deprecated form)
    }
    if (a[0] == 0x00 && a[1] == 0x64 && a[2] == 0xFF && a[3] == 0x9B) return PublicV4(a + 12);  // 64:ff9b::/96
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0x80) return false;  // fe80::/10 link-local
    if (a[0] == 0xFE && (a[1] & 0xC0) == 0xC0) return false;  // fec0::/10 site-local
    if ((a[0] & 0xFE) == 0xFC) return false;                  // fc00::/7 unique-local
    if (a[0] == 0xFF) return false;                           // multicast
    return true;
}

// Host and port of a canonical tracker URL (udp/http/https, as url::ValidTracker accepts it). False for user info,
// an empty or over-long host, a bad port, or a bracketed host that is not an IPv6 literal. The host comes back
// lowercase without brackets; port 0 = the scheme's default.
inline bool TrackerHost(std::string_view tr, std::string* host, int* port) {
    if (!url::ValidTracker(tr)) return false;
    const size_t sep = tr.find("://");
    std::string_view rest = tr.substr(sep + 3);
    const size_t end = rest.find_first_of("/?#");
    std::string_view auth = rest.substr(0, end);
    if (auth.find('@') != std::string_view::npos || auth.empty()) return false;
    std::string_view h, p;
    if (auth.front() == '[') {
        const size_t close = auth.find(']');
        if (close == std::string_view::npos || close < 3) return false;
        h = auth.substr(1, close - 1);
        for (char c : h)
            if (!(url::IsHex(c) || c == ':' || c == '.')) return false;
        if (h.find(':') == std::string_view::npos) return false;
        std::string_view after = auth.substr(close + 1);
        if (!after.empty()) {
            if (after.front() != ':') return false;
            p = after.substr(1);
            if (p.empty()) return false;
        }
    } else {
        const size_t colon = auth.find(':');
        h = auth.substr(0, colon);
        if (colon != std::string_view::npos) {
            p = auth.substr(colon + 1);
            if (p.empty()) return false;
        }
        for (char c : h)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.'))
                return false;  // no IDNA (allow_idna is off too), no '%' escapes, no '_' tricks
    }
    if (h.empty() || h.size() > 253) return false;
    int n = 0;
    if (!p.empty()) {
        if (p.size() > 5) return false;
        for (char c : p) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + (c - '0');
        }
        if (n < 1 || n > 65535) return false;
    }
    host->clear();
    for (char c : h) host->push_back(url::Lower(c));
    *port = n;
    return true;
}

// DNS names (not IP literals) refused without a lookup: local-only suffixes and single labels.
inline bool LocalHostName(std::string_view host) {
    auto ends = [&](std::string_view suf) {
        return host.size() >= suf.size() && host.substr(host.size() - suf.size()) == suf;
    };
    return host == "localhost" || ends(".localhost") || ends(".local") || ends(".lan") || ends(".home.arpa") ||
           ends(".internal") || host.find('.') == std::string_view::npos;  // a single label resolves on the LAN only
}

// ---------------------------------------------------------------- stream URL
// "holobt://<40|64 lowercase hex>/<0..kMaxTorrentFiles-1>", exactly as StreamUrl() builds it.
inline bool ParseStreamUrl(std::string_view u, std::string* ih, int* index) {
    const std::string pre = std::string(kStreamProtocol) + "://";
    if (u.substr(0, pre.size()) != pre) return false;
    u.remove_prefix(pre.size());
    const size_t slash = u.find('/');
    if (slash == std::string_view::npos) return false;
    const std::string_view h = u.substr(0, slash), i = u.substr(slash + 1);
    if (!url::ValidInfohash(h) || i.empty() || i.size() > 4 || (i.size() > 1 && i[0] == '0')) return false;
    int n = 0;
    for (char c : i) {
        if (c < '0' || c > '9') return false;
        n = n * 10 + (c - '0');
    }
    if (n >= static_cast<int>(proto::kMaxTorrentFiles)) return false;
    ih->assign(h);
    *index = n;
    return true;
}

// ---------------------------------------------------------------- files (S3)
// A metadata path as bt.files carries it: '/' separated, control characters removed, at most kMaxPathBytes on a
// UTF-8 boundary.
inline std::string CleanPath(std::string_view p) {
    std::string s;
    s.reserve(p.size());
    for (char c : p) s.push_back(c == '\\' ? '/' : c);
    return url::CleanName(s, proto::kMaxPathBytes);
}
// A file the picker may offer and the stream may open. Symlinks (BEP 47) and pad files never are.
inline bool PlayableFile(std::string_view path, bool symlink, bool pad) {
    return !symlink && !pad && url::IsVideoFile(path) && !url::IsRefusedFile(path);
}

// ---------------------------------------------------------------- piece math
// One file inside the torrent's piece space.
struct Span {
    int64_t offset = 0;    // the file's first byte in the torrent
    int64_t size = 0;      // bytes
    int64_t pieceLen = 0;  // bytes per piece (the last piece of the torrent may be shorter)
    int numPieces = 0;     // pieces in the torrent
    bool Valid() const { return offset >= 0 && size >= 0 && pieceLen > 0 && numPieces > 0; }
    // The piece holding byte `pos` of the file (pos clamped into the file).
    int PieceAt(int64_t pos) const {
        if (pos < 0) pos = 0;
        if (size > 0 && pos >= size) pos = size - 1;
        const int64_t p = (offset + pos) / pieceLen;
        return p >= numPieces ? numPieces - 1 : static_cast<int>(p);
    }
    int First() const { return PieceAt(0); }
    int Last() const { return PieceAt(size > 0 ? size - 1 : 0); }
    // Bytes from file position `pos` to the end of its piece, cut at the end of the file.
    int64_t ToPieceEnd(int64_t pos) const {
        if (pos < 0 || pos >= size) return 0;
        const int64_t abs = offset + pos;
        const int64_t end = (abs / pieceLen + 1) * pieceLen;
        const int64_t n = end - abs;
        return n < size - pos ? n : size - pos;
    }
};

inline constexpr int64_t kHeadBytes = 4ll << 20;        // the start of the file (container header, first frames)
inline constexpr int64_t kTailBytes = 2ll << 20;        // an MP4's moov or an MKV's cues at the end (design 1.1.4)
inline constexpr int64_t kMinWindowBytes = 16ll << 20;  // read-ahead before a rate is known, and its floor
inline constexpr int64_t kMaxWindowBytes = 192ll << 20;
inline constexpr double kWindowSeconds = 30;            // "about 30 s of bitrate"
inline constexpr int kMaxWindowPieces = 96;

// The engine never learns the duration, so the window is sized from a nominal bitrate: the file played over 90
// minutes. A 4 GB film gives about 740 KB/s (a 22 MB window); a short clip gets the 16 MB floor.
inline double NominalRate(int64_t fileSize) { return fileSize > 0 ? static_cast<double>(fileSize) / 5400.0 : 0; }

// Pieces in the read-ahead window for a playback rate in bytes per second (0 = unknown).
inline int WindowPieces(int64_t pieceLen, double rateBps) {
    if (pieceLen <= 0) return 1;
    double bytes = rateBps > 0 ? rateBps * kWindowSeconds : 0;
    if (bytes < static_cast<double>(kMinWindowBytes)) bytes = static_cast<double>(kMinWindowBytes);
    if (bytes > static_cast<double>(kMaxWindowBytes)) bytes = static_cast<double>(kMaxWindowBytes);
    int64_t n = (static_cast<int64_t>(bytes) + pieceLen - 1) / pieceLen;
    if (n < 2) n = 2;
    if (n > kMaxWindowPieces) n = kMaxWindowPieces;
    return static_cast<int>(n);
}
// The deadline (ms from now) of the k-th window piece: the first is due at once, the rest when playback would reach
// them at the measured rate, never closer together than 50 ms or further than 4 s apart.
inline int DeadlineMs(int k, int64_t pieceLen, double rateBps) {
    double step = rateBps > 0 ? static_cast<double>(pieceLen) / rateBps * 1000.0 : 500.0;
    if (step < 50) step = 50;
    if (step > 4000) step = 4000;
    const double ms = k * step;
    return ms > 600000 ? 600000 : static_cast<int>(ms);
}

// ---------------------------------------------------------------- cache folders (T2)
// The instance folder names core creates under media-cache: "<pid>-<rand>" (decimal pid, then 1..32 hex or
// alphanumeric characters). The sweep never touches anything else in that folder.
inline bool InstanceFolderName(std::wstring_view n) {
    const size_t dash = n.find(L'-');
    if (dash == std::wstring_view::npos || dash == 0 || dash > 10 || n.size() - dash - 1 < 1 || n.size() - dash - 1 > 32)
        return false;
    for (size_t i = 0; i < dash; ++i)
        if (n[i] < L'0' || n[i] > L'9') return false;
    for (size_t i = dash + 1; i < n.size(); ++i) {
        const wchar_t c = n[i];
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'))) return false;
    }
    return true;
}
// A torrent folder name under <cache>\bt: a valid infohash.
inline bool TorrentFolderName(std::wstring_view n) {
    if (n.size() != 40 && n.size() != 64) return false;
    std::string s;
    for (wchar_t c : n) {
        if (c > 0x7F) return false;
        s.push_back(static_cast<char>(c));
    }
    return url::ValidInfohash(s);
}
}  // namespace holo::bt::logic
