// Offline self-test for src/spotify/spotify_logic.h (wum.spotify's pure logic; no game, no network). Portable: it also
// builds with g++ on Linux (json_read.cpp needs a _wfopen stand-in there). Exit code 0 = all passed. Melange 0.14 adds the
// callback pages, the Client ID resolution, file and clipboard finder, the not-allowlisted heuristic and transfer.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "spotify/spotify_logic.h"
#include "store/fetch.h"

using namespace melange::spotify::logic;
namespace json = melange::json;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}
void ExpectEq(const std::string& got, const std::string& want, const std::string& what) {
    if (got != want) printf("  %s:\n    got  %s\n    want %s\n", what.c_str(), got.c_str(), want.c_str());
    Expect(got == want, what);
}
bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::string B64(const std::string& s) { return Base64Url(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
std::string Hex(const uint8_t* p, size_t n) {
    static const char k[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += k[p[i] >> 4];
        s += k[p[i] & 15];
    }
    return s;
}
std::string ShaHex(const std::string& s) {
    uint8_t d[32];
    Sha256(s.data(), s.size(), d);
    return Hex(d, 32);
}
json::Value J(const std::string& text) {
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e)) printf("  bad test JSON at %d:%d: %s\n", e.line, e.col, e.text.c_str());
    return v;
}

const char kState[] = "c3RhdGUtc3RhdGUtc3RhdGU";
std::string Head(const std::string& target, const std::string& host = "127.0.0.1:8780", const std::string& method = "GET") {
    return method + " " + target + " HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: test\r\nAccept: text/html\r\n\r\n";
}
CallbackResult Cb(const std::string& head) { return ParseCallbackRequest(head, 8780, kState); }

void TestEncoding() {
    ExpectEq(B64(""), "", "base64url ''");
    ExpectEq(B64("f"), "Zg", "base64url f");
    ExpectEq(B64("fo"), "Zm8", "base64url fo");
    ExpectEq(B64("foo"), "Zm9v", "base64url foo");
    ExpectEq(B64("foob"), "Zm9vYg", "base64url foob");
    ExpectEq(B64("fooba"), "Zm9vYmE", "base64url fooba");
    ExpectEq(B64("foobar"), "Zm9vYmFy", "base64url foobar");
    const uint8_t fb[] = {0xfb, 0xff};
    ExpectEq(Base64Url(fb, 2), "-_8", "base64url uses - and _");

    ExpectEq(ShaHex("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");
    ExpectEq(ShaHex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "sha256 448-bit vector");
    ExpectEq(ShaHex(""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    ExpectEq(ShaHex(std::string(1000, 'a')), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3", "sha256 1000 x a");

    ExpectEq(PkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk"), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
             "RFC 7636 appendix B challenge");
    uint8_t rnd[48];
    for (int i = 0; i < 48; ++i) rnd[i] = static_cast<uint8_t>(i * 37 + 11);
    const std::string v = PkceVerifier(rnd, sizeof rnd);
    Expect(v.size() == 64 && v.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos,
           "48 random bytes give a 64-character verifier");

    ExpectEq(PercentEncode("aZ09-._~"), "aZ09-._~", "percent-encode keeps the unreserved characters");
    ExpectEq(PercentEncode("a b&c=d/e?\xC3\xA9+"), "a%20b%26c%3Dd%2Fe%3F%C3%A9%2B", "percent-encode: space is %20, UTF-8 bytes");
    ExpectEq(FormEncode({{"grant_type", "refresh_token"}, {"refresh_token", "a/b+c"}, {"k y", ""}}),
             "grant_type=refresh_token&refresh_token=a%2Fb%2Bc&k%20y=", "form-encode");
    ExpectEq(RedirectUri(8780), "http://127.0.0.1:8780/callback", "redirect URI");
    const std::string url = AuthorizeUrl("0123456789abcdef0123456789ABCDEF", RedirectUri(8780), "st", "ch");
    Expect(url.rfind("https://accounts.spotify.com/authorize?", 0) == 0, "authorize URL prefix");
    Expect(Has(url, "client_id=0123456789abcdef0123456789ABCDEF") && Has(url, "response_type=code") &&
               Has(url, "redirect_uri=http%3A%2F%2F127.0.0.1%3A8780%2Fcallback") && Has(url, "code_challenge_method=S256") &&
               Has(url, "code_challenge=ch") && Has(url, "&state=st"),
           "authorize URL parameters");
    Expect(Has(url, "scope=user-library-read%20playlist-read-private%20playlist-read-collaborative%20user-read-playback-state%20"
                    "user-modify-playback-state%20user-read-currently-playing%20user-read-private"),
           "authorize URL scopes with %20");
    ExpectEq(TokenExchangeForm("c/d", RedirectUri(8780), "id", "ver"),
             "grant_type=authorization_code&code=c%2Fd&redirect_uri=http%3A%2F%2F127.0.0.1%3A8780%2Fcallback&client_id=id&code_verifier=ver",
             "the code exchange form");
    ExpectEq(TokenRefreshForm("r", "id"), "grant_type=refresh_token&refresh_token=r&client_id=id", "the refresh form");
    ExpectEq(TrimUtf8("ab\xC3\xA9", 3), "ab", "trim does not cut a UTF-8 sequence");
    ExpectEq(TrimUtf8("ab\xC3\xA9", 4), "ab\xC3\xA9", "trim keeps a whole sequence");
}

void TestIds() {
    Expect(ValidClientId("0123456789abcdef0123456789ABCDEF"), "client id: 32 alphanumerics");
    Expect(!ValidClientId("0123456789abcdef0123456789ABCDE"), "client id: 31");
    Expect(!ValidClientId("0123456789abcdef0123456789ABCDE-"), "client id: a dash");
    Expect(ValidId("4uLU6hMCjMI75M1A2tKUQC"), "id: 22 base62");
    Expect(!ValidId("4uLU6hMCjMI75M1A2tKUQ"), "id: 21");
    Expect(ClassifyUri("spotify:track:4uLU6hMCjMI75M1A2tKUQC") == UriKind::Track, "uri: track");
    Expect(ClassifyUri("spotify:playlist:37i9dQZF1DXcBWIGoYBM5M") == UriKind::Playlist, "uri: playlist");
    Expect(ClassifyUri("spotify:album:4uLU6hMCjMI75M1A2tKUQC") == UriKind::None, "uri: album is None");
    Expect(ClassifyUri("spotify:track:4uLU6hMCjMI75M1A2tKUQ") == UriKind::None, "uri: a 21-character id");
    Expect(ClassifyUri("spotify:track:x/../") == UriKind::None, "uri: path tricks");
    Expect(ClassifyUri("spotify:track:4uLU6hMCjMI75M1A2tKUQC ") == UriKind::None, "uri: trailing space");
    Expect(ClassifyUri("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC") == UriKind::None, "uri: a URL is None");
    Expect(ValidDeviceId("abc_DEF-123") && ValidDeviceId(std::string(128, 'a')), "device id: allowed characters and length");
    Expect(!ValidDeviceId("") && !ValidDeviceId(std::string(129, 'a')) && !ValidDeviceId("a b") && !ValidDeviceId("a&b"),
           "device id: empty, too long, space, &");
}

void TestCallback() {
    CallbackResult r = Cb(Head(std::string("/callback?code=AQB-x_y.z~&state=") + kState));
    Expect(r.kind == CallbackResult::Code && r.code == "AQB-x_y.z~", "callback: a good request");
    Expect(Cb(Head("/favicon.ico")).kind == CallbackResult::NotOurs, "callback: /favicon.ico is not ours");
    Expect(Cb(Head(std::string("/callback/x?code=a&state=") + kState)).kind == CallbackResult::NotOurs, "callback: a longer path");
    Expect(Cb(Head(std::string("/callback?code=a&state=") + kState, "localhost:8780")).kind == CallbackResult::Bad,
           "callback: Host localhost is refused");
    Expect(Cb(Head(std::string("/callback?code=a&state=") + kState, "127.0.0.1:8781")).kind == CallbackResult::Bad,
           "callback: the wrong port");
    Expect(Cb(Head("/callback?code=a&state=c3RhdGUtc3RhdGUtc3RhdGX")).kind == CallbackResult::Bad, "callback: a state mismatch");
    Expect(Cb(Head("/callback?code=a")).kind == CallbackResult::Bad, "callback: no state");
    r = Cb(Head(std::string("/callback?error=access_denied&state=") + kState));
    Expect(r.kind == CallbackResult::Denied, "callback: access_denied");
    r = Cb(Head(std::string("/callback?error=server_error&state=") + kState));
    Expect(r.kind == CallbackResult::OtherError && r.error == "server_error", "callback: another error");
    Expect(Cb(Head("/callback?error=access_denied&state=wrong")).kind == CallbackResult::Bad,
           "callback: a denial with the wrong state is not ours to act on");
    Expect(Cb(Head(std::string("/callback?state=") + kState)).kind == CallbackResult::Bad, "callback: a missing code");
    Expect(Cb(Head(std::string("/callback?code=&state=") + kState)).kind == CallbackResult::Bad, "callback: an empty code");
    Expect(Cb(Head(std::string("/callback?code=") + std::string(2049, 'a') + "&state=" + kState)).kind == CallbackResult::Bad,
           "callback: a code over 2048 bytes");
    Expect(Cb(Head(std::string("/callback?code=") + std::string(2048, 'a') + "&state=" + kState)).kind == CallbackResult::Code,
           "callback: a 2048-byte code");
    std::string big = Head(std::string("/callback?code=a&state=") + kState);
    big.insert(big.size() - 2, "X-Pad: " + std::string(9000, 'p') + "\r\n");
    Expect(Cb(big).kind == CallbackResult::Bad, "callback: a 9000-byte head");
    Expect(Cb(Head(std::string("/callback?code=a&state=") + kState, "127.0.0.1:8780", "POST")).kind == CallbackResult::Bad,
           "callback: POST");
    r = Cb(Head(std::string("/callback?code=AQ%2Bb%2F%3D&state=") + kState));
    Expect(r.kind == CallbackResult::Code && r.code == "AQ+b/=", "callback: a percent-encoded code is decoded");
    Expect(Cb(Head(std::string("/callback?code=a%2&state=") + kState)).kind == CallbackResult::Bad, "callback: a broken escape");
    Expect(Cb(Head(std::string("/callback?code=a&code=b&state=") + kState)).kind == CallbackResult::Bad, "callback: a repeated code");
    Expect(Cb(std::string("GET /callback?code=a&state=") + kState + " HTTP/1.1\r\nHost: 127.0.0.1:8780\r\n").kind == CallbackResult::Bad,
           "callback: no blank line");
    Expect(Cb(std::string("GET /callback?code=a&state=") + kState + " HTTP/1.1\r\nhOsT:127.0.0.1:8780\r\n\r\n").kind ==
               CallbackResult::Code,
           "callback: the Host name is case-insensitive");
    Expect(Cb(std::string("GET /callback?code=a&state=") + kState + " HTTP/1.1\r\nHost: 127.0.0.1:8780\r\nHost: 127.0.0.1:8780\r\n\r\n")
                   .kind == CallbackResult::Bad,
           "callback: two Host headers");
    Expect(ParseCallbackRequest(Head("/callback?code=a&state="), 8780, "").kind == CallbackResult::Bad, "callback: no expected state");

    const std::string nf = CallbackHttpResponse(404, Page::Ok), br = CallbackHttpResponse(400, Page::Failed);
    Expect(nf.rfind("HTTP/1.1 404 Not Found\r\n", 0) == 0 && Has(nf, "<p>Not found.</p>") && br.rfind("HTTP/1.1 400 Bad Request\r\n", 0) == 0 &&
               Has(br, "<p>Bad request.</p>"),
           "callback page: 404 and 400 keep their minimal bodies");
    Expect(CallbackHttpResponse(404, Page::Failed) == nf && CallbackHttpResponse(400, Page::Ok) == br, "callback page: 404/400 ignore the page");
}

// Melange 0.14: one fixed, styled, self-contained page per outcome, with strict headers.
void TestPages() {
    struct P {
        Page page;
        const char* title;
        const char* text;
        bool ok;
    };
    const P pages[] = {
        {Page::Ok, "Signed in to Spotify", "Go back to the game: your music is ready.", true},
        {Page::Denied, "Sign-in cancelled", "You did not allow access. Go back to the game to try again.", false},
        {Page::Cancelled, "Sign-in cancelled", "The sign-in was cancelled in the game. Go back to the game to try again.", false},
        {Page::NotAllowed, "This Spotify app is not open to your account",
         "The app only works for accounts its owner added. Go back to the game to set up your own free Spotify app.", false},
        {Page::Failed, "Could not sign in", "Spotify's answer did not work out. Go back to the game and try again.", false},
    };
    std::vector<std::string> all;
    for (const P& p : pages) {
        const std::string r = CallbackHttpResponse(200, p.page);
        const std::string name = std::string("page ") + p.title;
        const size_t split = r.find("\r\n\r\n");
        const std::string head = r.substr(0, split + 2), body = split == std::string::npos ? "" : r.substr(split + 4);
        Expect(r.rfind("HTTP/1.1 200 OK\r\n", 0) == 0, name + ": 200");
        Expect(Has(head, "Content-Type: text/html; charset=utf-8\r\n") && Has(head, "Cache-Control: no-store\r\n") &&
                   Has(head,
                       "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; "
                       "frame-ancestors 'none'\r\n") &&
                   Has(head, "X-Content-Type-Options: nosniff\r\n") && Has(head, "Referrer-Policy: no-referrer\r\n") &&
                   Has(head, "Connection: close\r\n"),
               name + ": headers (CSP, nosniff, no-referrer)");
        const size_t cl = head.find("Content-Length: ");
        Expect(cl != std::string::npos && std::stoul(head.substr(cl + 16)) == body.size(), name + ": Content-Length is the body's size");
        Expect(Has(body, std::string("<h1>") + p.title + "</h1>") && Has(body, std::string("<p>") + p.text + "</p>") &&
                   Has(body, "<p class=\"small\">You can close this tab.</p>") && Has(body, "<title>Melange - Spotify</title>"),
               name + ": title and text");
        Expect(Has(body, p.ok ? "<div class=\"mark\">" : "<div class=\"mark bad\">") && Has(body, p.ok ? "M5 12.5l4.5 4.5L19 7.5" : "M12 6v8"),
               name + ": the mark");
        Expect(!Has(body, "<script") && !Has(body, "http://") && !Has(body, "https://") && !Has(body, "<a ") && !Has(body, "<link") &&
                   !Has(body, "<img"),
               name + ": no script, no external URL, no link");
        Expect(body.rfind("<!DOCTYPE html><html lang=\"en\">", 0) == 0 && body.size() > 500 && body.size() < 4000, name + ": a whole document");
        Expect(!Has(r, "code=") && !Has(r, kState), name + ": reflects no query text");
        all.push_back(r);
    }
    for (size_t i = 0; i < all.size(); ++i)
        for (size_t j = i + 1; j < all.size(); ++j) Expect(all[i] != all[j], "pages differ " + std::to_string(i) + "/" + std::to_string(j));
    Expect(CallbackHttpResponse(200, Page::Ok) == all[0], "the page is the same whatever was requested (no input at all)");
    Expect(sizeof kBuiltinPorts / sizeof kBuiltinPorts[0] == 3 && kBuiltinPorts[0] == 8780 && kBuiltinPorts[1] == 8781 &&
               kBuiltinPorts[2] == 8782,
           "built-in app ports 8780, 8781, 8782");
    ExpectEq(kDashboardUrl, "https://developer.spotify.com/dashboard/create", "the dashboard URL");
}

// Melange 0.14: which Client ID is used, its hint, and the in-game file.
void TestSetup() {
    const std::string a(32, 'a'), b = "0123456789ABCDEFabcdef0123456789", c = "ZZZZZZZZZZzzzzzzzzzz000000000000";
    ResolvedId r = ResolveClientId(a, b, c);
    Expect(r.id == a && r.app == "user", "resolve: a valid user ID wins");
    r = ResolveClientId("short", b, c);
    Expect(r.id == b && r.app == "ini", "resolve: an invalid user ID -> ini");
    r = ResolveClientId("", "0123456789ABCDEFabcdef012345678!", c);
    Expect(r.id == c && r.app == "builtin", "resolve: an invalid ini ID -> builtin");
    r = ResolveClientId("", "", c);
    Expect(r.id == c && r.app == "builtin", "resolve: only the built-in ID");
    r = ResolveClientId("", "", "");
    Expect(r.id.empty() && r.app.empty(), "resolve: none -> app \"\"");
    r = ResolveClientId(std::string(33, 'a'), std::string(31, 'b'), "not valid");
    Expect(r.id.empty() && r.app.empty(), "resolve: all invalid -> none");
    ExpectEq(IdHint(b), "6789", "hint: the last 4 characters");
    ExpectEq(IdHint(""), "", "hint: empty");

    const std::string file = EncodeClientFile(b);
    ExpectEq(file, R"({"v":1,"clientId":"0123456789ABCDEFabcdef0123456789"})", "client.json encoding");
    std::string back;
    Expect(DecodeClientFile(file, &back) && back == b, "client.json round trip");
    back = "keep";
    Expect(!DecodeClientFile(R"({"v":2,"clientId":"0123456789ABCDEFabcdef0123456789"})", &back) && back == "keep", "client.json: v 2 rejected");
    Expect(!DecodeClientFile(R"({"v":1,"clientId":"0123456789ABCDEFabcdef012345678"})", &back), "client.json: 31 characters rejected");
    Expect(!DecodeClientFile(R"({"v":1})", &back) && !DecodeClientFile(R"({"clientId":"0123456789ABCDEFabcdef0123456789"})", &back),
           "client.json: a missing field is rejected");
    Expect(!DecodeClientFile("", &back) && !DecodeClientFile("[1]", &back) && !DecodeClientFile(R"({"v":1,"clientId":5})", &back),
           "client.json: garbage rejected");
}

void TestFindClientId() {
    const std::string id = "0123456789ABCDEFabcdef0123456789", id2 = "ffffffffffffffffffffffffffffffff";
    ExpectEq(FindClientId(id), id, "find: exact");
    ExpectEq(FindClientId("  " + id + "\r\n"), id, "find: surrounding whitespace");
    ExpectEq(FindClientId("Client ID: " + id), id, "find: a labelled line");
    ExpectEq(FindClientId("Client ID\t" + id + "\nClient secret: View client secret"), id, "find: a copied dashboard block");
    ExpectEq(FindClientId(id + " " + id2), "", "find: two IDs -> none");
    ExpectEq(FindClientId(id.substr(0, 31)), "", "find: a 31-character run -> none");
    ExpectEq(FindClientId(id + "x"), "", "find: a 33-character run -> none");
    ExpectEq(FindClientId("id=" + id.substr(0, 31) + " and " + id + "xy"), "", "find: 31 and 34 runs -> none");
    ExpectEq(FindClientId("\xC3\xA9" + id + "\xE2\x80\x8B"), id, "find: non-ASCII bytes separate runs");
    ExpectEq(FindClientId(""), "", "find: empty");
    ExpectEq(FindClientId("https://open.spotify.com/track/4uLU6hMCjMI75M1A2tKUQC"), "", "find: a track link is not a Client ID");
    ExpectEq(FindClientId("token " + std::string(64, 'a')), "", "find: a 64-character secret is not one");
}

void TestNotAllowlisted() {
    const std::string plain = "Check settings on developer.spotify.com/dashboard, the user may not be registered.";
    const std::string jsonBody = R"({"error":{"status":403,"message":"User not registered in the Developer Dashboard"}})";
    Expect(IsNotAllowlisted(403, plain) && IsNotAllowlisted(403, jsonBody), "not-allowlisted: both known texts");
    Expect(IsNotAllowlisted(403, "THE USER MAY NOT BE REGISTERED"), "not-allowlisted: case-insensitive");
    Expect(!IsNotAllowlisted(401, plain) && !IsNotAllowlisted(200, plain) && !IsNotAllowlisted(404, jsonBody), "not-allowlisted: 403 only");
    Expect(!IsNotAllowlisted(403, "not registered") && !IsNotAllowlisted(403, "developer"), "not-allowlisted: one half is not enough");
    Expect(!IsNotAllowlisted(403, std::string(1024, ' ') + "may not be registered"), "not-allowlisted: only the first 1024 bytes count");
    std::string m;
    Expect(MapApiError(403, plain + "\r\n", &m) == Err::NotAllowlisted && m == plain + "  ", "not-allowlisted: plain text, message = the text");
    Expect(MapApiError(403, jsonBody, &m) == Err::NotAllowlisted && m == "User not registered in the Developer Dashboard",
           "not-allowlisted: JSON message");
    MapApiError(403, std::string(300, 'x') + " may not be registered", &m);
    Expect(m.size() == 200, "not-allowlisted: a long text is trimmed to 200 bytes");
    Expect(MapApiError(403, R"({"error":{"status":403,"message":"Player command failed: Premium required","reason":"PREMIUM_REQUIRED"}})", &m) ==
               Err::Premium,
           "not-allowlisted: PREMIUM_REQUIRED keeps premium");
    Expect(MapApiError(401, plain, &m) == Err::Auth, "not-allowlisted: a 401 with the text stays auth");
    Expect(MapApiError(403, R"({"error":{"status":403,"message":"Premium required; user not registered in the developer plan",)"
                            R"("reason":"PREMIUM_REQUIRED"}})",
                       &m) == Err::Premium,
           "not-allowlisted: PREMIUM_REQUIRED wins even when its message matches the heuristic");
    Expect(MapApiError(403, plain, nullptr) == Err::NotAllowlisted, "not-allowlisted: no message pointer");
    ExpectEq(ErrName(Err::NotAllowlisted), "not-allowlisted", "error name not-allowlisted");
    ExpectEq(ShapeError(Err::NotAllowlisted, "x", 3), R"({"error":"not-allowlisted","message":"x"})", "error shape: not-allowlisted");
}

void TestTransfer() {
    Request r;
    r.op = Op::Transfer;
    r.deviceId = "abc_DEF-1";
    std::string method, url, body;
    Expect(BuildCall(r, &method, &url, &body) && method == "PUT" && url == std::string(kApiBase) + "me/player", "transfer: PUT /v1/me/player");
    ExpectEq(body, R"({"device_ids":["abc_DEF-1"],"play":false})", "transfer: the body (play false)");
    r.play = true;
    Expect(BuildCall(r, &method, &url, &body), "transfer: play true builds");
    ExpectEq(body, R"({"device_ids":["abc_DEF-1"],"play":true})", "transfer: the body (play true)");
    r.deviceId = "";
    Expect(!BuildCall(r, &method, &url, &body), "transfer: no device -> malformed");
    r.deviceId = "bad id!";
    Expect(!BuildCall(r, &method, &url, &body), "transfer: an invalid device -> malformed");
    r.deviceId = std::string(129, 'a');
    Expect(!BuildCall(r, &method, &url, &body), "transfer: a 129-character device -> malformed");
    Expect(IsControl(Op::Transfer) && !IsControl(Op::Devices), "transfer is a control call");
    Expect(ProductAfter("", Op::Transfer, true, Err::None) == "premium" && ProductAfter("", Op::Transfer, false, Err::Premium) == "free" &&
               ProductAfter("premium", Op::Transfer, false, Err::NotFound) == "premium",
           "transfer teaches the plan");
    std::string out;
    r.deviceId = "abc";
    Expect(ShapeSuccess(r, 204, "", &out) && out == R"({"ok":true})", "transfer: 204 -> ok");

    const json::Value v = J(ShapeDevices(J(
        R"({"devices":[{"id":"a1","name":"Phone","type":"Smartphone","is_active":false,"is_private_session":true,"is_restricted":false,)"
        R"("volume_percent":70,"supports_volume":false},{"id":"b2","name":"PS5","type":"GameConsole","is_active":true},)"
        R"({"id":"c3","name":"Web","type":"Computer","is_private_session":false,"supports_volume":true},)"
        R"({"id":"d4","name":"X","type":"Speaker","supports_volume":"yes"}]})")));
    const json::Value* items = v.Get("items");
    Expect(items && items->items.size() == 4, "devices: four kept");
    if (items && items->items.size() == 4) {
        const json::Value& p = items->items[0];
        Expect(p.Get("private") && p.Get("private")->boolean && p.Get("supportsVolume") && !p.Get("supportsVolume")->boolean &&
                   p.Get("type")->string == "Smartphone" && p.Get("volume")->number == 70,
               "devices: private and supportsVolume false");
        const json::Value& q = items->items[1];
        Expect(q.Get("private") && !q.Get("private")->boolean && !q.Get("supportsVolume") && q.Get("type")->string == "GameConsole" &&
                   !q.Get("volume"),
               "devices: private false by default, supportsVolume absent when not sent");
        Expect(items->items[2].Get("supportsVolume") && items->items[2].Get("supportsVolume")->boolean, "devices: supportsVolume true");
        Expect(!items->items[3].Get("supportsVolume"), "devices: a non-boolean supports_volume is left out");
    }
    const json::Value longType = J(ShapeDevices(J(R"({"devices":[{"id":"a","type":")" + std::string(40, 'T') + R"("}]})")));
    Expect(longType.Get("items")->items[0].Get("type")->string.size() == 32, "devices: type trimmed to 32 bytes");
}

void TestTokens() {
    Tokens t;
    std::string oe;
    Expect(ParseTokenResponse(R"({"access_token":"AT","token_type":"Bearer","expires_in":3600,"scope":"a b"})", &t, &oe) && t.access == "AT" &&
               t.expiresIn == 3600 && t.refresh.empty() && t.scope == "a b" && oe.empty(),
           "token response without a refresh token");
    Expect(ParseTokenResponse(R"({"access_token":"AT2","token_type":"bearer","expires_in":3600,"refresh_token":"RT2"})", &t, &oe) &&
               t.refresh == "RT2",
           "token response with a rotated refresh token");
    Expect(!ParseTokenResponse(R"({"error":"invalid_grant","error_description":"Refresh token revoked"})", &t, &oe) && oe == "invalid_grant",
           "token response: invalid_grant");
    Expect(!ParseTokenResponse(R"({"error":"invalid_client"})", &t, &oe) && oe == "invalid_client", "token response: invalid_client");
    Expect(!ParseTokenResponse(R"({"access_token":"AT","token_type":"MAC","expires_in":3600})", &t, &oe) && oe.empty(),
           "token response: not Bearer");
    Expect(!ParseTokenResponse(R"({"access_token":"AT","token_type":"Bearer"})", &t, &oe), "token response: no expires_in");
    Expect(!ParseTokenResponse("<html>", &t, &oe) && oe.empty(), "token response: not JSON");

    Expect(NeedsRefresh(1000, 0, true) && NeedsRefresh(1000, 99999999, false), "expiry: no token or no expiry refreshes");
    Expect(!NeedsRefresh(1000, ExpiresAtMs(1000, 3600), true), "expiry: a fresh token is used");
    Expect(NeedsRefresh(1000 + 3600000 - 60000, ExpiresAtMs(1000, 3600), true), "expiry: refresh 60 s before the end");
    Expect(!NeedsRefresh(1000 + 3600000 - 60001, ExpiresAtMs(1000, 3600), true), "expiry: not earlier");

    Me me;
    Expect(ParseMe(R"({"id":"u1","display_name":"Ann","product":"premium","country":"DE"})", &me) && me.id == "u1" && me.name == "Ann" &&
               me.product == "premium" && me.country == "DE",
           "me: premium");
    Expect(ParseMe(R"({"id":"u2","display_name":null,"product":"free"})", &me) && me.name == "u2" && me.product == "free",
           "me: free, no display name");
    Expect(ParseMe(R"({"id":"u3","display_name":")" + std::string(100, 'n') + R"("})", &me) && me.name.size() == 64, "me: name trimmed to 64");
    Expect(!ParseMe(R"({"display_name":"x"})", &me), "me: no id");

    StoredTokens s;
    s.clientId = "0123456789abcdef0123456789ABCDEF";
    s.refresh = "R\"\\";
    s.user = "Ann";
    s.product = "premium";
    s.grants = {"holo", "other"};
    StoredTokens back;
    Expect(DecodeStored(EncodeStored(s), &back) && back.clientId == s.clientId && back.refresh == s.refresh && back.user == "Ann" &&
               back.grants.size() == 2 && back.grants[1] == "other",
           "stored sign-in round-trips");
    Expect(!DecodeStored(R"({"v":2,"clientId":"0123456789abcdef0123456789ABCDEF","refresh":"r"})", &back), "stored: another version");
    Expect(!DecodeStored(R"({"v":1,"clientId":"short","refresh":"r"})", &back), "stored: a bad client id");
}

void TestErrors() {
    std::string m;
    Expect(MapApiError(401, R"({"error":{"status":401,"message":"The access token expired"}})", &m) == Err::Auth &&
               m == "The access token expired",
           "401 auth");
    Expect(MapApiError(403, R"({"error":{"status":403,"message":"Player command failed: Premium required","reason":"PREMIUM_REQUIRED"}})", &m) ==
               Err::Premium,
           "403 PREMIUM_REQUIRED");
    Expect(MapApiError(403, R"({"error":{"status":403,"message":"Restriction violated","reason":"UNKNOWN"}})", &m) == Err::Restricted,
           "403 other");
    Expect(MapApiError(404, R"({"error":{"status":404,"message":"Player command failed: No active device found","reason":"NO_ACTIVE_DEVICE"}})",
                       &m) == Err::NoDevice,
           "404 NO_ACTIVE_DEVICE");
    Expect(MapApiError(404, R"({"error":{"status":404,"message":"No active device found"}})", &m) == Err::NoDevice, "404 by message");
    Expect(MapApiError(404, R"({"error":{"status":404,"message":"Resource not found"}})", &m) == Err::NotFound, "404 other");
    Expect(MapApiError(400, "", &m) == Err::BadRequest && m.empty(), "400");
    Expect(MapApiError(429, "", &m) == Err::Rate, "429");
    Expect(MapApiError(500, "oops", &m) == Err::Server && MapApiError(503, "", &m) == Err::Server, "5xx");
    Expect(MapApiError(418, "", &m) == Err::Restricted && MapApiError(302, "", &m) == Err::Restricted, "other non-2xx");
    MapApiError(400, R"({"error":{"status":400,"message":")" + std::string(300, 'x') + R"("}})", &m);
    Expect(m.size() == 200, "the message is trimmed to 200 bytes");
    Expect(std::string(ErrName(Err::NoDevice)) == "no-device" && std::string(ErrName(Err::BadResponse)) == "bad-response" &&
               std::string(ErrName(Err::None)).empty() && std::string(ErrName(Err::Cancelled)) == "cancelled",
           "error names");

    Expect(RetryAfterSeconds("") == 5 && RetryAfterSeconds("abc") == 5 && RetryAfterSeconds("0") == 1 && RetryAfterSeconds("7") == 7 &&
               RetryAfterSeconds("999") == 120 && RetryAfterSeconds(" 3 ") == 3 && RetryAfterSeconds("99999999999999999999") == 120,
           "Retry-After");

    RateGate g(60);  // one a second
    uint64_t now = 1000000;
    int burst = 0;
    while (g.WaitMs(now) == 0 && burst < 100) {
        g.Take(now);
        ++burst;
    }
    Expect(burst == 60, "rate gate: a burst of the whole bucket (" + std::to_string(burst) + ")");
    const uint64_t w = g.WaitMs(now);
    Expect(w >= 999 && w <= 1000, "rate gate: then one token a second (" + std::to_string(w) + ")");
    Expect(g.WaitMs(now + 1000) == 0, "rate gate: the refill");
    g.Take(now + 1000);
    Expect(g.WaitMs(now + 1000) > 0, "rate gate: taken again");
    g.PauseUntil(now + 30000);
    Expect(g.WaitMs(now + 120000) == 0 && g.WaitMs(now + 20000) == 10000, "rate gate: PauseUntil");
    g.PauseUntil(now + 10000);
    Expect(g.WaitMs(now + 20000) == 10000, "rate gate: an earlier pause does not shorten a later one");

    Attempts a;
    Expect(Decide(200, &a) == Action::Shape && Decide(204, &a) == Action::Shape, "decide: 2xx");
    Expect(Decide(401, &a) == Action::RefreshRetry && Decide(401, &a) == Action::Fail, "decide: 401 refreshes once");
    Expect(Decide(429, &a) == Action::RateRetry && Decide(429, &a) == Action::RateRetry, "decide: 429 waits");
    Expect(Decide(502, &a) == Action::ServerRetry && Decide(503, &a) == Action::Fail, "decide: 5xx retries once");
    Expect(Decide(404, &a) == Action::Fail, "decide: 404 fails");

    // A request's call never starts after its deadline; with time left it gets at least 1 s, at most the cap.
    Expect(BudgetMs(1000, 1000, 10000) == 0 && BudgetMs(2000, 1000, 10000) == 0, "budget: none once the deadline passed");
    Expect(BudgetMs(1000, 1500, 10000) == 1000 && BudgetMs(1000, 8000, 10000) == 7000 && BudgetMs(0, 60000, 10000) == 10000,
           "budget: what is left, at least 1 s, at most the cap");
    Expect(BudgetMs(0, 300, 500) == 500 && BudgetMs(0, 5000, 500) == 500 && BudgetMs(0, 5000, 0) == 5000 && BudgetMs(0, 300, 0) == 1000,
           "budget: a cap under 1 s holds, and a cap of 0 (no total limit) leaves what is left");
    // WinHTTP's resolve, connect and send timeouts are capped by the request's total time.
    melange::store::fetch::Options fo;
    fo.totalMs = 0;
    Expect(melange::store::fetch::PhaseTimeoutMs(fo) == 10000, "phase timeout: 10 s without a total");
    fo.totalMs = 2500;
    Expect(melange::store::fetch::PhaseTimeoutMs(fo) == 2500, "phase timeout: capped by the total");
    fo.totalMs = 60000;
    Expect(melange::store::fetch::PhaseTimeoutMs(fo) == 10000, "phase timeout: never above 10 s");

    WarnOnce once;
    Expect(once.First("RequestsPerMinute", "5") && !once.First("RequestsPerMinute", "5") && !once.First("RequestsPerMinute", "5"),
           "warn once: a re-read of the same bad value is quiet");
    Expect(once.First("RedirectPort", "5") && once.First("RequestsPerMinute", "7") && !once.First("RedirectPort", "5"),
           "warn once: per key, and again for a new value");

    ExpectEq(ShapeError(Err::Rate, "slow down", 7), R"({"error":"rate","message":"slow down","retryAfter":7})", "error shape: rate");
    ExpectEq(ShapeError(Err::Premium, "", 7), R"({"error":"premium","message":""})", "error shape: retryAfter only for rate");
}

void TestPlanAndQueues() {
    // status().premium: unknown (nil) when Spotify did not say; GET /me has no `product` for Development-mode apps.
    Expect(!PremiumOf("").has_value() && PremiumOf("premium") == true && PremiumOf("free") == false && PremiumOf("open") == false,
           "premium: unknown, true, false");
    Expect(ProductAfter("", Op::Pause, true, Err::None) == "premium" && ProductAfter("free", Op::Play, true, Err::None) == "premium",
           "plan: a control call that works means Premium (an upgrade is learned)");
    Expect(ProductAfter("", Op::Play, false, Err::Premium) == "free" && ProductAfter("premium", Op::Volume, false, Err::Premium) == "free",
           "plan: PREMIUM_REQUIRED means Free");
    Expect(ProductAfter("open", Op::Play, false, Err::Premium) == "open", "plan: a known non-Premium plan is kept");
    Expect(ProductAfter("", Op::Playback, true, Err::None).empty() && ProductAfter("", Op::Tracks, false, Err::Premium).empty() &&
               ProductAfter("", Op::Pause, false, Err::NoDevice).empty() && ProductAfter("premium", Op::Seek, false, Err::Restricted) == "premium",
           "plan: reads and other errors say nothing");
    Expect(ProductAtStartup("premium", "") == "premium" && ProductAtStartup("free", "").empty() && ProductAtStartup("", "").empty() &&
               ProductAtStartup("premium", "free") == "free" && ProductAtStartup("", "premium") == "premium",
           "plan at start-up: /me wins; without it a learned Premium is kept and a learned Free is asked again (an upgrade shows)");

    // The worker's turn order: a mod that keeps control calls queued cannot starve the reads.
    int streak = 0;
    std::string order;
    for (int i = 0; i < 10; ++i) order += TakeControl(true, true, &streak) ? 'c' : 'r';
    ExpectEq(order, "cccrcccrcc", "queues: a waiting read gets every 4th turn");
    streak = 0;
    order.clear();
    for (int i = 0; i < 5; ++i) order += TakeControl(true, false, &streak) ? 'c' : 'r';
    Expect(order == "ccccc" && !TakeControl(false, true, &streak) && TakeControl(true, true, &streak) && streak == 1,
           "queues: control alone runs on; a read resets the streak");
    struct J {
        int id;
        uint64_t deadline;
        bool background;
    };
    std::deque<J> q{{1, 100, false}, {2, 300, false}, {3, 100, true}, {4, 200, false}};
    std::vector<int> expired;
    const size_t n = ExpireQueued(q, 200, [&](const J& j) { expired.push_back(j.id); });
    Expect(n == 2 && expired == std::vector<int>{1, 4} && q.size() == 2 && q[0].id == 2 && q[1].id == 3,
           "queues: jobs past their deadline expire while queued (a background one stays)");
}

void TestCalls() {
    std::string m, u, b;
    Request r;
    r.op = Op::Tracks;
    r.limit = 20;
    r.offset = 40;
    Expect(BuildCall(r, &m, &u, &b) && m == "GET" && u == "https://api.spotify.com/v1/me/tracks?limit=20&offset=40&market=from_token" && b.empty(),
           "call: tracks");
    r.limit = 51;
    Expect(!BuildCall(r, &m, &u, &b), "call: limit 51");
    r.limit = 0;
    Expect(!BuildCall(r, &m, &u, &b), "call: limit 0");
    r = {};
    r.op = Op::Playlists;
    Expect(BuildCall(r, &m, &u, &b) && m == "GET" && u == "https://api.spotify.com/v1/me/playlists?limit=50&offset=0", "call: playlists");
    r = {};
    r.op = Op::PlaylistTracks;
    r.id = "37i9dQZF1DXcBWIGoYBM5M";
    // Spotify's February 2026 change: /playlists/{id}/items with "item" entries (the old /tracks is gone for
    // Development-mode apps); the fields filter asks for "item" and the older "track".
    Expect(BuildCall(r, &m, &u, &b) && m == "GET" &&
               u == "https://api.spotify.com/v1/playlists/37i9dQZF1DXcBWIGoYBM5M/items?limit=50&offset=0&market=from_token&fields="
                    "total%2Cnext%2Citems%28is_local%2Citem%28type%2Curi%2Cname%2Cduration_ms%2Cis_playable%2Cis_local%2C"
                    "artists%28name%29%2Calbum%28name%29%29%2Ctrack%28type%2Curi%2Cname%2Cduration_ms%2Cis_playable%2Cis_local%2C"
                    "artists%28name%29%2Calbum%28name%29%29%29",
           "call: playlist items (not /tracks) with fields for item and track");
    r.id = "../me";
    Expect(!BuildCall(r, &m, &u, &b), "call: a bad playlist id");
    r = {};
    r.op = Op::Devices;
    Expect(BuildCall(r, &m, &u, &b) && m == "GET" && u == "https://api.spotify.com/v1/me/player/devices", "call: devices");
    r = {};
    r.op = Op::Play;
    r.uris = {"spotify:track:4uLU6hMCjMI75M1A2tKUQC", "spotify:track:1301WleyT98MSxVHPZCA6M"};
    r.positionMs = 1500;
    r.deviceId = "dev_1";
    Expect(BuildCall(r, &m, &u, &b) && m == "PUT" && u == "https://api.spotify.com/v1/me/player/play?device_id=dev_1" &&
               b == R"({"uris":["spotify:track:4uLU6hMCjMI75M1A2tKUQC","spotify:track:1301WleyT98MSxVHPZCA6M"],"position_ms":1500})",
           "call: play uris with a device and position");
    r = {};
    r.op = Op::Play;
    r.context = "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M";
    r.offsetIndex = 3;
    Expect(BuildCall(r, &m, &u, &b) && u == "https://api.spotify.com/v1/me/player/play" &&
               b == R"({"context_uri":"spotify:playlist:37i9dQZF1DXcBWIGoYBM5M","offset":{"position":3}})",
           "call: play a playlist from an index");
    r.offsetIndex = -1;
    r.offsetUri = "spotify:track:4uLU6hMCjMI75M1A2tKUQC";
    Expect(BuildCall(r, &m, &u, &b) &&
               b == R"({"context_uri":"spotify:playlist:37i9dQZF1DXcBWIGoYBM5M","offset":{"uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC"}})",
           "call: play a playlist from a track");
    r.uris = {"spotify:track:4uLU6hMCjMI75M1A2tKUQC"};
    Expect(!BuildCall(r, &m, &u, &b), "call: play with two sources");
    r = {};
    r.op = Op::Play;
    Expect(!BuildCall(r, &m, &u, &b), "call: play without a source");
    r.uris = {"spotify:album:4uLU6hMCjMI75M1A2tKUQC"};
    Expect(!BuildCall(r, &m, &u, &b), "call: play an album URI");
    r.uris = std::vector<std::string>(51, "spotify:track:4uLU6hMCjMI75M1A2tKUQC");
    Expect(!BuildCall(r, &m, &u, &b), "call: play 51 URIs");
    r = {};
    r.op = Op::Resume;
    Expect(BuildCall(r, &m, &u, &b) && m == "PUT" && u == "https://api.spotify.com/v1/me/player/play" && b.empty(), "call: resume, empty body");
    r.op = Op::Pause;
    r.deviceId = "d";
    Expect(BuildCall(r, &m, &u, &b) && m == "PUT" && u == "https://api.spotify.com/v1/me/player/pause?device_id=d" && b.empty(), "call: pause");
    r.op = Op::Seek;
    r.positionMs = 61000;
    Expect(BuildCall(r, &m, &u, &b) && u == "https://api.spotify.com/v1/me/player/seek?position_ms=61000&device_id=d", "call: seek");
    r.positionMs = -1;
    Expect(!BuildCall(r, &m, &u, &b), "call: seek without a position");
    r = {};
    r.op = Op::Volume;
    r.volume = 35;
    Expect(BuildCall(r, &m, &u, &b) && u == "https://api.spotify.com/v1/me/player/volume?volume_percent=35", "call: volume");
    r.volume = 101;
    Expect(!BuildCall(r, &m, &u, &b), "call: volume 101");
    r = {};
    r.op = Op::Playback;
    Expect(BuildCall(r, &m, &u, &b) && m == "GET" && u == "https://api.spotify.com/v1/me/player?market=from_token", "call: playback");
    r.deviceId = "a b";
    Expect(!BuildCall(r, &m, &u, &b), "call: a bad device id");
}

void TestShapes() {
    const std::string page = R"({"href":"x","total":5,"next":"https://api.spotify.com/v1/me/tracks?offset=50","items":[)"
                             R"({"track":{"type":"track","uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","name":"Song","duration_ms":201000,)"
                             R"("is_playable":true,"artists":[{"name":"A"},{"name":"B"}],"album":{"name":"Alb"}}},)"
                             R"({"track":{"type":"track","uri":"spotify:local:x:y:z:1","name":"Local","is_local":true}},)"
                             R"({"track":{"type":"episode","uri":"spotify:episode:4uLU6hMCjMI75M1A2tKUQC","name":"Ep"}},)"
                             R"({"track":null},)"
                             R"({"track":{"type":"track","uri":"spotify:track:1301WleyT98MSxVHPZCA6M","name":")" +
                             std::string(199, 'n') + "\xC3\xA9" + R"(","duration_ms":1,"is_playable":false,"artists":[],"album":{}}}]})";
    const std::string t = ShapeTracks(J(page), true, 0, 50);
    json::Value v = J(t);
    const json::Value* items = v.Get("items");
    Expect(v.Get("kind") && v.Get("kind")->string == "tracks" && items && items->items.size() == 2, "tracks: two kept");
    Expect(v.Get("skipped") && v.Get("skipped")->number == 3, "tracks: local, episode and null counted in skipped");
    Expect(v.Get("total") && v.Get("total")->number == 5 && v.Get("next") && v.Get("next")->boolean && v.Get("offset")->number == 0 &&
               v.Get("limit")->number == 50,
           "tracks: total, next, offset, limit");
    if (items && items->items.size() == 2) {
        const json::Value& a = items->items[0];
        Expect(a.Get("uri")->string == "spotify:track:4uLU6hMCjMI75M1A2tKUQC" && a.Get("name")->string == "Song" &&
                   a.Get("artists")->string == "A, B" && a.Get("album")->string == "Alb" && a.Get("durationMs")->number == 201000 &&
                   a.Get("playable")->boolean,
               "tracks: the fields, artists joined");
        const json::Value& b = items->items[1];
        Expect(b.Get("name")->string == std::string(199, 'n') && !b.Get("playable")->boolean && b.Get("artists")->string.empty(),
               "tracks: a name trimmed on a UTF-8 boundary, not playable");
    }
    // GET /playlists/{id}/items: each entry's song is under "item" (with a deprecated "track" copy, or none).
    v = J(ShapeTracks(J(R"({"total":3,"next":null,"items":[)"
                        R"({"added_at":"x","is_local":false,"item":{"type":"track","uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC",)"
                        R"("name":"New","duration_ms":5,"is_playable":true,"artists":[{"name":"A"}],"album":{"name":"Alb"}}},)"
                        R"({"item":{"type":"episode","uri":"spotify:episode:4uLU6hMCjMI75M1A2tKUQC","name":"Ep"},)"
                        R"("track":{"type":"track","uri":"spotify:track:1301WleyT98MSxVHPZCA6M","name":"Stale"}},)"
                        R"({"is_local":true,"item":{"type":"track","uri":"spotify:local:a:b:c:1","name":"L","is_local":true}}]})"),
                      false, 0, 50));
    items = v.Get("items");
    Expect(items && items->items.size() == 1 && v.Get("skipped")->number == 2 && v.Get("kind")->string == "playlist",
           "playlist items: the item key is read (an episode item is skipped even with a track copy, a local file too)");
    if (items && items->items.size() == 1)
        Expect(items->items[0].Get("uri")->string == "spotify:track:4uLU6hMCjMI75M1A2tKUQC" && items->items[0].Get("name")->string == "New" &&
                   items->items[0].Get("artists")->string == "A" && items->items[0].Get("durationMs")->number == 5,
               "playlist items: the song's fields");
    v = J(ShapeTracks(J(R"({"total":1,"items":[{"track":{"type":"track","uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","name":"Old"}}]})"),
                      false, 0, 50));
    Expect(v.Get("items") && v.Get("items")->items.size() == 1 && v.Get("skipped")->number == 0, "playlist items: an older track-only answer still reads");
    v = J(ShapeTracks(J(R"({"items":[],"total":0,"next":null})"), false, 100, 10));
    Expect(v.Get("kind")->string == "playlist" && !v.Get("next")->boolean && v.Get("offset")->number == 100, "playlist tracks: kind, next null");

    v = J(ShapePlaylists(J(R"({"total":2,"next":null,"items":[{"id":"37i9dQZF1DXcBWIGoYBM5M","name":"Mix","owner":{"display_name":"Ann"},)"
                           R"("tracks":{"total":12}},{"id":"1301WleyT98MSxVHPZCA6M","name":"New","owner":{"id":"bob"},"items":{"total":3}},)"
                           R"({"id":null}]})"),
                         0, 50));
    items = v.Get("items");
    Expect(items && items->items.size() == 2 && v.Get("skipped")->number == 1 && v.Get("kind")->string == "playlists", "playlists: kept and skipped");
    if (items && items->items.size() == 2)
        Expect(items->items[0].Get("uri")->string == "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M" && items->items[0].Get("owner")->string == "Ann" &&
                   items->items[0].Get("tracks")->number == 12 && items->items[1].Get("tracks")->number == 3 &&
                   items->items[1].Get("owner")->string == "bob",
               "playlists: uri, owner, tracks (and items.total)");

    v = J(ShapeDevices(J(R"({"devices":[{"id":"abc","name":"PC","type":"Computer","is_active":true,"is_restricted":false,"volume_percent":40},)"
                         R"({"id":null,"name":"Ghost","type":"Speaker"}]})")));
    items = v.Get("items");
    Expect(items && items->items.size() == 1 && items->items[0].Get("id")->string == "abc" && items->items[0].Get("active")->boolean &&
               items->items[0].Get("volume")->number == 40 && !items->items[0].Get("restricted")->boolean,
           "devices: a null id is dropped");

    ExpectEq(ShapePlayback(204, nullptr, 0), R"({"active":false})", "playback: 204");
    const json::Value pb = J(R"({"is_playing":true,"progress_ms":5000,"item":{"type":"track",)"
                             R"("uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC","name":"Song","duration_ms":9000,)"
                             R"("artists":[{"name":"A"}]},"device":{"id":"abc","name":"PC","type":"Computer",)"
                             R"("volume_percent":50},"context":{"uri":"spotify:playlist:37i9dQZF1DXcBWIGoYBM5M"}})");
    v = J(ShapePlayback(200, &pb, 0));
    Expect(v.Get("active")->boolean && v.Get("playing")->boolean && v.Get("uri")->string == "spotify:track:4uLU6hMCjMI75M1A2tKUQC" &&
               v.Get("artists")->string == "A" && v.Get("durationMs")->number == 9000 && v.Get("positionMs")->number == 5000 &&
               v.Get("at") && v.Get("device")->Get("volume")->number == 50 &&
               v.Get("context")->string == "spotify:playlist:37i9dQZF1DXcBWIGoYBM5M",
           "playback: a track");
    Expect(!v.Get("linkedFrom"), "playback: no linkedFrom when Spotify did not relink");
    const json::Value relinked = J(R"({"is_playing":true,"progress_ms":1,"item":{"type":"track","uri":"spotify:track:1301WleyT98MSxVHPZCA6M",)"
                                   R"("name":"Song","linked_from":{"type":"track","uri":"spotify:track:4uLU6hMCjMI75M1A2tKUQC"}}})");
    v = J(ShapePlayback(200, &relinked, 0));
    Expect(v.Get("uri") && v.Get("uri")->string == "spotify:track:1301WleyT98MSxVHPZCA6M" && v.Get("linkedFrom") &&
               v.Get("linkedFrom")->string == "spotify:track:4uLU6hMCjMI75M1A2tKUQC",
           "playback: linkedFrom is the track asked for when Spotify relinked it");
    const json::Value badLink = J(R"({"item":{"type":"track","uri":"spotify:track:1301WleyT98MSxVHPZCA6M","linked_from":{"uri":"spotify:album:x"}}})");
    Expect(!J(ShapePlayback(200, &badLink, 0)).Get("linkedFrom"), "playback: a linked_from that is not a track URI is left out");
    const json::Value ep = J(R"({"is_playing":false,"progress_ms":1,"item":{"type":"episode","uri":"spotify:episode:x","name":"Ep"},)"
                             R"("device":{"id":"abc","name":"PC","type":"Computer"},"context":null})");
    v = J(ShapePlayback(200, &ep, 0));
    Expect(v.Get("active")->boolean && !v.Get("uri") && !v.Get("name") && !v.Get("durationMs") && !v.Get("context") &&
               !v.Get("playing")->boolean,
           "playback: an episode has no song fields");

    std::string out;
    Request r;
    r.op = Op::Pause;
    Expect(ShapeSuccess(r, 204, "", &out) && out == R"({"ok":true})", "success: control ops give ok");
    r.op = Op::Playback;
    Expect(ShapeSuccess(r, 204, "", &out) && out == R"({"active":false})", "success: playback 204");
    r.op = Op::Devices;
    Expect(!ShapeSuccess(r, 200, "<html>", &out), "success: a bad body is bad-response");
}
}  // namespace

int main() {
    TestEncoding();
    TestIds();
    TestCallback();
    TestTokens();
    TestErrors();
    TestCalls();
    TestShapes();
    TestPlanAndQueues();
    TestPages();
    TestSetup();
    TestFindClientId();
    TestNotAllowlisted();
    TestTransfer();
    printf("spotify_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
