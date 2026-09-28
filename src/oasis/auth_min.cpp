// Minimal authentication: launch token in the URL, exchanged for a session cookie; Host allowlist on every
// request; Origin on the upgrade.
#include <windows.h>
#include <bcrypt.h>

#include <string>

#include "oasis/core/http.h"
#include "oasis/providers.h"

namespace melange::oasis::providers {
namespace {
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

class MinAuth final : public core::Auth {
  public:
    MinAuth() : token_(Random128()), session_(Random128()) {}

    bool CheckHttp(const core::Request& rq, core::Response* out) override {
        if (!HostOk(rq)) return Deny(out);
        const std::string k = rq.Query("k");
        if (!k.empty()) {
            if (!SecretEq(k, token_)) return Deny(out);
            if (rq.path != "/") return true;
            out->status = 303;
            out->body.clear();
            out->headers.emplace_back("Set-Cookie", "oasis_s=" + session_ + "; HttpOnly; SameSite=Strict; Path=/");
            out->headers.emplace_back("Location", "/");
            return false;
        }
        if (SecretEq(rq.Cookie("oasis_s"), session_)) return true;
        return Deny(out);
    }

    bool CheckUpgrade(const core::Request& rq) override {
        const std::string_view origin = rq.Header("origin");
        if (origin.empty()) return SecretEq(rq.Query("k"), token_);
        const std::string p = std::to_string(rq.port);
        return core::IEquals(origin, "http://127.0.0.1:" + p) || core::IEquals(origin, "http://localhost:" + p);
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
    static bool Deny(core::Response* out) {
        core::NoteAuthFailure();
        out->status = 403;
        out->body = "Forbidden\n";
        out->close = true;
        return false;
    }
    std::string token_, session_;
};
}  // namespace

core::Auth* MakeAuth() {
    static MinAuth a;
    return &a;
}
}  // namespace melange::oasis::providers
