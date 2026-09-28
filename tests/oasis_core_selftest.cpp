// Offline self-test for the Oasis server core (no game needed). Exit code 0 = all passed.
//   oasis_core_selftest [--mutate <seconds>]
// Pure parts (validation, WebSocket rules, queues, the protocol router) are driven directly; the HTTP/WebSocket
// corpus runs against a live server on 127.0.0.1. --mutate adds a randomized run of mutated requests and frames,
// then checks the server still answers, and that memory and handles came back after Stop().
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <psapi.h>

#include <miniz.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_DEBUG)
#include <crtdbg.h>
#endif

#include "melange/oasis.h"
#include "oasis/core/files.h"
#include "oasis/core/http.h"
#include "oasis/core/queue.h"
#include "oasis/core/router.h"
#include "oasis/core/server.h"
#include "oasis/core/ws.h"
#include "oasis/providers.h"

namespace oc = melange::oasis::core;
namespace oa = melange::oasis;

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

// ---------------------------------------------------------------- pure parts
oc::Request Req(std::string method, std::string target, std::vector<std::pair<std::string, std::string>> h) {
    oc::Request r;
    r.method = std::move(method);
    r.target = std::move(target);
    const size_t q = r.target.find('?');
    r.path = r.target.substr(0, q);
    r.query = q == std::string::npos ? "" : r.target.substr(q + 1);
    r.headers = std::move(h);
    r.port = 8765;
    return r;
}

void TestHttp() {
    using H = std::vector<std::pair<std::string, std::string>>;
    const H host = {{"Host", "127.0.0.1:8765"}};
    Expect(oc::Validate(Req("GET", "/", host)) == 0, "valid GET");
    Expect(oc::Validate(Req("HEAD", "/app/x.js", host)) == 0, "valid HEAD");
    Expect(oc::Validate(Req("POST", "/", host)) == 405, "POST is 405");
    Expect(oc::Validate(Req("G(T", "/", host)) == 400, "bad method token is 400");
    Expect(oc::Validate(Req("GET", "/", {})) == 400, "missing Host");
    Expect(oc::Validate(Req("GET", "/", {{"Host", "a"}, {"host", "b"}})) == 400, "duplicate Host");
    Expect(oc::Validate(Req("GET", "/", {{"Host", "a"}, {"Transfer-Encoding", "chunked"}})) == 400, "Transfer-Encoding");
    Expect(oc::Validate(Req("GET", "/", {{"Host", "a"}, {"Content-Length", "5"}})) == 400, "request body");
    Expect(oc::Validate(Req("GET", "/", {{"Host", "a"}, {"Content-Length", "0"}})) == 0, "empty body is fine");
    Expect(oc::Validate(Req("GET", "/caf\xc3\xa9", host)) == 400, "non-ASCII target");
    Expect(oc::Validate(Req("GET", std::string("/a\0b", 4), host)) == 400, "NUL in target");
    Expect(oc::Validate(Req("GET", "x", host)) == 400, "target without /");
    Expect(oc::Validate(Req("GET", "/", {{"Host", "a"}, {"X", std::string("a\0b", 3)}})) == 400, "NUL in a header value");
    Expect(oc::Validate(Req("GET", "/", {{"Host", "a"}, {"X Y", "1"}})) == 400, "space in a header name");
    H many = host;
    for (int i = 0; i < 64; ++i) many.emplace_back("X-" + std::to_string(i), "1");
    Expect(oc::Validate(Req("GET", "/", many)) == 431, "65 headers");
    Expect(oc::Validate(Req("GET", "/", {{"Host", "a"}, {"X", std::string(17000, 'a')}})) == 431, "16 KB of headers");

    Expect(oc::SafePath("app/main.js") && oc::SafePath("index.html") && oc::SafePath("app/chunks/a-B_c~1.js"), "safe paths");
    for (const char* bad : {"", "../x", "app/../x", "app//x", "/x", "app/./x", "a\\b", "a%2e", "a b", "app/"})
        Expect(!oc::SafePath(bad), "unsafe path rejected", bad);

    uint64_t a = 0, b = 0;
    Expect(oc::ParseRange("bytes=0-99", 1000, &a, &b) && a == 0 && b == 99, "range 0-99");
    Expect(oc::ParseRange("bytes=900-", 1000, &a, &b) && a == 900 && b == 999, "open range");
    Expect(oc::ParseRange("bytes=-100", 1000, &a, &b) && a == 900 && b == 999, "suffix range");
    Expect(oc::ParseRange("bytes=0-5000", 1000, &a, &b) && b == 999, "range clamped");
    Expect(!oc::ParseRange("bytes=1000-", 1000, &a, &b), "range past the end");
    Expect(!oc::ParseRange("bytes=5-1", 1000, &a, &b), "reversed range");
    Expect(!oc::ParseRange("bytes=0-1,5-6", 1000, &a, &b), "multi range");
    Expect(!oc::ParseRange("items=0-1", 1000, &a, &b), "other unit");

    oc::Request c = Req("GET", "/?k=abc&x=1", {{"Host", "a"}, {"Cookie", "a=1; oasis_s=S3cr3t; b=2"}});
    Expect(c.Query("k") == "abc" && c.Query("x") == "1" && c.Query("y").empty(), "query parameters");
    Expect(c.Cookie("oasis_s") == "S3cr3t" && c.Cookie("c").empty(), "cookies");
    oc::Response r;
    r.status = 200;
    const std::string head = oc::Head(r, 5, true);
    Expect(head.find("Content-Security-Policy: default-src 'self'") != std::string::npos &&
               head.find("X-Content-Type-Options: nosniff") != std::string::npos &&
               head.find("Referrer-Policy: no-referrer") != std::string::npos &&
               head.find("Cache-Control: no-store") != std::string::npos &&
               head.find("Access-Control") == std::string::npos && head.find("Content-Length: 5") != std::string::npos,
           "response headers");
}

void TestWs() {
    using oc::ws::Assembler;
    Expect(oc::ws::Utf8Valid("plain", 5) && oc::ws::Utf8Valid("caf\xc3\xa9 \xf0\x9f\x8c\xb6", 10), "valid UTF-8");
    for (const char* bad : {"\xc3", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff", "\x80"})
        Expect(!oc::ws::Utf8Valid(bad, strlen(bad)), "invalid UTF-8 rejected");

    Assembler a(16);
    Expect(a.Feed(0x81, "hello", 5) == Assembler::Out::Message && a.message() == "hello", "single text frame");
    Expect(a.Feed(0x01, "he", 2) == Assembler::Out::None, "first fragment");
    Expect(a.Feed(0x89, "p", 1) == Assembler::Out::Control, "ping between fragments");
    Expect(a.Feed(0x00, "ll", 2) == Assembler::Out::None, "middle fragment");
    Expect(a.Feed(0x80, "o", 1) == Assembler::Out::Message && a.message() == "hello", "reassembled message");
    Expect(a.Feed(0x80, "x", 1) == Assembler::Out::Close && a.closeCode() == 1002, "continuation without a start");
    Expect(a.Feed(0x01, "a", 1) == Assembler::Out::None && a.Feed(0x81, "b", 1) == Assembler::Out::Close, "new message inside a fragment");
    Expect(a.Feed(0x82, "b", 1) == Assembler::Out::Close && a.closeCode() == 1003, "binary refused");
    Expect(a.Feed(0xc1, "a", 1) == Assembler::Out::Close && a.closeCode() == 1002, "RSV1 (compression) refused");
    Expect(a.Feed(0x83, "a", 1) == Assembler::Out::Close && a.closeCode() == 1002, "reserved opcode");
    Expect(a.Feed(0x81, "0123456789abcdefg", 17) == Assembler::Out::Close && a.closeCode() == 1009, "message over the cap");
    Expect(a.Feed(0x01, "0123456789", 10) == Assembler::Out::None && a.Feed(0x80, "abcdefg", 7) == Assembler::Out::Close &&
               a.closeCode() == 1009, "fragments over the cap");
    Expect(a.Feed(0x81, "\xc3", 1) == Assembler::Out::Close && a.closeCode() == 1007, "bad UTF-8 in text");
    Expect(a.Feed(0x01, "\xc3", 1) == Assembler::Out::None && a.Feed(0x80, "\xa9", 1) == Assembler::Out::Message,
           "UTF-8 split across fragments");
    std::string big(126, 'p');
    Expect(a.Feed(0x89, big.data(), big.size()) == Assembler::Out::Close && a.closeCode() == 1002, "control frame over 125");
    Expect(a.Feed(0x09, "p", 1) == Assembler::Out::Close, "fragmented control frame");
    Expect(a.Feed(0x88, "\x03\xe8", 2) == Assembler::Out::Close && a.closeCode() == 1000, "client close 1000");
    Expect(a.Feed(0x88, "\x03\xed", 2) == Assembler::Out::Close && a.closeCode() == 1002, "invalid close code 1005");
    Expect(a.Feed(0x88, "\x03", 1) == Assembler::Out::Close && a.closeCode() == 1002, "one-byte close");
    const std::string p = oc::ws::ClosePayload(4029, std::string(300, 'r'));
    Expect(p.size() == 125 && static_cast<unsigned char>(p[0]) == 0x0f && static_cast<unsigned char>(p[1]) == 0xbd, "close payload");
}

void TestOutbox() {
    oc::Outbox::Limits l;
    l.connectionBytes = 1000;
    l.batchItems = 3;
    oc::Outbox o(l);
    std::vector<std::string> out;
    Expect(!o.Publish(1, "1"), "publish without a subscription");
    o.Subscribe(1, "log", oa::Overflow::DropOldest, 20);
    uint64_t lost = 0;
    for (int i = 1; i <= 5; ++i) o.Publish(1, "\"" + std::string(3, static_cast<char>('a' + i)) + "\"", &lost);
    o.Take(1000, &out);
    Expect(lost == 1 && out.size() == 3 && out[0] == R"({"t":"drop","ch":"log","n":1,"why":"queue"})", "drop notice first",
           out.empty() ? "" : out[0]);
    Expect(out.size() == 3 && out[1] == R"({"t":"ev","ch":"log","seq":2,"b":["ccc","ddd","eee"]})" &&
               out[2] == R"({"t":"ev","ch":"log","seq":5,"d":"fff"})",
           "seq counts the dropped message; batches split", out.size() > 1 ? out[1] : "");
    out.clear();
    o.Publish(1, "1");
    Expect(o.Take(1010, &out) == 40 && out.empty(), "flushed at most every 50 ms");
    Expect(o.Take(1050, &out) == UINT32_MAX && out.size() == 1 && out[0] == R"({"t":"ev","ch":"log","seq":6,"d":1})", "single item as d",
           out.empty() ? "" : out[0]);
    out.clear();
    o.Subscribe(2, "state", oa::Overflow::Coalesce, 1000);
    o.Publish(2, "{\"a\":1}");
    o.Publish(2, "{\"a\":2}");
    o.Take(2000, &out);
    Expect(out.size() == 1 && out[0] == R"({"t":"ev","ch":"state","seq":2,"d":{"a":2}})", "coalesce keeps the latest", out.empty() ? "" : out[0]);
    out.clear();
    o.Subscribe(3, "big", oa::Overflow::DropOldest, 100000);
    for (int i = 0; i < 7; ++i) o.Publish(3, std::to_string(i));
    o.Take(3000, &out);
    Expect(out.size() == 3 && out[2] == R"({"t":"ev","ch":"big","seq":7,"d":6})", "batches of at most batchItems", out.empty() ? "" : out.back());
    out.clear();
    o.PushControl("{\"t\":\"res\"}");
    o.Publish(3, "9");
    o.Take(3010, &out);
    Expect(out.size() == 1 && out[0] == "{\"t\":\"res\"}", "control messages are never delayed");
    Expect(!o.PushControl(std::string(2000, 'x')), "control over the connection cap reports it");
    o.Unsubscribe(3);
    Expect(!o.Subscribed(3) && o.Subscribed(1), "unsubscribe");
}

// ---------------------------------------------------------------- router (no sockets)
struct FakeClient {
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int id = 0;
    std::vector<std::string> Take(uint16_t* code = nullptr, uint32_t now = 0) {
        std::vector<std::string> out;
        uint32_t wait = 0;
        uint16_t c = 0;
        std::string reason;
        oc::router::Take(id, now ? now : GetTickCount(), &out, &wait, &c, &reason);
        if (code) *code = c;
        return out;
    }
};

std::atomic<int> g_mainCalls{0};
void Echo(const oa::Call& c, oa::Result& r, void*) {
    ++g_mainCalls;
    r.json = std::string(c.paramsJson);
}
void Crash(const oa::Call&, oa::Result&, void*) {
    volatile int* p = nullptr;
    *p = 1;
}
void Throws(const oa::Call&, oa::Result&, void*) { throw 42; }
void Refuse(const oa::Call&, oa::Result& r, void*) {
    r.ok = false;
    r.code = -32000;
    r.message = "no";
}
std::vector<std::string> g_subLog;
void OnSub(oa::ChannelId, int client, std::string_view filter, bool on, void*) {
    g_subLog.push_back(std::to_string(client) + (on ? "+" : "-") + std::string(filter));
}

bool Has(const std::vector<std::string>& v, const char* needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

void TestRouter() {
    oc::Config cfg;
    cfg.maxClients = 2;
    oc::router::Configure(cfg);
    oc::Host host;
    host.server = "game";
    host.gameJson = R"({"exeBuild":1077,"melange":"test"})";
    oc::SetHost(host);
    oc::SetBuild("selftest");

    Expect(oa::AddChannel("Bad Name") == 0 && oa::AddChannel("") == 0 && oa::AddChannel(std::string(49, 'a').c_str()) == 0, "channel names checked");
    const oa::ChannelId log = oa::AddChannel("t.log");
    Expect(log != 0 && oa::AddChannel("t.log") == 0, "channel added once");
    oa::ChannelOptions co;
    co.overflow = oa::Overflow::Coalesce;
    co.mainThreadSubscribe = true;
    const oa::ChannelId st = oa::AddChannel("t.state", co);
    oa::OnSubscribe(log, &OnSub, nullptr);
    oa::OnSubscribe(st, &OnSub, nullptr);
    Expect(oa::AddMethod("noarea", &Echo, nullptr) == 0 && oa::AddMethod("T.x", &Echo, nullptr) == 0, "method names checked");
    Expect(oa::AddMethod("t.echo", &Echo, nullptr) != 0 && oa::AddMethod("t.echo", &Echo, nullptr) == 0, "method added once");
    oa::AddMethod("t.now", &Echo, nullptr, oa::kRpcServerThread);
    oa::AddMethod("t.crash", &Crash, nullptr, oa::kRpcServerThread);
    oa::AddMethod("t.throws", &Throws, nullptr, oa::kRpcServerThread);
    oa::AddMethod("t.refuse", &Refuse, nullptr, oa::kRpcServerThread);
    oa::AddMethod("t.write", &Echo, nullptr, oa::kRpcServerThread | oa::kRpcMutating);

    FakeClient a;
    a.id = oc::router::Open(a.ev);
    uint16_t code = 0;
    oc::router::Text(a.id, R"({"t":"sub","ch":"t.log"})");
    a.Take(&code);
    Expect(code == oc::kCloseNoHello, "message before hello closes 4002");
    oc::router::Gone(a.id);
    oc::router::Release(a.id);

    FakeClient b;
    b.id = oc::router::Open(b.ev);
    oc::router::Text(b.id, R"({"t":"hello","proto":2})");
    auto out = b.Take(&code);
    Expect(code == oc::kCloseProtocol && out.size() == 1 && out[0] == R"({"t":"bye","reason":"protocol","want":1})", "wrong proto: bye + 4001");
    oc::router::Gone(b.id);
    oc::router::Release(b.id);

    FakeClient c;
    c.id = oc::router::Open(c.ev);
    oc::router::Text(c.id, R"({"t":"hello","proto":1,"build":"x","client":"selftest"})");
    out = c.Take(&code);
    Expect(code == 0 && out.size() == 1 && out[0].starts_with(R"({"t":"welcome","proto":1,"build":"selftest","server":"game","game":{"exeBuild":1077)") &&
               Has(out, "\"t.log\"") && Has(out, "\"sys.ping\"") && Has(out, "\"maxClients\":2"),
           "welcome", out.empty() ? "" : out[0]);
    Expect(oa::Clients() == 1, "one client");
    oc::router::Text(c.id, "not json");
    oc::router::Text(c.id, R"({"x":1})");
    oc::router::Text(c.id, R"({"t":"nope"})");
    oc::router::Text(c.id, R"({"t":"call","m":"t.now"})");
    oc::router::Text(c.id, R"({"t":"call","id":1.5,"m":"t.now"})");
    oc::router::Text(c.id, R"({"t":"call","id":2,"m":"t.now","p":[1]})");
    oc::router::Text(c.id, R"({"t":"call","id":3,"m":"no.such"})");
    oc::router::Text(c.id, R"({"t":"sub","ch":"no.such","id":4})");
    out = c.Take();
    Expect(out.size() == 8 && out[0].find("-32600") != std::string::npos && out[1].find("-32600") != std::string::npos &&
               out[2].find("unknown message type") != std::string::npos && out[3].find(R"("id":null,"code":-32600)") != std::string::npos &&
               out[4].find("-32600") != std::string::npos && out[5].find(R"("id":2,"code":-32602)") != std::string::npos &&
               out[6].find(R"("id":3,"code":-32601)") != std::string::npos && out[7].find(R"("id":4,"code":-32602)") != std::string::npos,
           "envelope errors", std::to_string(out.size()));

    oc::router::Text(c.id, R"({"t":"call","id":10,"m":"t.now","p":{"a":[1,2.5,"x\n",true,null]}})");
    oc::router::Text(c.id, R"({"t":"call","id":11,"m":"t.refuse"})");
    oc::router::Text(c.id, R"({"t":"call","id":12,"m":"sys.ping"})");
    out = c.Take();
    Expect(out.size() == 3 && out[0] == R"({"t":"res","id":10,"r":{"a":[1,2.5,"x\n",true,null]}})" &&
               out[1] == R"({"t":"err","id":11,"code":-32000,"msg":"no"})" && out[2].starts_with(R"({"t":"res","id":12,"r":{"frame":0,"ms":)"),
           "server-thread calls", out.empty() ? "" : out[0]);

    for (int i = 0; i < 4; ++i) oc::router::Text(c.id, R"({"t":"call","id":20,"m":"t.crash"})");
    oc::router::Text(c.id, R"({"t":"call","id":21,"m":"t.throws"})");
    out = c.Take();
    Expect(out.size() == 5 && Has({out[0]}, "-32004") && Has({out[0]}, "0xc0000005") && Has({out[2]}, "method disabled") &&
               Has({out[3]}, "disabled after repeated faults") && Has({out[4]}, "-32004"),
           "faulting handler is contained and disabled after 3", out.empty() ? "" : out[0]);

    for (int i = 0; i < 17; ++i) oc::router::Text(c.id, (R"({"t":"call","id":)" + std::to_string(100 + i) + R"(,"m":"t.echo","p":{"i":)" + std::to_string(i) + "}}").c_str());
    out = c.Take();
    Expect(out.size() == 1 && Has(out, R"("id":116,"code":-32002)"), "17th queued call is refused", out.empty() ? "" : out[0]);
    g_mainCalls = 0;
    oc::Pump();
    out = c.Take();
    Expect(g_mainCalls >= 1 && out.size() == static_cast<size_t>(g_mainCalls.load()) && out[0] == R"({"t":"res","id":100,"r":{"i":0}})", "Pump runs main-thread calls in order");
    for (int i = 0; i < 20 && g_mainCalls < 16; ++i) oc::Pump();
    c.Take();
    Expect(g_mainCalls == 16, "all queued calls ran", std::to_string(g_mainCalls.load()));

    oc::router::Text(c.id, R"({"t":"sub","ch":"t.log","filter":{"minLevel":"info"},"id":30})");
    oc::router::Text(c.id, R"({"t":"sub","ch":"t.state","id":31})");
    out = c.Take();
    Expect(out.size() == 2 && out[0] == R"({"t":"res","id":30,"r":true})", "sub acknowledged");
    Expect(g_subLog.size() == 1 && g_subLog[0] == std::to_string(c.id) + "+{\"minLevel\":\"info\"}", "server-thread subscribe callback with filter",
           g_subLog.empty() ? "" : g_subLog[0]);
    oc::Pump();
    Expect(g_subLog.size() == 2 && g_subLog[1] == std::to_string(c.id) + "+{}", "main-thread subscribe callback runs in Pump");
    Expect(oa::HasSubscribers(log) && oa::HasSubscribers(st), "HasSubscribers");
    Expect(oa::Publish(log, "{\"n\":1}") && oa::Publish(log, "{\"n\":2}") && oa::PublishTo(st, c.id, "{\"s\":1}") && !oa::PublishTo(st, 999, "1"),
           "publish");
    out = c.Take(nullptr, GetTickCount() + 100000);
    Expect(Has(out, R"({"t":"ev","ch":"t.log","seq":1,"b":[{"n":1},{"n":2}]})") && Has(out, R"({"t":"ev","ch":"t.state","seq":1,"d":{"s":1}})"),
           "events delivered", out.empty() ? "" : out[0]);

    cfg.readOnly = true;
    oc::router::Configure(cfg);
    oc::router::Text(c.id, R"({"t":"call","id":40,"m":"t.write"})");
    out = c.Take();
    Expect(out.size() == 1 && Has(out, R"("id":40,"code":-32003)"), "read-only refuses mutating methods");
    cfg.readOnly = false;
    oc::router::Configure(cfg);

    FakeClient d, e;
    d.id = oc::router::Open(d.ev);
    oc::router::Text(d.id, R"({"t":"hello","proto":1})");
    e.id = oc::router::Open(e.ev);
    e.Take(&code);
    Expect(code == oc::kCloseTooMany, "third client over maxClients=2 closes 4029");
    oc::router::Text(c.id, R"({"t":"unsub","ch":"t.log","id":50})");
    Expect(!oa::HasSubscribers(log), "unsubscribe");
    oc::router::Gone(c.id);
    oc::Pump();
    Expect(!oa::HasSubscribers(st) && g_subLog.back() == std::to_string(c.id) + "-{}", "disconnect drops subscriptions");
    Expect(!oa::Publish(st, "1"), "publish with nobody subscribed");
    oa::RemoveChannel(log);
    Expect(!oa::HasSubscribers(log) && oa::AddChannel("t.log") != 0, "channel removed and re-added");
    for (FakeClient* f : {&c, &d, &e}) {
        oc::router::Gone(f->id);
        oc::router::Release(f->id);
    }
    Expect(oa::Clients() == 0, "no clients left");
}

// ---------------------------------------------------------------- live server
int g_port = 0;
std::string g_token, g_cookie;

SOCKET Connect() {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(g_port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    DWORD tmo = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
    if (connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

std::string RecvSome(SOCKET s, size_t want = 0) {
    std::string out;
    char buf[8192];
    for (;;) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (want && out.size() >= want) break;
        if (!want && out.find("\r\n\r\n") != std::string::npos) {
            const size_t cl = out.find("Content-Length: ");
            const size_t end = out.find("\r\n\r\n") + 4;
            if (cl == std::string::npos || cl > end) break;
            if (out.size() >= end + static_cast<size_t>(atoll(out.c_str() + cl + 16))) break;
        }
    }
    return out;
}

// One request on a fresh connection; returns the whole response (head and body).
std::string Http(const std::string& raw) {
    SOCKET s = Connect();
    if (s == INVALID_SOCKET) return {};
    send(s, raw.data(), static_cast<int>(raw.size()), 0);
    std::string r = RecvSome(s);
    closesocket(s);
    return r;
}

int Status(const std::string& r) { return r.size() > 12 && r.starts_with("HTTP/1.") ? atoi(r.c_str() + 9) : 0; }

std::string Get(const std::string& target, const std::string& extra = "") {
    return Http("GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(g_port) + "\r\n" + extra + "\r\n");
}

struct WsClient {
    SOCKET s = INVALID_SOCKET;
    std::string buf;
    bool Open(const std::string& extra = "") {
        s = Connect();
        const std::string rq = "GET /ws?k=" + g_token + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(g_port) +
                               "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n" + extra + "\r\n";
        send(s, rq.data(), static_cast<int>(rq.size()), 0);
        buf = RecvSome(s);
        const size_t end = buf.find("\r\n\r\n");
        head = buf.substr(0, end);
        buf.erase(0, end == std::string::npos ? buf.size() : end + 4);
        return Status(head) == 101;
    }
    void Frame(uint8_t first, const std::string& payload, bool mask = true, uint64_t fakeLen = 0) {
        std::string f;
        f += static_cast<char>(first);
        const uint64_t n = fakeLen ? fakeLen : payload.size();
        const uint8_t m = mask ? 0x80 : 0;
        if (n < 126) f += static_cast<char>(m | n);
        else if (n < 65536) { f += static_cast<char>(m | 126); f += static_cast<char>(n >> 8); f += static_cast<char>(n & 255); }
        else { f += static_cast<char>(m | 127); for (int i = 7; i >= 0; --i) f += static_cast<char>(n >> (8 * i) & 255); }
        const char key[4] = {0x12, 0x34, 0x56, 0x78};
        if (mask) f.append(key, 4);
        for (size_t i = 0; i < payload.size(); ++i) f += mask ? static_cast<char>(payload[i] ^ key[i & 3]) : payload[i];
        send(s, f.data(), static_cast<int>(f.size()), 0);
    }
    void Text(const std::string& t) { Frame(0x81, t); }
    // Next server frame: opcode, payload. -1 when the connection closed or timed out.
    int Read(std::string* payload) {
        for (;;) {
            if (buf.size() >= 2) {
                const auto* b = reinterpret_cast<const unsigned char*>(buf.data());
                uint64_t len = b[1] & 0x7f;
                size_t hp = 2;
                if (len == 126) { if (buf.size() < 4) goto more; len = static_cast<uint64_t>(b[2]) << 8 | b[3]; hp = 4; }
                else if (len == 127) { if (buf.size() < 10) goto more; len = 0; for (int i = 0; i < 8; ++i) len = len << 8 | b[2 + i]; hp = 10; }
                if (buf.size() >= hp + len) {
                    const int op = b[0] & 0x0f;
                    *payload = buf.substr(hp, static_cast<size_t>(len));
                    buf.erase(0, hp + static_cast<size_t>(len));
                    return op;
                }
            }
        more:
            char tmp[65536];
            const int n = recv(s, tmp, sizeof tmp, 0);
            if (n <= 0) return -1;
            buf.append(tmp, static_cast<size_t>(n));
        }
    }
    int ReadText(std::string* p) {
        for (;;) {
            const int op = Read(p);
            if (op != 9 && op != 10) return op;
        }
    }
    uint16_t CloseCode() {
        std::string p;
        for (int i = 0; i < 20; ++i) {
            const int op = Read(&p);
            if (op == 8) return p.size() >= 2 ? static_cast<uint16_t>(static_cast<unsigned char>(p[0]) << 8 | static_cast<unsigned char>(p[1])) : 1005;
            if (op < 0) return 0;
        }
        return 0;
    }
    bool Hello() {
        Text(R"({"t":"hello","proto":1,"build":"t","client":"selftest"})");
        std::string p;
        return ReadText(&p) == 1 && p.find("\"welcome\"") != std::string::npos;
    }
    ~WsClient() {
        if (s != INVALID_SOCKET) closesocket(s);
    }
    std::string head;
};

std::string MakeZip() {
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    const std::string index = "<!doctype html><html><body>selftest</body></html>";
    const std::string js = std::string(3000, 'a');
    mz_zip_writer_add_mem(&z, "index.html", index.data(), index.size(), 9);
    mz_zip_writer_add_mem(&z, "app/main.js", js.data(), js.size(), 9);
    mz_zip_writer_add_mem(&z, "app/main.js.gz", "\x1f\x8bGZ", 4, 0);
    mz_zip_writer_add_mem(&z, "build.txt", "zipbuild\n", 9, 0);
    void* p = nullptr;
    size_t n = 0;
    mz_zip_writer_finalize_heap_archive(&z, &p, &n);
    std::string s(static_cast<const char*>(p), n);
    mz_zip_writer_end(&z);
    return s;
}

bool NoCors(const std::string& r) { return r.find("Access-Control") == std::string::npos; }

void TestLive(oc::Files* files) {
    oc::Config cfg;
    cfg.port = 28765;
    cfg.portRange = 20;
    cfg.maxClients = 4;
    // Hold the first port so the server has to walk to the next.
    SOCKET holder = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in ha{};
    ha.sin_family = AF_INET;
    ha.sin_port = htons(static_cast<u_short>(cfg.port));
    ha.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool held = bind(holder, reinterpret_cast<sockaddr*>(&ha), sizeof ha) == 0 && listen(holder, 1) == 0;
    Expect(oc::Start(cfg, oa::providers::MakeAuth(), files), "server starts");
    g_port = oc::Port();
    Expect(!held || g_port == cfg.port + 1, "port clash walks to the next port", std::to_string(g_port));
    closesocket(holder);
    const std::string url = oc::LaunchUrl();
    g_token = url.substr(url.find("k=") + 2);
    Expect(g_token.size() == 22, "128-bit base64url token", g_token);

    const std::string host = "127.0.0.1:" + std::to_string(g_port);
    std::string r = Get("/?k=" + g_token);
    Expect(Status(r) == 303 && r.find("\r\nLocation: /\r\n") != std::string::npos && r.find("HttpOnly; SameSite=Strict; Path=/") != std::string::npos,
           "token exchanged for a cookie", r.substr(0, 80));
    // A: the cookie is named per port (oasis_s_<port>), since 127.0.0.1 has no per-port cookie jar.
    const std::string cookiePrefix = "oasis_s_" + std::to_string(g_port) + "=";
    const size_t cp = r.find(cookiePrefix);
    g_cookie = cp == std::string::npos ? "" : r.substr(cp, r.find(';', cp) - cp);
    const std::string ck = "Cookie: " + g_cookie + "\r\n";
    r = Get("/", ck);
    Expect(Status(r) == 200 && r.find("selftest</body>") != std::string::npos && r.find("ETag: \"") != std::string::npos &&
               r.find("Cache-Control: no-cache") != std::string::npos,
           "page by cookie", r.substr(0, 60));
    const size_t ep = r.find("ETag: ");
    const std::string etag = r.substr(ep + 6, r.find("\r\n", ep) - ep - 6);
    Expect(Status(Get("/", ck + "If-None-Match: " + etag + "\r\n")) == 304, "conditional GET");
    r = Http("HEAD / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "\r\n");
    Expect(Status(r) == 200 && r.find("</body>") == std::string::npos, "HEAD has no body");
    r = Get("/app/main.js", ck + "Accept-Encoding: gzip, br\r\n");
    Expect(Status(r) == 200 && r.find("Content-Encoding: gzip") != std::string::npos && r.find("\x1f\x8bGZ") != std::string::npos,
           "precompressed .gz served as-is");
    r = Get("/app/main.js", ck);
    Expect(Status(r) == 200 && r.find("Content-Encoding") == std::string::npos && r.find("Content-Length: 3000") != std::string::npos, "plain when gzip is not accepted");
    Expect(Status(Get("/app/nope.js", ck)) == 404 && Status(Get("/app/../build.txt", ck)) == 404 && Status(Get("/build.txt", ck)) == 404,
           "unknown and traversal paths are 404");

    const std::vector<std::pair<std::string, int>> neg = {
        {"GET / HTTP/1.1\r\nHost: " + host + "\r\n\r\n", 403},
        {"GET /?k=AAAAAAAAAAAAAAAAAAAAAA HTTP/1.1\r\nHost: " + host + "\r\n\r\n", 403},
        {"GET /?k=" + g_token + " HTTP/1.1\r\nHost: evil.example\r\n\r\n", 403},
        {"GET /?k=" + g_token + " HTTP/1.1\r\nHost: attacker.com:" + std::to_string(g_port) + "\r\n\r\n", 403},
        {"GET /?k=" + g_token + " HTTP/1.1\r\nHost: rebind.test:" + std::to_string(g_port) + "\r\n\r\n", 403},
        {"GET / HTTP/1.1\r\nHost: " + host + "\r\nCookie: oasis_s=AAAAAAAAAAAAAAAAAAAAAA\r\n\r\n", 403},
        {"GET /ws?k=" + g_token + " HTTP/1.1\r\nHost: " + host + "\r\nOrigin: http://evil.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 403},
        {"GET /ws?k=AAAAAAAAAAAAAAAAAAAAAA HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 403},
        {"GET /ws HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n", 403},
        {"POST / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "Content-Length: 3\r\n\r\nabc", 405},
        {"GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: " + host + "\r\nHost: " + host + "\r\n" + ck + "\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "X-A: 1\r\n folded\r\n\r\n", 400},
        {"GET / HTTP/1.1\r\nHost : " + host + "\r\n" + ck + "\r\n", 400},
        {"GET /\xc3\xa9 HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "\r\n", 400},
        {"GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "X: a" + std::string(1, '\0') + "b\r\n\r\n", 400},
        {"GET /ws HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "\r\n", 400},
    };
    int i = 0;
    for (const auto& [raw, want] : neg) {
        r = Http(raw);
        Expect(Status(r) == want && NoCors(r), ("negative case " + std::to_string(i++)).c_str(), r.substr(0, 40));
    }
    std::string many = "GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck;
    for (int k = 0; k < 70; ++k) many += "X-" + std::to_string(k) + ": 1\r\n";
    Expect(Status(Http(many + "\r\n")) >= 400, "70 headers refused");
    Expect(Status(Http("GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "X: " + std::string(20000, 'a') + "\r\n\r\n")) >= 400 ||
               Http("GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "X: " + std::string(20000, 'a') + "\r\n\r\n").empty(),
           "oversized headers refused");
    {
        SOCKET s = Connect();
        const std::string half = "GET / HTTP/1.1\r\nHost: " + host;
        send(s, half.data(), static_cast<int>(half.size()), 0);
        closesocket(s);
    }
    Expect(Status(Get("/", ck)) == 200, "truncated request does not hurt the server");
    {
        SOCKET s = Connect();
        const std::string two = "GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "\r\nGET /app/main.js HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "\r\n";
        send(s, two.data(), static_cast<int>(two.size()), 0);
        const std::string both = RecvSome(s, 1);
        std::string all = both;
        for (int k = 0; k < 10 && all.find("HTTP/1.1 200", all.find("HTTP/1.1 200") + 1) == std::string::npos; ++k) all += RecvSome(s, 1);
        closesocket(s);
        Expect(all.find("HTTP/1.1 200", all.find("HTTP/1.1 200") + 1) != std::string::npos, "keep-alive serves two requests");
    }

    {
        WsClient w;
        Expect(w.Open("Origin: http://localhost:" + std::to_string(g_port) + "\r\n") &&
                   w.head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos && NoCors(w.head),
               "upgrade with the RFC 6455 example key", w.head.substr(0, 40));
        Expect(w.Hello(), "hello/welcome over the socket");
        w.Frame(0x01, R"({"t":"call",)");
        w.Frame(0x89, "ping!");
        w.Frame(0x00, R"("id":5,"m":)");
        w.Frame(0x80, R"("sys.ping"})");
        std::string p;
        int op = w.Read(&p);
        Expect(op == 10 && p == "ping!", "ping answered with pong", std::to_string(op));
        Expect(w.ReadText(&p) == 1 && p.starts_with(R"({"t":"res","id":5,"r":{"frame":)"), "fragmented call reassembled", p);
        w.Text("\xc3\x28");
        Expect(w.CloseCode() == 1007, "bad UTF-8 closes with 1007");
    }
    {
        WsClient w;
        w.Open();
        w.Hello();
        w.Frame(0x81, R"({"t":"call","id":1,"m":"sys.ping"})", false);
        std::string p;
        Expect(w.Read(&p) < 0, "unmasked client frame drops the connection");
    }
    {
        WsClient w;
        w.Open();
        w.Hello();
        w.Frame(0x81, "", true, 1ull << 40);
        std::string p;
        Expect(w.Read(&p) < 0, "64-bit length frame dropped without allocating");
    }
    {
        WsClient w;
        w.Open();
        w.Hello();
        w.Frame(0x89, std::string(126, 'p'));
        Expect(w.CloseCode() == 1002 || w.buf.empty(), "control frame over 125 bytes");
    }
    {
        WsClient w;
        w.Open();
        w.Hello();
        w.Frame(0x82, "bin");
        Expect(w.CloseCode() == 1003, "binary message refused with 1003");
    }
    {
        WsClient w;
        w.Open();
        w.Hello();
        w.Frame(0xc1, "x");
        Expect(w.CloseCode() == 1002, "RSV bit refused with 1002");
    }
    {
        WsClient w;
        w.Open();
        w.Hello();
        std::string big = R"({"t":"call","id":1,"m":"sys.ping","p":{"x":")" + std::string(700000, 'a') + "\"}}";
        w.Frame(0x01, big.substr(0, 600000));
        w.Frame(0x80, big.substr(600000) + std::string(500000, 'b'));
        Expect(w.CloseCode() == 1009, "message over 1 MB refused with 1009");
    }
    {
        std::vector<std::unique_ptr<WsClient>> ws;
        for (int k = 0; k < 4; ++k) {
            ws.push_back(std::make_unique<WsClient>());
            ws.back()->Open();
            ws.back()->Hello();
        }
        WsClient fifth;
        fifth.Open();
        Expect(fifth.CloseCode() == 4029, "fifth client closed with 4029");
        oc::Kick(0);
        Expect(ws[0]->CloseCode() == 4000, "kick closes with 4000");
    }
    Sleep(300);
    WsClient after;
    Expect(after.Open() && after.Hello(), "server still answers after the corpus");
}

// ---------------------------------------------------------------- mutation run
std::vector<std::string> Corpus() {
    const std::string host = "127.0.0.1:" + std::to_string(g_port);
    const std::string ck = "Cookie: " + g_cookie + "\r\n";
    return {
        "GET / HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "\r\n",
        "GET /app/main.js HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "Accept-Encoding: gzip\r\nIf-None-Match: \"x\"\r\n\r\n",
        "GET /?k=" + g_token + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n",
        "HEAD /index.html HTTP/1.1\r\nHost: " + host + "\r\n" + ck + "Range: bytes=0-10\r\n\r\n",
        "GET /ws?k=" + g_token + " HTTP/1.1\r\nHost: " + host + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n",
    };
}

std::string Masked(uint8_t first, const std::string& p) {
    std::string f;
    f += static_cast<char>(first);
    if (p.size() < 126) f += static_cast<char>(0x80 | p.size());
    else { f += static_cast<char>(0x80 | 126); f += static_cast<char>(p.size() >> 8); f += static_cast<char>(p.size() & 255); }
    f += "\x01\x02\x03\x04";
    for (size_t i = 0; i < p.size(); ++i) f += static_cast<char>(p[i] ^ "\x01\x02\x03\x04"[i & 3]);
    return f;
}

std::string Mutate(std::string s, std::mt19937& rng) {
    const int n = 1 + static_cast<int>(rng() % 6);
    for (int k = 0; k < n && !s.empty(); ++k) {
        const size_t at = rng() % s.size();
        switch (rng() % 7) {
            case 0: s[at] = static_cast<char>(rng()); break;
            case 1: s[at] ^= static_cast<char>(1 << (rng() % 8)); break;
            case 2: s.insert(at, 1, static_cast<char>(rng())); break;
            case 3: s.erase(at, 1 + rng() % 8); break;
            case 4: s.resize(at); break;
            case 5: s.insert(at, s.substr(at, rng() % 64)); break;
            case 6: s.insert(at, std::string(rng() % 3000, static_cast<char>("a \r\n:\0\x80"[rng() % 7]))); break;
        }
    }
    return s;
}

uint64_t PrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    return pm.PrivateUsage;
}

void MutationRun(int seconds) {
    const auto corpus = Corpus();
    const std::string host = "127.0.0.1:" + std::to_string(g_port);
    const std::vector<std::string> frames = {
        Masked(0x81, R"({"t":"hello","proto":1})"), Masked(0x81, R"({"t":"call","id":1,"m":"sys.ping","p":{}})"),
        Masked(0x81, R"({"t":"sub","ch":"t.log","filter":{"a":[1,{"b":null}]},"id":2})"), Masked(0x01, R"({"t":"ca)"),
        Masked(0x80, R"(ll","id":3,"m":"sys.ping"})"), Masked(0x89, "ping"), Masked(0x88, "\x03\xe8")};
    std::atomic<uint64_t> sent{0};
    std::atomic<bool> stop{false};
    auto worker = [&](unsigned seed) {
        std::mt19937 rng(seed);
        while (!stop) {
            SOCKET s = Connect();
            if (s == INVALID_SOCKET) continue;
            DWORD tmo = 60;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof tmo);
            std::string msg;
            if (rng() % 3 == 0) {
                msg = corpus.back();
                for (int k = 0; k < 1 + static_cast<int>(rng() % 5); ++k) msg += frames[rng() % frames.size()];
                const size_t from = corpus.back().size();
                msg = msg.substr(0, from) + Mutate(msg.substr(from), rng);
            } else {
                msg = Mutate(corpus[rng() % corpus.size()], rng);
            }
            send(s, msg.data(), static_cast<int>(msg.size()), 0);
            char buf[4096];
            for (int k = 0; k < 4 && recv(s, buf, sizeof buf, 0) > 0; ++k) {
            }
            closesocket(s);
            ++sent;
        }
    };
    std::vector<std::thread> ts;
    for (unsigned t = 0; t < 8; ++t) ts.emplace_back(worker, 1234u + t);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    stop = true;
    for (auto& t : ts) t.join();
    printf("mutation run: %llu mutated inputs in %d s\n", static_cast<unsigned long long>(sent.load()), seconds);
    Sleep(500);
    Expect(Status(Get("/", "Cookie: " + g_cookie + "\r\n")) == 200, "page still served after the mutation run");
    WsClient w;
    Expect(w.Open() && w.Hello(), "WebSocket still works after the mutation run");
}
}  // namespace

int main(int argc, char** argv) {
    int mutate = 0;
    for (int i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--mutate") && i + 1 < argc) mutate = atoi(argv[++i]);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);

    TestHttp();
    TestWs();
    TestOutbox();
    TestRouter();

    const std::string zip = MakeZip();
    auto files = oc::ZipFiles(zip.data(), zip.size());
    Expect(files != nullptr && oc::ZipEntryText(zip.data(), zip.size(), "build.txt") == "zipbuild", "zip bundle");
    Sleep(200);
    DWORD h0 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h0);
    const uint64_t m0 = PrivateBytes();
#if defined(_DEBUG)
    _CrtMemState s0, s1, sd;
    _CrtMemCheckpoint(&s0);
#endif
    TestLive(files.get());
    if (mutate > 0) MutationRun(mutate);
    oc::Stop();
    Expect(!oc::Running() && oc::Port() == 0, "server stopped");
    Sleep(300);
#if defined(_DEBUG)
    _CrtMemCheckpoint(&s1);
    if (_CrtMemDifference(&sd, &s0, &s1) && sd.lCounts[_NORMAL_BLOCK] > 64) {
        _CrtMemDumpStatistics(&sd);
        Expect(false, "CRT debug heap: blocks left after Stop()", std::to_string(sd.lCounts[_NORMAL_BLOCK]));
    } else {
        Expect(true, "CRT debug heap");
    }
#endif
    DWORD h1 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h1);
    const uint64_t m1 = PrivateBytes();
    Expect(h1 <= h0 + 8, "handles back after Stop()", std::to_string(h0) + " -> " + std::to_string(h1));
    Expect(m1 <= m0 + (4u << 20), "private bytes back after Stop()", std::to_string(m0 / 1024) + " KB -> " + std::to_string(m1 / 1024) + " KB");

    printf("oasis_core_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
