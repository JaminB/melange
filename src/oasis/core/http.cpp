#include "oasis/core/http.h"

#include <cstdio>
#include <cstdlib>

namespace melange::oasis::core {
namespace {
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

bool TokenChar(unsigned char c) {
    return c > 0x20 && c < 0x7f && c != '(' && c != ')' && c != '<' && c != '>' && c != '@' && c != ',' && c != ';' &&
           c != ':' && c != '\\' && c != '"' && c != '/' && c != '[' && c != ']' && c != '?' && c != '=' && c != '{' &&
           c != '}';
}
}  // namespace

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

std::string_view Request::Header(std::string_view name) const {
    for (const auto& [k, v] : headers)
        if (IEquals(k, name)) return v;
    return {};
}

size_t Request::HeaderCount(std::string_view name) const {
    size_t n = 0;
    for (const auto& h : headers) n += IEquals(h.first, name);
    return n;
}

std::string Request::Query(std::string_view key) const {
    std::string_view q = query;
    while (!q.empty()) {
        const size_t amp = q.find('&');
        const std::string_view kv = q.substr(0, amp);
        const size_t eq = kv.find('=');
        if (kv.substr(0, eq) == key) return std::string(eq == std::string_view::npos ? std::string_view{} : kv.substr(eq + 1));
        if (amp == std::string_view::npos) break;
        q.remove_prefix(amp + 1);
    }
    return {};
}

std::string Request::Cookie(std::string_view name) const {
    for (const auto& [k, v] : headers) {
        if (!IEquals(k, "cookie")) continue;
        std::string_view s = v;
        while (!s.empty()) {
            const size_t semi = s.find(';');
            const std::string_view kv = Trim(s.substr(0, semi));
            const size_t eq = kv.find('=');
            if (eq != std::string_view::npos && kv.substr(0, eq) == name) return std::string(kv.substr(eq + 1));
            if (semi == std::string_view::npos) break;
            s.remove_prefix(semi + 1);
        }
    }
    return {};
}

int Validate(const Request& rq) {
    if (rq.method != "GET" && rq.method != "HEAD") {
        for (unsigned char c : rq.method)
            if (!TokenChar(c)) return 400;
        return 405;
    }
    if (rq.target.empty() || rq.target[0] != '/') return 400;
    for (unsigned char c : rq.target)
        if (c <= 0x20 || c >= 0x7f) return 400;
    if (rq.headers.size() > 64) return 431;
    size_t bytes = rq.target.size();
    for (const auto& [k, v] : rq.headers) {
        if (k.empty()) return 400;
        for (unsigned char c : k)
            if (!TokenChar(c)) return 400;
        for (unsigned char c : v)
            if ((c < 0x20 && c != '\t') || c == 0x7f) return 400;
        bytes += k.size() + v.size() + 4;
    }
    if (bytes > 16 * 1024) return 431;
    if (rq.HeaderCount("host") != 1) return 400;
    if (rq.HeaderCount("transfer-encoding")) return 400;
    if (rq.HeaderCount("content-length") > 1) return 400;
    if (auto cl = rq.Header("content-length"); !cl.empty() && Trim(cl) != "0") return 400;
    return 0;
}

bool SafePath(std::string_view p) {
    if (p.empty() || p.size() > 256) return false;
    size_t seg = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            const std::string_view s = p.substr(seg, i - seg);
            if (s.empty() || s == "." || s == "..") return false;
            seg = i + 1;
            continue;
        }
        const char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
              c == '-' || c == '~'))
            return false;
    }
    return true;
}

const char* StatusText(int s) {
    switch (s) {
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 206: return "Partial Content";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 416: return "Range Not Satisfiable";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 503: return "Service Unavailable";
        default: return s < 400 ? "OK" : "Error";
    }
}

const char* MimeType(std::string_view p) {
    const size_t dot = p.rfind('.');
    const std::string_view e = dot == std::string_view::npos ? std::string_view{} : p.substr(dot + 1);
    if (IEquals(e, "html")) return "text/html; charset=utf-8";
    if (IEquals(e, "js") || IEquals(e, "mjs")) return "text/javascript; charset=utf-8";
    if (IEquals(e, "css")) return "text/css; charset=utf-8";
    if (IEquals(e, "json") || IEquals(e, "map")) return "application/json";
    if (IEquals(e, "svg")) return "image/svg+xml";
    if (IEquals(e, "png")) return "image/png";
    if (IEquals(e, "jpg") || IEquals(e, "jpeg")) return "image/jpeg";
    if (IEquals(e, "webp")) return "image/webp";
    if (IEquals(e, "ico")) return "image/x-icon";
    if (IEquals(e, "woff2")) return "font/woff2";
    if (IEquals(e, "txt") || IEquals(e, "md") || IEquals(e, "log")) return "text/plain; charset=utf-8";
    if (IEquals(e, "jsonl")) return "application/jsonl";
    if (IEquals(e, "wasm")) return "application/wasm";
    return "application/octet-stream";
}

namespace {
constexpr const char* kCsp =
    "Content-Security-Policy: default-src 'self'; connect-src 'self'; img-src 'self' blob: data:; "
    "style-src 'self' 'unsafe-inline'; frame-src 'self'; frame-ancestors 'none'; base-uri 'none'; "
    "form-action 'none'\r\n";
constexpr const char* kOther = "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
}  // namespace

const char* SecurityHeaders() {
    static const std::string all = std::string(kCsp) + kOther;
    return all.c_str();
}

std::string Head(const Response& r, uint64_t length, bool keepAlive) {
    std::string h;
    h.reserve(768);
    char line[96];
    snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", r.status, StatusText(r.status));
    h += line;
    bool ownCsp = false, ownCacheControl = false;
    for (const auto& kv : r.headers) {
        ownCsp |= IEquals(kv.first, "content-security-policy");
        ownCacheControl |= IEquals(kv.first, "cache-control");
    }
    if (!ownCsp) h += kCsp;  // a route with its own CSP (sandboxed panels) replaces the default
    h += kOther;
    // a route with its own Cache-Control (e.g. D's /erg/assets/, cached previews) replaces the default too
    if (!ownCacheControl) h += r.cacheable ? "Cache-Control: no-cache\r\n" : "Cache-Control: no-store\r\n";
    if (!r.contentType.empty() && r.status != 304) h += "Content-Type: " + r.contentType + "\r\n";
    for (const auto& [k, v] : r.headers) h += k + ": " + v + "\r\n";
    snprintf(line, sizeof line, "Content-Length: %llu\r\n", static_cast<unsigned long long>(length));
    h += line;
    h += keepAlive && !r.close ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n";
    return h;
}

bool ParseRange(std::string_view hv, uint64_t size, uint64_t* first, uint64_t* last) {
    hv = Trim(hv);
    if (hv.substr(0, 6) != "bytes=" || size == 0) return false;
    hv.remove_prefix(6);
    if (hv.find(',') != std::string_view::npos) return false;
    const size_t dash = hv.find('-');
    if (dash == std::string_view::npos) return false;
    auto num = [](std::string_view s, uint64_t* out) {
        if (s.empty() || s.size() > 19) return false;
        uint64_t v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<uint64_t>(c - '0');
        }
        *out = v;
        return true;
    };
    const std::string_view a = hv.substr(0, dash), b = hv.substr(dash + 1);
    uint64_t x = 0, y = 0;
    if (a.empty()) {
        if (!num(b, &y) || y == 0) return false;
        *first = y >= size ? 0 : size - y;
        *last = size - 1;
        return true;
    }
    if (!num(a, &x) || x >= size) return false;
    if (b.empty()) y = size - 1;
    else if (!num(b, &y) || y < x) return false;
    *first = x;
    *last = y >= size ? size - 1 : y;
    return true;
}
}  // namespace melange::oasis::core
