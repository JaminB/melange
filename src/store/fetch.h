#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// One GET into a sink: https:// through WinHTTP (TLS 1.2+, system proxy, no cookies or credentials) or file:/// from
// disk. The caller decides which URLs are allowed (store/index.h ResolveUrl).
namespace melange::store::fetch {
struct Sink {
    virtual ~Sink() = default;
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
};
bool Get(const std::string& url, Sink& sink, const Options& o, std::string* err);
void CancelActive();   // any thread: closes the request in flight so a blocked read returns at once
}  // namespace melange::store::fetch
