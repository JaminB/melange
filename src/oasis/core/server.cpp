#include "oasis/core/server.h"

#include <winsock2.h>
#include <windows.h>

#include <civetweb.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "core/log.h"
#include "oasis/core/router.h"
#include "oasis/core/ws.h"

extern "C" void mg_oasis_close_after(struct mg_connection* conn);
extern "C" void mg_oasis_abort(struct mg_connection* conn);

namespace melange::oasis::core {
namespace {
// Held by shared_ptr so a request already dispatched to a route can keep it (and the `user` it points to)
// alive after RemoveRoute has taken it out of g_routes; `busy` lets RemoveRoute wait for that request to
// finish before its caller frees `user` (RemovePanel deletes the Panel right after RemoveRoute returns).
struct RouteEntry {
    int handle;
    std::string prefix;
    Route fn;
    void* user;
    std::atomic<int> busy{0};
};

std::mutex g_startMx;  // Start/Stop
mg_context* g_ctx = nullptr;
Auth* g_auth = nullptr;
Files* g_files = nullptr;
Config g_cfg;
std::atomic<int> g_port{0};
std::atomic<bool> g_running{false};
std::mutex g_urlMx;
std::string g_url;
std::shared_mutex g_routesMx;
std::vector<std::shared_ptr<RouteEntry>> g_routes;
int g_nextRoute = 1;

struct WsConn {
    mg_connection* conn = nullptr;
    HANDLE wake = nullptr, thread = nullptr;
    int id = 0;
    std::atomic<bool> closeSent{false};
    ws::Assembler assembler;
    explicit WsConn(size_t maxMessage) : assembler(maxMessage) {}
};

int LogMessage(const mg_connection*, const char* msg) {
    static std::atomic<ULONGLONG> windowStart{0};
    static std::atomic<int> inWindow{0};
    const ULONGLONG now = GetTickCount64();
    if (now - windowStart.load() > 60000) {
        windowStart = now;
        inWindow = 0;
    }
    if (++inWindow <= 20) LOG_WARN("[oasis] server: %s", msg);
    return 1;
}

Request BuildRequest(const mg_request_info* ri) {
    Request rq;
    rq.method = ri->request_method ? ri->request_method : "";
    rq.path = ri->local_uri_raw ? ri->local_uri_raw : "";
    rq.query = ri->query_string ? ri->query_string : "";
    rq.target = rq.query.empty() ? rq.path : rq.path + "?" + rq.query;
    for (int i = 0; i < ri->num_headers; ++i)
        rq.headers.emplace_back(ri->http_headers[i].name ? ri->http_headers[i].name : "",
                                ri->http_headers[i].value ? ri->http_headers[i].value : "");
    rq.port = g_port.load();
    rq.upgrade = IEquals(rq.Header("upgrade"), "websocket");
    return rq;
}

bool KeepAlive(const Request& rq) {
    const std::string_view c = rq.Header("connection");
    return !IEquals(c, "close");
}

void WriteAll(mg_connection* conn, const std::string& s) {
    if (!s.empty()) mg_write(conn, s.data(), s.size());
}

int Reply(mg_connection* conn, const Request& rq, Response& r) {
    const bool head = rq.method == "HEAD";
    const bool keep = KeepAlive(rq) && !r.close;
    if (!r.file.empty()) {
        HANDLE h = CreateFileW(r.file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        LARGE_INTEGER size{};
        if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &size)) {
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            Response nf;
            nf.status = 404;
            nf.body = "Not Found\n";
            return Reply(conn, rq, nf);
        }
        const uint64_t total = static_cast<uint64_t>(size.QuadPart);
        uint64_t first = 0, last = total ? total - 1 : 0;
        r.headers.emplace_back("Accept-Ranges", "bytes");
        if (auto range = rq.Header("range"); !range.empty() && total) {
            if (!ParseRange(range, total, &first, &last)) {
                CloseHandle(h);
                Response bad;
                bad.status = 416;
                bad.headers.emplace_back("Content-Range", "bytes */" + std::to_string(total));
                return Reply(conn, rq, bad);
            }
            r.status = 206;
            r.headers.emplace_back("Content-Range", "bytes " + std::to_string(first) + "-" + std::to_string(last) + "/" +
                                                        std::to_string(total));
        }
        const uint64_t len = total ? last - first + 1 : 0;
        WriteAll(conn, Head(r, len, keep));
        if (!head && len) {
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(first);
            SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
            std::vector<char> buf(64 * 1024);
            uint64_t left = len;
            while (left) {
                DWORD got = 0;
                const DWORD want = static_cast<DWORD>(left < buf.size() ? left : buf.size());
                if (!ReadFile(h, buf.data(), want, &got, nullptr) || !got) break;
                if (mg_write(conn, buf.data(), got) <= 0) break;
                left -= got;
            }
            if (left) mg_oasis_abort(conn);
        }
        CloseHandle(h);
    } else {
        WriteAll(conn, Head(r, r.body.size(), keep));
        if (!head) WriteAll(conn, r.body);
    }
    if (!keep) mg_oasis_close_after(conn);
    return r.status;
}

int Plain(mg_connection* conn, const Request& rq, int status, bool close = false) {
    Response r;
    r.status = status;
    r.body = std::string(StatusText(status)) + "\n";
    r.close = close || status == 400 || status == 431;
    return Reply(conn, rq, r);
}

int ServeStatic(mg_connection* conn, const Request& rq, const std::string& file) {
    if (!g_files || !SafePath(file)) return Plain(conn, rq, 404);
    Response r;
    std::string etag;
    bool gz = rq.Header("accept-encoding").find("gzip") != std::string_view::npos;
    if (!g_files->Get(file, &r.body, &etag, &gz)) return Plain(conn, rq, 404);
    r.contentType = MimeType(file);
    r.cacheable = true;
    r.headers.emplace_back("ETag", etag);
    r.headers.emplace_back("Vary", "Accept-Encoding");
    if (gz) r.headers.emplace_back("Content-Encoding", "gzip");
    if (auto inm = rq.Header("if-none-match"); !inm.empty() && inm == etag) {
        r.status = 304;
        r.body.clear();
    }
    return Reply(conn, rq, r);
}

int BeginRequest(mg_connection* conn) {
    const Request rq = BuildRequest(mg_get_request_info(conn));
    if (const int st = Validate(rq)) return Plain(conn, rq, st, true);
    Response deny;
    if (!g_auth || !g_auth->CheckHttp(rq, &deny)) {
        if (deny.status == 200) deny.status = 403;
        if (deny.status >= 400 && deny.body.empty()) deny.body = std::string(StatusText(deny.status)) + "\n";
        return Reply(conn, rq, deny);
    }
    if (rq.path == "/ws") {
        if (!rq.upgrade || rq.method != "GET") return Plain(conn, rq, 400, true);
        if (!g_auth->CheckUpgrade(rq)) {
            router::CountAuthFailure();
            return Plain(conn, rq, 403, true);
        }
        return 0;  // civetweb performs the handshake
    }
    if (rq.upgrade) return Plain(conn, rq, 400, true);
    if (rq.path == "/" || rq.path == "/index.html") return ServeStatic(conn, rq, "index.html");
    if (rq.path.starts_with("/app/")) return ServeStatic(conn, rq, rq.path.substr(1));
    std::shared_ptr<RouteEntry> route;
    {
        std::shared_lock lk(g_routesMx);
        for (const auto& e : g_routes)
            if (rq.path.starts_with(e->prefix) && (!route || e->prefix.size() > route->prefix.size())) route = e;
    }
    if (route) {
        route->busy.fetch_add(1, std::memory_order_acq_rel);
        Response r;
        const bool matched = route->fn(rq, &r, route->user);
        route->busy.fetch_sub(1, std::memory_order_acq_rel);
        if (matched) return Reply(conn, rq, r);
    }
    return Plain(conn, rq, 404);
}

DWORD WINAPI Writer(void* p) {
    auto* w = static_cast<WsConn*>(p);
    std::vector<std::string> msgs;
    std::vector<uint8_t> binary;
    std::string reason;
    for (;;) {
        msgs.clear();
        binary.clear();
        uint32_t wait = UINT32_MAX;
        uint16_t code = 0;
        if (!router::Take(w->id, GetTickCount(), &msgs, &wait, &code, &reason, &binary)) break;
        uint64_t bytes = 0;
        bool failed = false;
        for (size_t i = 0; i < msgs.size(); ++i) {
            const auto& m = msgs[i];
            const int op = i < binary.size() && binary[i] ? MG_WEBSOCKET_OPCODE_BINARY : MG_WEBSOCKET_OPCODE_TEXT;
            if (mg_websocket_write(w->conn, op, m.data(), m.size()) <= 0) {
                failed = true;
                break;
            }
            bytes += m.size();
        }
        router::CountIo(msgs.size(), bytes, 0);
        if (failed) {
            mg_oasis_abort(w->conn);
            break;
        }
        if (code) {
            const std::string payload = ws::ClosePayload(code, reason);
            if (!w->closeSent.exchange(true))
                mg_websocket_write(w->conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, payload.data(), payload.size());
            // The peer answers with its own close, which ends civetweb's read loop; if it does not, cut it.
            const ULONGLONG until = GetTickCount64() + 2000;
            while (GetTickCount64() < until && router::Take(w->id, GetTickCount(), &msgs, &wait, &code, &reason)) {
                msgs.clear();
                WaitForSingleObject(w->wake, 100);
            }
            mg_oasis_abort(w->conn);
            break;
        }
        WaitForSingleObject(w->wake, wait == UINT32_MAX ? INFINITE : wait);
    }
    return 0;
}

int WsConnect(const mg_connection*, void*) { return 0; }

void WsReady(mg_connection* conn, void*) {
    auto* w = new WsConn(static_cast<size_t>(g_cfg.maxMessageKB) * 1024);
    w->conn = conn;
    w->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    w->id = router::Open(w->wake);
    mg_set_user_connection_data(conn, w);
    w->thread = CreateThread(nullptr, 64 * 1024, &Writer, w, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!w->thread) mg_oasis_abort(conn);
}

int WsData(mg_connection* conn, int bits, char* data, size_t len, void*) {
    auto* w = static_cast<WsConn*>(mg_get_user_connection_data(conn));
    if (!w) return 0;
    router::CountIo(0, 0, len);
    switch (w->assembler.Feed(static_cast<uint8_t>(bits), data, len)) {
        case ws::Assembler::Out::Message: router::Text(w->id, w->assembler.message()); return 1;
        case ws::Assembler::Out::Close: {
            const std::string p = ws::ClosePayload(w->assembler.closeCode(), "");
            if (!w->closeSent.exchange(true)) mg_websocket_write(conn, MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE, p.data(), p.size());
            return 0;
        }
        default: return 1;
    }
}

void WsClose(const mg_connection* cconn, void*) {
    auto* conn = const_cast<mg_connection*>(cconn);
    auto* w = static_cast<WsConn*>(mg_get_user_connection_data(conn));
    if (!w) return;
    router::Gone(w->id);
    if (w->thread) {
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
    }
    router::Release(w->id);
    CloseHandle(w->wake);
    mg_set_user_connection_data(conn, nullptr);
    delete w;
}

const char kAdditionalHeaders[] =
    "Content-Security-Policy: default-src 'self'; connect-src 'self'; img-src 'self' blob: data:; style-src 'self' "
    "'unsafe-inline'; frame-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
    "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer";
}  // namespace

bool Start(const Config& c, Auth* a, Files* f) {
    std::lock_guard lk(g_startMx);
    if (g_ctx) return true;
    g_cfg = c;
    g_auth = a;
    g_files = f;
    router::Configure(c);
    mg_init_library(0);
    const std::string threads = std::to_string(c.maxClients + 6);
    const std::string maxReq = std::to_string(c.maxHeaderKB * 1024);
    mg_callbacks cb{};
    cb.begin_request = &BeginRequest;
    cb.log_message = &LogMessage;
    const int range = c.portRange < 1 ? 1 : c.portRange;
    for (int i = 0; i < range; ++i) {
        const int port = c.port + i;
        const std::string listen = "127.0.0.1:" + std::to_string(port);
        const char* opts[] = {
            "listening_ports", listen.c_str(),
            "num_threads", threads.c_str(),
            "connection_queue", "32",
            "listen_backlog", "32",
            "max_request_size", maxReq.c_str(),
            "enable_keep_alive", "yes",
            "keep_alive_timeout_ms", "2000",
            "request_timeout_ms", "10000",
            "websocket_timeout_ms", "20000",
            "enable_websocket_ping_pong", "yes",
            "decode_url", "no",
            "tcp_nodelay", "1",
            "access_control_allow_origin", "",
            "access_control_allow_methods", "",
            "access_control_allow_headers", "",
            "enable_auth_domain_check", "no",
            "additional_header", kAdditionalHeaders,
            nullptr, nullptr,
        };
        g_port = port;
        mg_init_data init{};
        init.callbacks = &cb;
        init.configuration_options = opts;
        char err[256] = {};
        mg_error_data ed{};
        ed.text = err;
        ed.text_buffer_size = sizeof err;
        mg_context* ctx = mg_start2(&init, &ed);
        if (ctx) {
            mg_set_websocket_handler(ctx, "/ws", &WsConnect, &WsReady, &WsData, &WsClose, nullptr);
            g_ctx = ctx;
            {
                std::lock_guard ul(g_urlMx);
                g_url = a ? a->LaunchUrl(port) : "";
            }
            g_running = true;
            LOG_INFO("[oasis] listening on 127.0.0.1:%d (%s threads)", port, threads.c_str());
            return true;
        }
        LOG_WARN("[oasis] port %d unavailable (%s)%s", port, err, i + 1 < range ? ", trying the next" : "");
    }
    g_port = 0;
    LOG_ERROR("[oasis] no free port in %d..%d; server not started", c.port, c.port + range - 1);
    return false;
}

void Stop() {
    std::lock_guard lk(g_startMx);
    if (!g_ctx) return;
    g_running = false;
    router::CloseAll(1001, "server stopping");
    mg_stop(g_ctx);
    g_ctx = nullptr;
    g_port = 0;
    std::lock_guard ul(g_urlMx);
    g_url.clear();
    LOG_INFO("[oasis] server stopped");
}

int Port() { return g_running ? g_port.load() : 0; }
bool Running() { return g_running.load(); }

std::string LaunchUrl() {
    std::lock_guard lk(g_urlMx);
    return g_url;
}

int AddRoute(const char* prefix, Route fn, void* user) {
    if (!prefix || !fn || prefix[0] != '/') return 0;
    auto e = std::make_shared<RouteEntry>();
    e->handle = g_nextRoute;
    e->prefix = prefix;
    e->fn = fn;
    e->user = user;
    std::unique_lock lk(g_routesMx);
    g_routes.push_back(e);
    return g_nextRoute++;
}

void RemoveRoute(int handle) {
    std::shared_ptr<RouteEntry> removed;
    {
        std::unique_lock lk(g_routesMx);
        for (auto it = g_routes.begin(); it != g_routes.end(); ++it)
            if ((*it)->handle == handle) {
                removed = *it;
                g_routes.erase(it);
                break;
            }
    }
    // A request already dispatched to this route copied the shared_ptr and released g_routesMx before calling
    // fn(); wait for it so the caller can safely free `user` (e.g. RemovePanel deletes the Panel) right after.
    while (removed && removed->busy.load(std::memory_order_acquire) > 0) Sleep(1);
}

void NoteAuthFailure() { router::CountAuthFailure(); }

void Kick(int client) {
    if (client) router::Close(client, kCloseKicked, "kicked");
    else router::CloseAll(kCloseKicked, "kicked");
}
}  // namespace melange::oasis::core
