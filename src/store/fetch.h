#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// One GET into a sink: https:// through WinHTTP (TLS 1.2+, system proxy, no cookies or credentials) or file:/// from
// disk. The caller decides which URLs are allowed (store/index.h ResolveUrl).
namespace melange::store::fetch {
struct Sink {
    virtual ~Sink() = default;
    // Called once, after the response headers and before the first Write: the body starts at byte `offset` of the
    // resource (0 unless a Range request was answered with 206) whose full length is `total` (0 when unknown). A sink
    // that kept bytes for a resume must drop them when `offset` is 0. False aborts the request.
    virtual bool Start(uint64_t offset, uint64_t total) {
        (void)offset;
        (void)total;
        return true;
    }
    virtual bool Write(const void* data, size_t n) = 0;
};
struct StringSink : Sink {
    std::string data;
    bool Write(const void* p, size_t n) override {
        data.append(static_cast<const char*>(p), n);
        return true;
    }
};
struct Options {
    uint64_t cap = 0;               // the body may not be longer
    uint32_t totalMs = 0;           // 0: no overall limit
    uint32_t stallMs = 30000;       // abort after this long without a byte
    const std::atomic<bool>* cancel = nullptr;
    std::function<void(uint64_t got, uint64_t total)> progress;   // total 0 when unknown
    std::string userAgent = "Melange";
    bool registerActive = true;     // false: CancelActive leaves this request alone (a background check beside the Store)
    // Resume: ask for the resource from this byte on ("Range: bytes=N-"). A 206 is accepted only when its Content-Range
    // starts at exactly N; a 200 (the server ignored the range) restarts at 0, which Sink::Start reports. `cap` and
    // `progress` count from the start of the resource. file:/// seeks.
    uint64_t resumeFrom = 0;
};
bool Get(const std::string& url, Sink& sink, const Options& o, std::string* err);

// One request with a method, headers and a body (Melange 0.13, wum.spotify's Web API calls).
struct HttpRequest {
    std::string method = "GET";
    std::vector<std::pair<std::string, std::string>> headers;  // no CR/LF; refused otherwise
    std::string body;
};
struct HttpResponse {
    int status = 0;
    std::string body;
    std::string retryAfter;   // the Retry-After header, or empty
    std::string contentType;
};
// https:// only (refuses file:// and anything else). Returns true when an HTTP response arrived (ANY status; the body
// is read for every status up to o.cap), false on a transport failure (err set). Redirects are NOT followed
// (WINHTTP_OPTION_REDIRECT_POLICY = NEVER), so an Authorization header never leaves the requested host. Same proxy,
// TLS 1.2/1.3, timeouts, no-cookies and no-auto-auth as Get. o.resumeFrom and Sink are not used.
bool Send(const std::string& url, const HttpRequest& req, HttpResponse* out, const Options& o, std::string* err);
void CancelActive();   // any thread: closes the request in flight so a blocked read returns at once
// "bytes 100-199/1000" (or ".../*"): first and last byte and the full length (0 for "*"). False when malformed.
bool ParseContentRange(std::string_view v, uint64_t* first, uint64_t* last, uint64_t* total);
}  // namespace melange::store::fetch
