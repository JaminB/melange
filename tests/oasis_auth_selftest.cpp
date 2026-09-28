// Offline self-test for Oasis authentication (no game, no live server needed). Exit code 0 = all passed.
// Drives melange::oasis::auth's pure pieces directly, and the production core::Auth from
// melange::oasis::providers::MakeAuth() with synthetic Request/Response values.
#include "oasis/auth.h"
#include "oasis/core/http.h"
#include "oasis/providers.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace core = melange::oasis::core;
namespace auth = melange::oasis::auth;
namespace providers = melange::oasis::providers;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const char* what, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    }
}

core::Request Req(int port, std::string target, std::vector<std::pair<std::string, std::string>> headers) {
    core::Request rq;
    rq.method = "GET";
    rq.target = std::move(target);
    const size_t q = rq.target.find('?');
    rq.path = rq.target.substr(0, q);
    rq.query = q == std::string::npos ? "" : rq.target.substr(q + 1);
    rq.headers = std::move(headers);
    rq.port = port;
    return rq;
}

std::string CookieHeaderValue(const core::Response& r) {
    for (const auto& [k, v] : r.headers)
        if (core::IEquals(k, "set-cookie")) return v;
    return {};
}

// "name=value; HttpOnly; ..." -> {"name", "value"}
std::pair<std::string, std::string> ParseSetCookie(const std::string& sc) {
    const size_t semi = sc.find(';');
    const std::string kv = sc.substr(0, semi);
    const size_t eq = kv.find('=');
    if (eq == std::string::npos) return {};
    return {kv.substr(0, eq), kv.substr(eq + 1)};
}

// ------------------------------------------------------------------ pure pieces (auth::)
void TestRandom128() {
    const std::string a = auth::Random128(), b = auth::Random128();
    Expect(a.size() == 22, "Random128 is 22 base64url chars", std::to_string(a.size()));
    for (char c : a)
        Expect((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_',
               "Random128 uses only base64url characters");
    Expect(a != b, "two calls to Random128 differ");
}

void TestSecretEq() {
    Expect(auth::SecretEq("", ""), "SecretEq: two empty strings are equal");
    Expect(auth::SecretEq("abc123", std::string("abc123")), "SecretEq: identical strings are equal");
    Expect(!auth::SecretEq("abc123", std::string("abc124")), "SecretEq: a one-character difference is unequal");
    Expect(!auth::SecretEq("abc12", std::string("abc123")), "SecretEq: different lengths are unequal");
}

void TestCookieName() {
    Expect(auth::CookieName(8765) == "oasis_s_8765", "CookieName includes the port", auth::CookieName(8765));
    Expect(auth::CookieName(8765) != auth::CookieName(8766), "CookieName differs across ports");
}

void TestFailureLimiter() {
    static uint64_t s_now;
    s_now = 0;
    auth::FailureLimiter lim([]() -> uint64_t { return s_now; });
    bool anyOverEarly = false;
    for (int i = 0; i < auth::FailureLimiter::kThreshold; ++i) anyOverEarly |= lim.Note();
    Expect(!anyOverEarly, "the first kThreshold failures in a window are not delayed");
    Expect(lim.Note(), "the failure past kThreshold is delayed");
    Expect(lim.Note(), "and so is the next one, still inside the window");
    s_now += auth::FailureLimiter::kWindowMs + 1;
    Expect(!lim.Note(), "a fresh window resets the count");
}

// ------------------------------------------------------------------ the production Auth (providers::MakeAuth)
std::string TokenFromLaunchUrl(const core::Auth& a, int port) {
    const std::string url = a.LaunchUrl(port);
    const size_t k = url.find("k=");
    return k == std::string::npos ? "" : url.substr(k + 2);
}

void TestAuthImpl() {
    core::Auth* a = providers::MakeAuth();
    Expect(a != nullptr, "MakeAuth returns an Auth");
    const int port = 8765;
    const std::string token = TokenFromLaunchUrl(*a, port);
    Expect(token.size() >= 16, "the launch URL carries a token", token);

    const std::vector<std::pair<std::string, std::string>> hostOk = {{"Host", "127.0.0.1:8765"}};
    const std::vector<std::pair<std::string, std::string>> hostBad = {{"Host", "evil.example:8765"}};

    // Forged Host: denied before anything else, even with a correct token.
    {
        core::Response r;
        Expect(!a->CheckHttp(Req(port, "/?k=" + token, hostBad), &r), "forged Host is denied");
        Expect(r.status == 403, "forged Host answers 403", std::to_string(r.status));
    }
    // No token, no cookie: denied.
    {
        core::Response r;
        Expect(!a->CheckHttp(Req(port, "/", hostOk), &r), "no token and no cookie is denied");
        Expect(r.status == 403, "missing auth answers 403", std::to_string(r.status));
    }
    // Wrong token: denied.
    {
        core::Response r;
        Expect(!a->CheckHttp(Req(port, "/?k=wrong", hostOk), &r), "a wrong token is denied");
    }
    // Correct token on "/": redirected, cookie set, token not echoed in the redirect.
    std::string cookieName, cookieValue;
    {
        core::Response r;
        const bool ok = a->CheckHttp(Req(port, "/?k=" + token, hostOk), &r);
        Expect(!ok, "a correct token on / does not itself grant the request (it redirects)");
        Expect(r.status == 303, "the token exchange answers 303", std::to_string(r.status));
        const std::string loc = [&] {
            for (const auto& [k, v] : r.headers)
                if (core::IEquals(k, "location")) return v;
            return std::string();
        }();
        Expect(loc == "/", "the redirect strips the token from the location", loc);
        const std::string sc = CookieHeaderValue(r);
        Expect(!sc.empty(), "the exchange sets a cookie");
        Expect(sc.find(token) == std::string::npos, "the cookie value is not the raw token");
        std::tie(cookieName, cookieValue) = ParseSetCookie(sc);
        Expect(cookieName == auth::CookieName(port), "the cookie is named per port", cookieName);
    }
    // The cookie alone now authenticates on this port.
    {
        core::Response r;
        Expect(a->CheckHttp(Req(port, "/", {{"Host", "127.0.0.1:8765"}, {"Cookie", cookieName + "=" + cookieValue}}), &r),
               "the session cookie authenticates a plain request");
    }
    // The same cookie value does not authenticate a different port (it is named for 8765, not 8766).
    {
        core::Response r;
        const auto req = Req(8766, "/", {{"Host", "127.0.0.1:8766"}, {"Cookie", cookieName + "=" + cookieValue}});
        Expect(!a->CheckHttp(req, &r), "a cookie from another port is rejected");
    }
    // A correct token also works on a non-root path (no redirect needed).
    {
        core::Response r;
        Expect(a->CheckHttp(Req(port, "/app/x.js?k=" + token, hostOk), &r), "a correct token authenticates a non-root path directly");
    }

    // CheckUpgrade: absent Origin needs a valid token.
    Expect(!a->CheckUpgrade(Req(port, "/ws?k=wrong", hostOk)), "upgrade with no Origin and a wrong token is denied");
    Expect(a->CheckUpgrade(Req(port, "/ws?k=" + token, hostOk)), "upgrade with no Origin and the right token is allowed");
    // Origin must match this port.
    auto withOrigin = [&](std::string origin) {
        auto rq = Req(port, "/ws", hostOk);
        rq.headers.emplace_back("Origin", std::move(origin));
        return rq;
    };
    Expect(!a->CheckUpgrade(withOrigin("http://evil.example")), "upgrade from a foreign Origin is denied");
    Expect(!a->CheckUpgrade(withOrigin("http://127.0.0.1:9999")), "upgrade from the right host, wrong port is denied");
    Expect(a->CheckUpgrade(withOrigin("http://127.0.0.1:8765")), "upgrade from this server's own Origin is allowed");
    Expect(a->CheckUpgrade(withOrigin("http://localhost:8765")), "localhost is accepted as an alias for 127.0.0.1");
}
}  // namespace

int main() {
    TestRandom128();
    TestSecretEq();
    TestCookieName();
    TestFailureLimiter();
    TestAuthImpl();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
