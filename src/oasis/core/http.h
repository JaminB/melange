#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The request/response view the server core hands to Auth, Files and routes. Parsing and framing are civetweb's;
// Validate() adds the stricter rules of the protocol on top.
namespace melange::oasis::core {
struct Request {
    std::string method, target, path, query;  // target = path[?query], exactly as received (never percent-decoded)
    std::vector<std::pair<std::string, std::string>> headers;
    int port = 0;                              // the port the server is bound to
    bool upgrade = false;                      // "Upgrade: websocket" was asked for

    std::string_view Header(std::string_view name) const;  // first match, case-insensitive; "" if absent
    size_t HeaderCount(std::string_view name) const;
    std::string Query(std::string_view key) const;         // raw value of key=value in the query; "" if absent
    std::string Cookie(std::string_view name) const;       // from the Cookie header(s); "" if absent
};

struct Response {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;  // Set-Cookie, Location, ETag, Content-Encoding, ...
    bool cacheable = false;   // static app file: "Cache-Control: no-cache" instead of "no-store"
    bool close = false;       // close the connection after this response
    std::wstring file;        // when set, the body is streamed from this file (Range supported)
};

// 0 when the request may proceed, else the status to reject it with: 400 malformed, 405 method, 431 headers.
int Validate(const Request& rq);
// App paths: [A-Za-z0-9._~/-] only, no empty, "." or ".." segment. `path` has no leading '/'.
bool SafePath(std::string_view path);
const char* StatusText(int status);
const char* MimeType(std::string_view path);
bool IEquals(std::string_view a, std::string_view b);
// Headers every response carries: CSP, nosniff, Referrer-Policy, frame-ancestors. Never any Access-Control-*.
const char* SecurityHeaders();
// The status line and headers of `r` (no body), with Content-Length `length`.
std::string Head(const Response& r, uint64_t length, bool keepAlive);
// "bytes=a-b" against a resource of `size` bytes: false when unsatisfiable or malformed (single ranges only).
bool ParseRange(std::string_view header, uint64_t size, uint64_t* first, uint64_t* last);
}  // namespace melange::oasis::core
