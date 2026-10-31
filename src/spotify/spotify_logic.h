#pragma once
// wum.spotify's pure logic (Melange 0.13.0): PKCE (base64url, SHA-256), the authorize URL, the loopback callback
// parser and its fixed page, the token and profile responses, the Web API calls each request makes, the error mapping,
// Retry-After and the rate gate, and the JSON each result gives Lua. Header-only and portable: no windows.h, only std,
// tools/json_read.h and tools/json_mini.h, so tests/spotify_selftest.cpp also builds with g++ on Linux. spotify.cpp
// (the module), wum_spotify.cpp (the binding) and token_store.cpp use it.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::spotify::logic {
// ---------------------------------------------------------------- encoding and hashing
// RFC 4648 section 5 (URL and filename safe alphabet), no padding.
inline std::string Base64Url(const uint8_t* p, size_t n) {
    static const char kT[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    s.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const uint32_t v = static_cast<uint32_t>(p[i]) << 16 | static_cast<uint32_t>(p[i + 1]) << 8 | p[i + 2];
        s += kT[v >> 18 & 63];
        s += kT[v >> 12 & 63];
        s += kT[v >> 6 & 63];
        s += kT[v & 63];
    }
    if (n - i == 1) {
        const uint32_t v = static_cast<uint32_t>(p[i]) << 16;
        s += kT[v >> 18 & 63];
        s += kT[v >> 12 & 63];
    } else if (n - i == 2) {
        const uint32_t v = static_cast<uint32_t>(p[i]) << 16 | static_cast<uint32_t>(p[i + 1]) << 8;
        s += kT[v >> 18 & 63];
        s += kT[v >> 12 & 63];
        s += kT[v >> 6 & 63];
    }
    return s;
}

// FIPS 180-4 SHA-256 (portable: the PKCE challenge, testable without BCrypt).
inline void Sha256(const void* data, size_t n, uint8_t out[32]) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    auto rotr = [](uint32_t x, int r) { return (x >> r) | (x << (32 - r)); };
    auto block = [&](const uint8_t* b) {
        uint32_t w[64];
        for (int t = 0; t < 16; ++t)
            w[t] = static_cast<uint32_t>(b[t * 4]) << 24 | static_cast<uint32_t>(b[t * 4 + 1]) << 16 |
                   static_cast<uint32_t>(b[t * 4 + 2]) << 8 | b[t * 4 + 3];
        for (int t = 16; t < 64; ++t) {
            const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
            const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
            w[t] = w[t - 16] + s0 + w[t - 7] + s1;
        }
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int t = 0; t < 64; ++t) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[t] + w[t];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
            const uint32_t t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = bb;
            bb = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += bb;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    };
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = n;
    while (left >= 64) {
        block(p);
        p += 64;
        left -= 64;
    }
    uint8_t tail[128] = {};
    if (left) std::memcpy(tail, p, left);
    tail[left] = 0x80;
    const size_t tlen = left + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(n) * 8;
    for (size_t i = 0; i < 8; ++i) tail[tlen - 1 - i] = static_cast<uint8_t>(bits >> (8 * i));
    block(tail);
    if (tlen == 128) block(tail + 64);
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
}

// RFC 7636: the verifier is the base64url of the caller's random bytes (48 bytes give 64 characters, within 43..128).
inline std::string PkceVerifier(const uint8_t* rnd, size_t n) { return Base64Url(rnd, n); }
inline std::string PkceChallenge(const std::string& verifier) {
    uint8_t d[32];
    Sha256(verifier.data(), verifier.size(), d);
    return Base64Url(d, sizeof d);
}

// RFC 3986: the unreserved characters are kept, every other byte is %XX (a space is %20, never '+').
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
inline std::string FormEncode(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string s;
    for (const auto& [k, v] : kv) {
        if (!s.empty()) s += '&';
        s += PercentEncode(k);
        s += '=';
        s += PercentEncode(v);
    }
    return s;
}

// The longest prefix of `s` of at most `n` bytes that does not cut a UTF-8 sequence.
inline std::string TrimUtf8(std::string_view s, size_t n) {
    if (s.size() <= n) return std::string(s);
    size_t i = n;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return std::string(s.substr(0, i));
}

// Constant-time equality (the OAuth state).
inline bool SecretEq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

// ---------------------------------------------------------------- the authorization request
inline std::string RedirectUri(int port) { return "http://127.0.0.1:" + std::to_string(port) + "/callback"; }
inline constexpr const char kScopes[] =
    "user-library-read playlist-read-private playlist-read-collaborative user-read-playback-state user-modify-playback-state "
    "user-read-currently-playing user-read-private";
inline constexpr const char kAuthorizePrefix[] = "https://accounts.spotify.com/authorize?";
inline constexpr const char kTokenUrl[] = "https://accounts.spotify.com/api/token";
inline constexpr const char kApiBase[] = "https://api.spotify.com/v1/";

inline std::string AuthorizeUrl(const std::string& clientId, const std::string& redirectUri, const std::string& state,
                                const std::string& challenge) {
    return kAuthorizePrefix + FormEncode({{"client_id", clientId},
                                          {"response_type", "code"},
                                          {"redirect_uri", redirectUri},
                                          {"code_challenge_method", "S256"},
                                          {"code_challenge", challenge},
                                          {"state", state},
                                          {"scope", kScopes}});
}
// The token endpoint's form bodies (application/x-www-form-urlencoded).
inline std::string TokenExchangeForm(const std::string& code, const std::string& redirectUri, const std::string& clientId,
                                     const std::string& verifier) {
    return FormEncode({{"grant_type", "authorization_code"},
                       {"code", code},
                       {"redirect_uri", redirectUri},
                       {"client_id", clientId},
                       {"code_verifier", verifier}});
}
inline std::string TokenRefreshForm(const std::string& refresh, const std::string& clientId) {
    return FormEncode({{"grant_type", "refresh_token"}, {"refresh_token", refresh}, {"client_id", clientId}});
}

// ---------------------------------------------------------------- ids and URIs
inline bool Alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
inline bool AllAlnum(std::string_view s) { return std::all_of(s.begin(), s.end(), Alnum); }
inline bool ValidClientId(std::string_view s) { return s.size() == 32 && AllAlnum(s); }  // ^[0-9A-Za-z]{32}$
inline bool ValidId(std::string_view s) { return s.size() == 22 && AllAlnum(s); }        // base62 Spotify id
enum class UriKind { None, Track, Playlist };
// Exactly "spotify:track:<id>" or "spotify:playlist:<id>"; anything else (albums, episodes, URLs) is None.
inline UriKind ClassifyUri(std::string_view s) {
    constexpr std::string_view kTrack = "spotify:track:", kPlaylist = "spotify:playlist:";
    if (s.substr(0, kTrack.size()) == kTrack && ValidId(s.substr(kTrack.size()))) return UriKind::Track;
    if (s.substr(0, kPlaylist.size()) == kPlaylist && ValidId(s.substr(kPlaylist.size()))) return UriKind::Playlist;
    return UriKind::None;
}
inline bool ValidDeviceId(std::string_view s) {  // ^[0-9A-Za-z_-]{1,128}$
    if (s.empty() || s.size() > 128) return false;
    for (char c : s)
        if (!Alnum(c) && c != '_' && c != '-') return false;
    return true;
}

// ---------------------------------------------------------------- the loopback callback
struct CallbackResult {
    enum Kind { NotOurs, Bad, Code, Denied, OtherError } kind = Bad;
    std::string code, error;
};

inline int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Query-component decoding: %XX and '+' as a space. False on a broken escape.
inline bool PercentDecode(std::string_view in, std::string* out) {
    out->clear();
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '%') {
            if (i + 2 >= in.size()) return false;
            const int hi = HexVal(in[i + 1]), lo = HexVal(in[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out->push_back(static_cast<char>(hi << 4 | lo));
            i += 2;
        } else if (c == '+') {
            out->push_back(' ');
        } else {
            out->push_back(c);
        }
    }
    return true;
}

inline bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}
inline std::string_view TrimSpace(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

inline constexpr size_t kMaxCallbackHead = 8192;

// One request to http://127.0.0.1:<port>/callback. `head` is the bytes up to and including the blank line.
//   NotOurs: another path (answer 404, keep waiting).   Bad: anything malformed, a wrong Host or state (400, keep waiting).
//   Denied: error=access_denied.   OtherError: another error.   Code: a code of 1..2048 printable bytes.
inline CallbackResult ParseCallbackRequest(std::string_view head, int port, std::string_view expectedState) {
    CallbackResult r;
    r.kind = CallbackResult::Bad;
    if (head.size() > kMaxCallbackHead) return r;
    const size_t end = head.find("\r\n\r\n");
    if (end == std::string_view::npos) return r;
    head = head.substr(0, end + 2);  // every line ends with \r\n
    const size_t le = head.find("\r\n");
    const std::string_view line = head.substr(0, le);
    const size_t s1 = line.find(' ');
    if (s1 == std::string_view::npos) return r;
    const size_t s2 = line.find(' ', s1 + 1);
    if (s2 == std::string_view::npos || line.find(' ', s2 + 1) != std::string_view::npos) return r;
    const std::string_view method = line.substr(0, s1), target = line.substr(s1 + 1, s2 - s1 - 1), version = line.substr(s2 + 1);
    if (method != "GET" || version.size() != 8 || version.substr(0, 7) != "HTTP/1." || version[7] < '0' || version[7] > '9') return r;
    if (target.empty() || target[0] != '/') return r;
    for (char c : target)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) return r;
    const size_t q = target.find('?');
    const std::string_view path = target.substr(0, q);
    std::string_view query = q == std::string_view::npos ? std::string_view() : target.substr(q + 1);
    if (path != "/callback") {
        r.kind = CallbackResult::NotOurs;
        return r;
    }
    // Headers: exactly one Host, 127.0.0.1:<port> (a DNS-rebinding page cannot name that).
    int hosts = 0;
    std::string_view host;
    size_t at = le + 2;
    while (at < head.size()) {
        const size_t e = head.find("\r\n", at);
        const std::string_view h = head.substr(at, e - at);
        at = e + 2;
        const size_t colon = h.find(':');
        if (colon == std::string_view::npos || colon == 0) return r;
        if (IEquals(h.substr(0, colon), "host")) {
            ++hosts;
            host = TrimSpace(h.substr(colon + 1));
        }
    }
    if (hosts != 1 || host != "127.0.0.1:" + std::to_string(port)) return r;
    // Query: state, code, error (each at most once).
    std::string state, code, error;
    bool haveState = false, haveCode = false, haveError = false;
    while (!query.empty()) {
        const size_t amp = query.find('&');
        const std::string_view part = query.substr(0, amp);
        query = amp == std::string_view::npos ? std::string_view() : query.substr(amp + 1);
        if (part.empty()) continue;
        const size_t eq = part.find('=');
        std::string k, v;
        if (!PercentDecode(part.substr(0, eq), &k)) return r;
        if (!PercentDecode(eq == std::string_view::npos ? std::string_view() : part.substr(eq + 1), &v)) return r;
        if (k == "state") {
            if (haveState) return r;
            haveState = true;
            state = std::move(v);
        } else if (k == "code") {
            if (haveCode) return r;
            haveCode = true;
            code = std::move(v);
        } else if (k == "error") {
            if (haveError) return r;
            haveError = true;
            error = std::move(v);
        }
    }
    if (!haveState || expectedState.empty() || !SecretEq(state, expectedState)) return r;
    if (haveError) {
        r.kind = error == "access_denied" ? CallbackResult::Denied : CallbackResult::OtherError;
        r.error = TrimUtf8(error, 64);
        return r;
    }
    if (!haveCode || code.empty() || code.size() > 2048) return r;
    for (char c : code)
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) return r;
    r.kind = CallbackResult::Code;
    r.code = std::move(code);
    return r;
}

// The whole HTTP/1.1 answer to the browser: a fixed page that never reflects any input.
inline std::string CallbackHttpResponse(int status, bool ok) {
    const char* reason = status == 200 ? "OK" : status == 404 ? "Not Found" : status == 400 ? "Bad Request" : "Error";
    std::string body =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>Melange</title></head>"
        "<body style=\"font-family:sans-serif;margin:3em\"><p>";
    if (status == 404) body += "Not found.";
    else if (ok) body += "Melange is linked to Spotify. You can close this tab and go back to the game.";
    else body += "Melange could not link Spotify. Go back to the game and try again.";
    body += "</p></body></html>";
    std::string s = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n";
    s += "Content-Type: text/html; charset=utf-8\r\n";
    s += "Cache-Control: no-store\r\n";
    s += "Content-Security-Policy: default-src 'none'\r\n";
    s += "Connection: close\r\n";
    s += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    s += body;
    return s;
}

// ---------------------------------------------------------------- JSON helpers
inline const json::Value* Member(const json::Value* v, std::string_view k) { return v ? v->Get(k) : nullptr; }
inline std::string StrOf(const json::Value* v) { return v && v->IsString() ? v->string : std::string(); }
inline bool IsTrue(const json::Value* v) { return v && v->IsBool() && v->boolean; }
inline bool IsFalse(const json::Value* v) { return v && v->IsBool() && !v->boolean; }
inline bool ParseObject(std::string_view body, json::Value* out) {
    json::Error e;
    return json::Parse(body, out, &e) && out->IsObject();
}

// ---------------------------------------------------------------- the token endpoint
struct Tokens {
    std::string access, refresh, scope;
    int expiresIn = 0;
};
// A 200 needs access_token, token_type "Bearer" and expires_in; refresh_token (optional) is a rotated token to store.
// An error body {"error": "invalid_grant" | "invalid_client" | ...} sets *oauthError. False for anything else.
inline bool ParseTokenResponse(std::string_view body, Tokens* t, std::string* oauthError) {
    if (oauthError) oauthError->clear();
    json::Value v;
    if (!ParseObject(body, &v)) return false;
    if (const json::Value* e = v.Get("error"); e && e->IsString()) {
        if (oauthError) *oauthError = TrimUtf8(e->string, 64);
        return false;
    }
    const json::Value* at = v.Get("access_token");
    const json::Value* tt = v.Get("token_type");
    const json::Value* ei = v.Get("expires_in");
    if (!at || !at->IsString() || at->string.empty() || at->string.size() > 4096 || !tt || !tt->IsString() ||
        !IEquals(tt->string, "bearer") || !ei || !ei->IsNumber() || !(ei->number > 0) || ei->number > 1e7)
        return false;
    t->access = at->string;
    t->expiresIn = static_cast<int>(ei->number);
    const json::Value* rt = v.Get("refresh_token");
    t->refresh = rt && rt->IsString() && rt->string.size() <= 4096 ? rt->string : std::string();
    t->scope = StrOf(v.Get("scope"));
    return true;
}
// Token expiry: the access token is refreshed when less than 60 s are left.
inline constexpr uint64_t kRefreshMarginMs = 60000;
inline uint64_t ExpiresAtMs(uint64_t nowMs, int expiresIn) { return nowMs + static_cast<uint64_t>(std::max(0, expiresIn)) * 1000; }
inline bool NeedsRefresh(uint64_t nowMs, uint64_t expiresAtMs, bool haveAccess) {
    return !haveAccess || expiresAtMs == 0 || nowMs + kRefreshMarginMs >= expiresAtMs;
}

// GET /v1/me. name = display_name, or the id when there is none, trimmed to 64 bytes.
struct Me {
    std::string id, name, product, country;
};
inline bool ParseMe(std::string_view body, Me* me) {
    json::Value v;
    if (!ParseObject(body, &v)) return false;
    const std::string id = StrOf(v.Get("id"));
    if (id.empty() || id.size() > 256) return false;
    me->id = id;
    std::string name = StrOf(v.Get("display_name"));
    if (name.empty()) name = id;
    me->name = TrimUtf8(name, 64);
    me->product = TrimUtf8(StrOf(v.Get("product")), 32);
    me->country = TrimUtf8(StrOf(v.Get("country")), 8);
    return true;
}

// ---------------------------------------------------------------- errors, Retry-After, the rate gate
enum class Err { None, Auth, Premium, NoDevice, Restricted, NotFound, BadRequest, Rate, Network, Timeout, Server, BadResponse, Cancelled };
inline const char* ErrName(Err e) {
    switch (e) {
        case Err::None: return "";
        case Err::Auth: return "auth";
        case Err::Premium: return "premium";
        case Err::NoDevice: return "no-device";
        case Err::Restricted: return "restricted";
        case Err::NotFound: return "not-found";
        case Err::BadRequest: return "bad-request";
        case Err::Rate: return "rate";
        case Err::Network: return "network";
        case Err::Timeout: return "timeout";
        case Err::Server: return "server";
        case Err::BadResponse: return "bad-response";
        case Err::Cancelled: return "cancelled";
    }
    return "";
}
// A Web API answer that is not 2xx. Reads Spotify's {"error": {"status", "message", "reason"}}; message <= 200 bytes.
inline Err MapApiError(int status, std::string_view body, std::string* message) {
    std::string reason, msg;
    json::Value v;
    if (ParseObject(body, &v)) {
        const json::Value* e = v.Get("error");
        if (e && e->IsObject()) {
            msg = StrOf(e->Get("message"));
            reason = StrOf(e->Get("reason"));
        } else if (e && e->IsString()) {
            msg = StrOf(v.Get("error_description"));
            if (msg.empty()) msg = e->string;
        }
    }
    if (message) *message = TrimUtf8(msg, 200);
    if (status >= 200 && status < 300) return Err::None;
    if (status == 401) return Err::Auth;
    if (status == 403) return reason == "PREMIUM_REQUIRED" ? Err::Premium : Err::Restricted;
    if (status == 404)
        return reason == "NO_ACTIVE_DEVICE" || msg.find("No active device") != std::string::npos ? Err::NoDevice : Err::NotFound;
    if (status == 400) return Err::BadRequest;
    if (status == 429) return Err::Rate;
    if (status >= 500 && status < 600) return Err::Server;
    return Err::Restricted;
}
// Retry-After in whole seconds: 5 when missing or not a number, clamped to 1..120.
inline int RetryAfterSeconds(std::string_view v) {
    v = TrimSpace(v);
    if (v.empty()) return 5;
    long long n = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return 5;
        if (n < 1000000) n = n * 10 + (c - '0');
    }
    return static_cast<int>(std::clamp<long long>(n, 1, 120));
}

// A token bucket of perMinute tokens refilled continuously, plus a pause (Retry-After). Pure: the caller gives the time.
class RateGate {
public:
    explicit RateGate(int perMinute) : cap_(std::max(1, perMinute)), tokens_(cap_) {}
    // Milliseconds until a call may go (0: now).
    uint64_t WaitMs(uint64_t nowMs) const {
        uint64_t wait = 0;
        if (nowMs < pausedUntil_) wait = pausedUntil_ - nowMs;
        const double t = Level(nowMs);
        if (t < 1.0) {
            const double need = (1.0 - t) / Rate();
            const uint64_t w = static_cast<uint64_t>(need) + (static_cast<double>(static_cast<uint64_t>(need)) < need ? 1 : 0);
            wait = std::max(wait, w);
        }
        return wait;
    }
    void Take(uint64_t nowMs) {
        tokens_ = Level(nowMs) - 1.0;
        if (nowMs > last_) last_ = nowMs;
    }
    void PauseUntil(uint64_t untilMs) { pausedUntil_ = std::max(pausedUntil_, untilMs); }
    void SetPerMinute(int perMinute) {
        cap_ = std::max(1, perMinute);
        tokens_ = std::min(tokens_, static_cast<double>(cap_));
    }

private:
    double Rate() const { return static_cast<double>(cap_) / 60000.0; }  // tokens per ms
    double Level(uint64_t nowMs) const {
        const double dt = nowMs > last_ ? static_cast<double>(nowMs - last_) : 0.0;
        return std::min(static_cast<double>(cap_), tokens_ + dt * Rate());
    }
    int cap_;
    double tokens_;
    uint64_t last_ = 0, pausedUntil_ = 0;
};

// What the worker does with one answer. 401: refresh once and retry once; 429: wait for Retry-After and retry inside
// the deadline; 5xx: retry once after 1 s; 2xx: shape it; anything else fails (MapApiError).
enum class Action { Shape, RefreshRetry, RateRetry, ServerRetry, Fail };
struct Attempts {
    bool refreshed = false, serverRetried = false;
};
inline Action Decide(int status, Attempts* a) {
    if (status >= 200 && status < 300) return Action::Shape;
    if (status == 401) {
        if (a->refreshed) return Action::Fail;
        a->refreshed = true;
        return Action::RefreshRetry;
    }
    if (status == 429) return Action::RateRetry;
    if (status >= 500 && status < 600) {
        if (a->serverRetried) return Action::Fail;
        a->serverRetried = true;
        return Action::ServerRetry;
    }
    return Action::Fail;
}

// ---------------------------------------------------------------- requests
enum class Op { Tracks, Playlists, PlaylistTracks, Devices, Play, Pause, Resume, Seek, Volume, Playback };
inline bool IsControl(Op op) { return op == Op::Play || op == Op::Pause || op == Op::Resume || op == Op::Seek || op == Op::Volume; }
struct Request {
    Op op = Op::Playback;
    int offset = 0, limit = 50;
    std::string id;                 // PlaylistTracks: the playlist id
    std::vector<std::string> uris;  // Play: track URIs
    std::string context;            // Play: a playlist URI
    int offsetIndex = -1;           // Play with context: 0-based position, or
    std::string offsetUri;          //   a track URI in it
    int64_t positionMs = -1;        // Play (>= 0 to set), Seek
    std::string deviceId;           // empty: the active device
    int volume = -1;                // Volume: 0..100
};

inline constexpr const char kPlaylistFields[] =
    "total,next,items(track(type,uri,name,duration_ms,is_playable,is_local,artists(name),album(name)))";

// The HTTP call for a request. False when the request is malformed (the binding validates first; this is the last check).
inline bool BuildCall(const Request& r, std::string* method, std::string* url, std::string* jsonBody) {
    method->clear();
    url->clear();
    jsonBody->clear();
    if (!r.deviceId.empty() && !ValidDeviceId(r.deviceId)) return false;
    const std::string base = kApiBase;
    auto page = [&]() { return r.limit >= 1 && r.limit <= 50 && r.offset >= 0; };
    auto device = [&](char sep) { return r.deviceId.empty() ? std::string() : std::string(1, sep) + "device_id=" + PercentEncode(r.deviceId); };
    switch (r.op) {
        case Op::Tracks:
            if (!page()) return false;
            *method = "GET";
            *url = base + "me/tracks?limit=" + std::to_string(r.limit) + "&offset=" + std::to_string(r.offset) + "&market=from_token";
            return true;
        case Op::Playlists:
            if (!page()) return false;
            *method = "GET";
            *url = base + "me/playlists?limit=" + std::to_string(r.limit) + "&offset=" + std::to_string(r.offset);
            return true;
        case Op::PlaylistTracks:
            if (!page() || !ValidId(r.id)) return false;
            *method = "GET";
            *url = base + "playlists/" + r.id + "/tracks?limit=" + std::to_string(r.limit) + "&offset=" + std::to_string(r.offset) +
                   "&market=from_token&fields=" + PercentEncode(kPlaylistFields);
            return true;
        case Op::Devices:
            *method = "GET";
            *url = base + "me/player/devices";
            return true;
        case Op::Play: {
            const bool haveUris = !r.uris.empty(), haveContext = !r.context.empty();
            if (haveUris == haveContext) return false;
            jsonmini::Obj o;
            if (haveUris) {
                if (r.uris.size() > 50 || r.offsetIndex >= 0 || !r.offsetUri.empty()) return false;
                jsonmini::Arr a;
                for (const std::string& u : r.uris) {
                    if (ClassifyUri(u) != UriKind::Track) return false;
                    a.Str(u);
                }
                o.Raw("uris", a.End());
            } else {
                if (ClassifyUri(r.context) != UriKind::Playlist) return false;
                if (r.offsetIndex >= 0 && !r.offsetUri.empty()) return false;
                o.Str("context_uri", r.context);
                if (r.offsetIndex >= 0) o.Raw("offset", jsonmini::Obj().Int("position", r.offsetIndex).End());
                if (!r.offsetUri.empty()) {
                    if (ClassifyUri(r.offsetUri) != UriKind::Track) return false;
                    o.Raw("offset", jsonmini::Obj().Str("uri", r.offsetUri).End());
                }
            }
            if (r.positionMs >= 0) o.Int("position_ms", r.positionMs);
            *method = "PUT";
            *url = base + "me/player/play" + device('?');
            *jsonBody = o.End();
            return true;
        }
        case Op::Resume:
            *method = "PUT";
            *url = base + "me/player/play" + device('?');
            return true;  // an empty body resumes
        case Op::Pause:
            *method = "PUT";
            *url = base + "me/player/pause" + device('?');
            return true;
        case Op::Seek:
            if (r.positionMs < 0) return false;
            *method = "PUT";
            *url = base + "me/player/seek?position_ms=" + std::to_string(r.positionMs) + device('&');
            return true;
        case Op::Volume:
            if (r.volume < 0 || r.volume > 100) return false;
            *method = "PUT";
            *url = base + "me/player/volume?volume_percent=" + std::to_string(r.volume) + device('&');
            return true;
        case Op::Playback:
            *method = "GET";
            *url = base + "me/player?market=from_token";
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- results for Lua (JSON object text)
inline constexpr size_t kMaxText = 200;  // name, artists, album, owner

inline std::string JoinArtists(const json::Value* artists) {
    std::string s;
    if (!artists || !artists->IsArray()) return s;
    for (const json::Value& a : artists->items) {
        const std::string n = StrOf(a.Get("name"));
        if (n.empty()) continue;
        if (!s.empty()) s += ", ";
        s += n;
        if (s.size() > kMaxText * 4) break;
    }
    return TrimUtf8(s, kMaxText);
}
inline long long IntOf(const json::Value* v, long long def = 0) {
    return v && v->IsNumber() && v->number >= -9e15 && v->number <= 9e15 ? static_cast<long long>(v->number) : def;
}

// Saved tracks (saved = true, kind "tracks") or a playlist's tracks (kind "playlist"): items[].track either way.
// Kept: type "track", not is_local, a spotify:track: URI. Everything else counts in "skipped".
inline std::string ShapeTracks(const json::Value& page, bool saved, int offset, int limit) {
    jsonmini::Arr items;
    long long skipped = 0;
    if (const json::Value* list = page.Get("items"); list && list->IsArray()) {
        for (const json::Value& it : list->items) {
            const json::Value* t = it.Get("track");
            const std::string uri = StrOf(Member(t, "uri"));
            if (!t || !t->IsObject() || StrOf(t->Get("type")) != "track" || IsTrue(t->Get("is_local")) || IsTrue(it.Get("is_local")) ||
                ClassifyUri(uri) != UriKind::Track) {
                ++skipped;
                continue;
            }
            jsonmini::Obj o;
            o.Str("uri", uri)
                .Str("name", TrimUtf8(StrOf(t->Get("name")), kMaxText))
                .Str("artists", JoinArtists(t->Get("artists")))
                .Str("album", TrimUtf8(StrOf(Member(t->Get("album"), "name")), kMaxText))
                .Int("durationMs", std::max(0LL, IntOf(t->Get("duration_ms"))))
                .Bool("playable", !IsFalse(t->Get("is_playable")));
            items.Raw(o.End());
        }
    }
    const json::Value* next = page.Get("next");
    return jsonmini::Obj()
        .Str("kind", saved ? "tracks" : "playlist")
        .Raw("items", items.End())
        .Int("offset", offset)
        .Int("limit", limit)
        .Int("total", std::max(0LL, IntOf(page.Get("total"))))
        .Bool("next", next && next->IsString() && !next->string.empty())
        .Int("skipped", skipped)
        .End();
}

inline std::string ShapePlaylists(const json::Value& page, int offset, int limit) {
    jsonmini::Arr items;
    long long skipped = 0;
    if (const json::Value* list = page.Get("items"); list && list->IsArray()) {
        for (const json::Value& p : list->items) {
            const std::string id = StrOf(p.Get("id"));
            if (!p.IsObject() || !ValidId(id)) {
                ++skipped;
                continue;
            }
            const json::Value* tracks = p.Get("tracks");
            if (!tracks || !tracks->IsObject()) tracks = p.Get("items");  // newer payloads
            std::string owner = StrOf(Member(p.Get("owner"), "display_name"));
            if (owner.empty()) owner = StrOf(Member(p.Get("owner"), "id"));
            jsonmini::Obj o;
            o.Str("id", id)
                .Str("uri", "spotify:playlist:" + id)
                .Str("name", TrimUtf8(StrOf(p.Get("name")), kMaxText))
                .Str("owner", TrimUtf8(owner, kMaxText))
                .Int("tracks", std::max(0LL, IntOf(Member(tracks, "total"))));
            items.Raw(o.End());
        }
    }
    const json::Value* next = page.Get("next");
    return jsonmini::Obj()
        .Str("kind", "playlists")
        .Raw("items", items.End())
        .Int("offset", offset)
        .Int("limit", limit)
        .Int("total", std::max(0LL, IntOf(page.Get("total"))))
        .Bool("next", next && next->IsString() && !next->string.empty())
        .Int("skipped", skipped)
        .End();
}

inline std::string ShapeDevices(const json::Value& body) {
    jsonmini::Arr items;
    if (const json::Value* list = body.Get("devices"); list && list->IsArray()) {
        for (const json::Value& d : list->items) {
            const json::Value* id = d.Get("id");
            if (!id || !id->IsString() || !ValidDeviceId(id->string)) continue;  // a null id cannot be addressed
            jsonmini::Obj o;
            o.Str("id", id->string)
                .Str("name", TrimUtf8(StrOf(d.Get("name")), kMaxText))
                .Str("type", TrimUtf8(StrOf(d.Get("type")), 32))
                .Bool("active", IsTrue(d.Get("is_active")))
                .Bool("restricted", IsTrue(d.Get("is_restricted")));
            if (const json::Value* vol = d.Get("volume_percent"); vol && vol->IsNumber()) o.Int("volume", IntOf(vol));
            items.Raw(o.End());
        }
    }
    return jsonmini::Obj().Raw("items", items.End()).End();
}

// GET /me/player: 204 (or no body) is {"active":false}. `at` is a placeholder the pump replaces with wum.peer.now() time.
inline std::string ShapePlayback(int status, const json::Value* body, double at) {
    if (status == 204 || !body || !body->IsObject()) return R"({"active":false})";
    jsonmini::Obj o;
    o.Bool("active", true).Bool("playing", IsTrue(body->Get("is_playing")));
    const json::Value* item = body->Get("item");
    if (item && item->IsObject() && StrOf(item->Get("type")) == "track") {
        const std::string uri = StrOf(item->Get("uri"));
        if (ClassifyUri(uri) == UriKind::Track) o.Str("uri", uri);
        o.Str("name", TrimUtf8(StrOf(item->Get("name")), kMaxText))
            .Str("artists", JoinArtists(item->Get("artists")))
            .Int("durationMs", std::max(0LL, IntOf(item->Get("duration_ms"))));
    }
    o.Int("positionMs", std::max(0LL, IntOf(body->Get("progress_ms")))).Float("at", at);
    if (const json::Value* d = body->Get("device"); d && d->IsObject()) {
        jsonmini::Obj dev;
        const std::string id = StrOf(d->Get("id"));
        if (ValidDeviceId(id)) dev.Str("id", id);
        dev.Str("name", TrimUtf8(StrOf(d->Get("name")), kMaxText)).Str("type", TrimUtf8(StrOf(d->Get("type")), 32));
        if (const json::Value* vol = d->Get("volume_percent"); vol && vol->IsNumber()) dev.Int("volume", IntOf(vol));
        o.Raw("device", dev.End());
    }
    const std::string ctx = StrOf(Member(body->Get("context"), "uri"));
    if (!ctx.empty() && ctx.size() <= 256) o.Str("context", ctx);
    return o.End();
}

inline std::string ShapeError(Err e, const std::string& message, int retryAfter) {
    jsonmini::Obj o;
    o.Str("error", ErrName(e)).Str("message", TrimUtf8(message, 200));
    if (e == Err::Rate) o.Int("retryAfter", retryAfter);
    return o.End();
}

// A 2xx answer for `r` as the callback's JSON; false (BadResponse) when a read answer is not the expected object.
inline bool ShapeSuccess(const Request& r, int status, std::string_view body, std::string* out) {
    if (IsControl(r.op)) {
        *out = R"({"ok":true})";
        return true;
    }
    if (r.op == Op::Playback && (status == 204 || body.empty())) {
        *out = ShapePlayback(204, nullptr, 0);
        return true;
    }
    json::Value v;
    if (!ParseObject(body, &v)) return false;
    switch (r.op) {
        case Op::Tracks: *out = ShapeTracks(v, true, r.offset, r.limit); return true;
        case Op::PlaylistTracks: *out = ShapeTracks(v, false, r.offset, r.limit); return true;
        case Op::Playlists: *out = ShapePlaylists(v, r.offset, r.limit); return true;
        case Op::Devices: *out = ShapeDevices(v); return true;
        case Op::Playback: *out = ShapePlayback(status, &v, 0); return true;
        default: break;
    }
    return false;
}

// ---------------------------------------------------------------- the stored sign-in (token_store.cpp encrypts it)
struct StoredTokens {
    std::string clientId, refresh, scope, userId, user, product, country;
    std::vector<std::string> grants;  // mod ids the user linked with their own button
};
inline std::string EncodeStored(const StoredTokens& s) {
    jsonmini::Arr g;
    for (const std::string& m : s.grants) g.Str(m);
    return jsonmini::Obj()
        .Int("v", 1)
        .Str("clientId", s.clientId)
        .Str("refresh", s.refresh)
        .Str("scope", s.scope)
        .Str("userId", s.userId)
        .Str("user", s.user)
        .Str("product", s.product)
        .Str("country", s.country)
        .Raw("grants", g.End())
        .End();
}
inline bool DecodeStored(std::string_view text, StoredTokens* s) {
    json::Value v;
    if (!ParseObject(text, &v)) return false;
    const json::Value* ver = v.Get("v");
    if (!ver || !ver->IsNumber() || ver->number != 1) return false;
    StoredTokens t;
    t.clientId = StrOf(v.Get("clientId"));
    t.refresh = StrOf(v.Get("refresh"));
    t.scope = StrOf(v.Get("scope"));
    t.userId = StrOf(v.Get("userId"));
    t.user = StrOf(v.Get("user"));
    t.product = StrOf(v.Get("product"));
    t.country = StrOf(v.Get("country"));
    if (const json::Value* g = v.Get("grants"); g && g->IsArray())
        for (const json::Value& m : g->items)
            if (m.IsString() && !m.string.empty() && m.string.size() <= 128 && t.grants.size() < 256) t.grants.push_back(m.string);
    if (!ValidClientId(t.clientId) || t.refresh.empty()) return false;
    *s = std::move(t);
    return true;
}
}  // namespace melange::spotify::logic
