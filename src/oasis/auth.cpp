// Full authentication: launch token in the URL, exchanged for a per-port session cookie; Host allowlist on
// every request; Origin on the upgrade; a visibility rate limit on repeated failures. Replaces auth_min.cpp,
// the scaffold's minimal placeholder version.
#include "oasis/auth.h"

#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <string>

#include "core/log.h"
#include "oasis/core/http.h"
#include "oasis/providers.h"

namespace melange::oasis::auth {

std::string Random128() {
    unsigned char b[16];
    BCryptGenRandom(nullptr, b, sizeof b, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    for (size_t i = 0; i < sizeof b; i += 3) {
        const uint32_t v = static_cast<uint32_t>(b[i]) << 16 | (i + 1 < sizeof b ? b[i + 1] << 8 : 0) | (i + 2 < sizeof b ? b[i + 2] : 0);
        s += t[v >> 18 & 63];
        s += t[v >> 12 & 63];
        if (i + 1 < sizeof b) s += t[v >> 6 & 63];
        if (i + 2 < sizeof b) s += t[v & 63];
    }
    return s;
}

bool SecretEq(std::string_view a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

std::string CookieName(int port) { return "oasis_s_" + std::to_string(port); }

uint64_t FailureLimiter::DefaultClock() { return GetTickCount64(); }

bool FailureLimiter::Note() {
    std::lock_guard lk(mx_);
    const uint64_t now = clock_();
    if (now - windowStart_ >= kWindowMs) {
        windowStart_ = now;
        count_ = 0;
    }
    ++count_;
    const bool over = count_ > kThreshold;
    if (over && now - lastLog_ >= kWindowMs) {
        lastLog_ = now;
        LOG_WARN("[oasis] %d auth failure(s) in the last minute", count_);
    }
    return over;
}

namespace {
constexpr int kMaxConcurrentDelays = 2;
std::atomic<int> g_delaying{0};
}  // namespace

void Delay() {
    if (g_delaying.fetch_add(1, std::memory_order_acq_rel) < kMaxConcurrentDelays) Sleep(1000);
    g_delaying.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace melange::oasis::auth

namespace melange::oasis::providers {
namespace {
using auth::CookieName;
using auth::Random128;
using auth::SecretEq;

class AuthImpl final : public core::Auth {
  public:
    AuthImpl() : token_(Random128()), session_(Random128()) {}

    bool CheckHttp(const core::Request& rq, core::Response* out) override {
        if (!HostOk(rq)) return Deny(out);
        // A sandboxed panel iframe (no allow-same-origin) has an opaque origin, so its own requests never carry
        // the session cookie (the ancestor-chain same-site check fails). Its content is the same non-secret
        // bundle already inside melange.asi; only /ws and the RPCs it can reach stay behind the token/cookie.
        // The frame's bridge script, /app/ext.js, is loaded the same way.
        if (rq.path.starts_with("/ext/") || rq.path == "/app/ext.js") return true;
        const std::string k = rq.Query("k");
        if (!k.empty()) {
            if (!SecretEq(k, token_)) return Deny(out);
            if (rq.path != "/") return true;
            out->status = 303;
            out->body.clear();
            out->headers.emplace_back("Set-Cookie", CookieName(rq.port) + "=" + session_ + "; HttpOnly; SameSite=Strict; Path=/");
            out->headers.emplace_back("Location", "/");
            return false;
        }
        if (SecretEq(rq.Cookie(CookieName(rq.port)), session_)) return true;
        return Deny(out);
    }

    bool CheckUpgrade(const core::Request& rq) override {
        const std::string_view origin = rq.Header("origin");
        bool ok;
        if (origin.empty()) {
            ok = SecretEq(rq.Query("k"), token_);
        } else {
            const std::string p = std::to_string(rq.port);
            ok = core::IEquals(origin, "http://127.0.0.1:" + p) || core::IEquals(origin, "http://localhost:" + p);
        }
        if (!ok && limiter_.Note()) auth::Delay();
        return ok;
    }

    std::string LaunchUrl(int port) const override {
        return "http://127.0.0.1:" + std::to_string(port) + "/?k=" + token_;
    }

  private:
    static bool HostOk(const core::Request& rq) {
        const std::string_view h = rq.Header("host");
        const std::string p = std::to_string(rq.port);
        return core::IEquals(h, "127.0.0.1:" + p) || core::IEquals(h, "localhost:" + p);
    }
    bool Deny(core::Response* out) {
        core::NoteAuthFailure();
        if (limiter_.Note()) auth::Delay();
        out->status = 403;
        out->body = "Forbidden\n";
        out->close = true;
        return false;
    }
    std::string token_, session_;
    auth::FailureLimiter limiter_;
};
}  // namespace

core::Auth* MakeAuth() {
    static AuthImpl a;
    return &a;
}
}  // namespace melange::oasis::providers
