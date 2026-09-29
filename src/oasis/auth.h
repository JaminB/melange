#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

// Oasis authentication: the launch token, the per-port session cookie, the Host/Origin checks and a
// visibility rate limit on repeated failures. melange.asi and oasis.exe both reach this through
// providers::MakeAuth() (providers.h); the pieces below are exposed only so tests/oasis_auth_selftest.cpp can
// drive them directly.
namespace melange::oasis::auth {
std::string Random128();                                  // 128 bits from the system RNG, base64url
bool SecretEq(std::string_view a, const std::string& b);  // constant-time
std::string CookieName(int port);  // "oasis_s_<port>": one cookie per Oasis process, since 127.0.0.1 has no
                                    // per-port cookie jar and two instances would otherwise overwrite each other

// A denied request's 1 s penalty (see FailureLimiter), bounded so a flood of denied requests cannot put every
// server thread to sleep at once: only a few callers actually wait, the rest return right away. Thread-safe.
void Delay();

// After kThreshold failures inside a rolling kWindowMs window, Note() starts returning true: the caller should
// delay its response by 1 s. Not a security control (the token's 128 bits of entropy are): this only keeps
// hammering visible in the log, at one warning line per window. `clock` is injectable for tests.
class FailureLimiter {
  public:
    using Clock = uint64_t (*)();
    explicit FailureLimiter(Clock clock = &DefaultClock) : clock_(clock) {}
    bool Note();  // thread-safe

    static constexpr uint64_t kWindowMs = 60000;
    static constexpr int kThreshold = 10;

  private:
    static uint64_t DefaultClock();
    Clock clock_;
    std::mutex mx_;
    uint64_t windowStart_ = 0, lastLog_ = 0;
    int count_ = 0;
};
}  // namespace melange::oasis::auth
