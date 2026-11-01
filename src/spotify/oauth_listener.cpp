// The sign-in's loopback listener (oauth_listener.h).
#include "spotify/oauth_listener.h"

#include <winsock2.h>
#include <windows.h>

#include <algorithm>

#include "core/log.h"
#include "spotify/spotify_logic.h"

namespace melange::spotify::oauth {
namespace {
bool WinsockReady() {
    static const bool ok = [] {
        WSADATA d{};
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

void SendAll(SOCKET c, const std::string& s) {
    size_t at = 0;
    while (at < s.size()) {
        const int n = send(c, s.data() + at, static_cast<int>(std::min<size_t>(s.size() - at, 65536)), 0);
        if (n <= 0) return;
        at += static_cast<size_t>(n);
    }
}

void Wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

// Reads one request head: until the blank line or kMaxCallbackHead bytes (5 s receive timeout).
std::string ReadHead(SOCKET c) {
    std::string head;
    char buf[1024];
    while (head.size() < logic::kMaxCallbackHead && head.find("\r\n\r\n") == std::string::npos) {
        const int want = static_cast<int>(std::min<size_t>(sizeof buf, logic::kMaxCallbackHead - head.size()));
        const int n = recv(c, buf, want, 0);
        if (n <= 0) break;
        head.append(buf, static_cast<size_t>(n));
    }
    SecureZeroMemory(buf, sizeof buf);
    return head;
}
// Closes an answered connection. The browser is let close first ("Connection: close"; at most 1 s), so the TIME_WAIT
// stays on its side: with SO_EXCLUSIVEADDRUSE a connection of ours in TIME_WAIT would keep the next sign-in from
// binding the port ("port-busy") for minutes.
void CloseAfterPeer(SOCKET c) {
    DWORD timeout = 1000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    char sink[512];
    const ULONGLONG until = GetTickCount64() + 1000;
    while (GetTickCount64() < until)
        if (recv(c, sink, sizeof sink, 0) <= 0) break;
    SecureZeroMemory(sink, sizeof sink);
    shutdown(c, SD_BOTH);
    closesocket(c);
}
// Closes a connection that never got an answer with a reset (SO_LINGER 0): nothing of ours is left in TIME_WAIT, which
// a graceful close of an idle spare connection would leave if the browser closes it after us (see CloseAfterPeer).
void CloseAbortive(SOCKET c) {
    const LINGER l{1, 0};
    setsockopt(c, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
    closesocket(c);
}
}  // namespace

bool Listener::Open(int port) {
    if (!WinsockReady()) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<u_short>(port));
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    u_long nonBlocking = 1;
    if (bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof a) == SOCKET_ERROR || listen(s, 4) == SOCKET_ERROR ||
        ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        LOG_WARN("[spotify] cannot listen on 127.0.0.1:%d for the sign-in (error %d); is [Spotify] RedirectPort in use?", port,
                 WSAGetLastError());
        closesocket(s);
        return false;
    }
    port_ = port;
    sock_.store(static_cast<uintptr_t>(s));
    return true;
}

void Listener::Close() {
    const SOCKET s = static_cast<SOCKET>(sock_.exchange(static_cast<uintptr_t>(INVALID_SOCKET)));
    if (s != INVALID_SOCKET) closesocket(s);
}

Listener::Outcome Listener::Serve(const std::string& state, uint64_t deadlineMs, const std::atomic<bool>& cancel, std::string* code) {
    for (;;) {
        if (cancel.load()) {
            Close();
            return Outcome::Cancelled;
        }
        if (GetTickCount64() >= deadlineMs) {
            Close();
            return Outcome::Timeout;
        }
        const SOCKET s = static_cast<SOCKET>(sock_.load());
        if (s == INVALID_SOCKET) return Outcome::Cancelled;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, 500000};
        const int ready = select(0, &rd, nullptr, nullptr, &tv);
        if (ready == SOCKET_ERROR) {
            if (static_cast<SOCKET>(sock_.load()) == INVALID_SOCKET) return Outcome::Cancelled;
            Sleep(100);
            continue;
        }
        if (ready == 0) continue;
        SOCKET c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        u_long blocking = 0;
        ioctlsocket(c, FIONBIO, &blocking);  // an accepted socket inherits the listener's non-blocking mode
        DWORD timeout = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
        std::string head = ReadHead(c);
        if (head.empty()) {  // a browser's spare connection that never sent anything (or closed at once)
            CloseAbortive(c);
            continue;
        }
        logic::CallbackResult r = logic::ParseCallbackRequest(head, port_, state);
        Wipe(head);
        Outcome out = Outcome::Cancelled;
        bool done = true;
        switch (r.kind) {
            case logic::CallbackResult::NotOurs:
                SendAll(c, logic::CallbackHttpResponse(404, false));
                done = false;
                break;
            case logic::CallbackResult::Bad:
                SendAll(c, logic::CallbackHttpResponse(400, false));
                done = false;
                break;
            case logic::CallbackResult::Denied:
                SendAll(c, logic::CallbackHttpResponse(200, false));
                out = Outcome::Denied;
                break;
            case logic::CallbackResult::OtherError:
                SendAll(c, logic::CallbackHttpResponse(200, false));
                out = Outcome::OtherError;
                break;
            case logic::CallbackResult::Code:
                SendAll(c, logic::CallbackHttpResponse(200, true));
                *code = std::move(r.code);
                out = Outcome::Code;
                break;
        }
        Wipe(r.code);
        CloseAfterPeer(c);
        if (done) {
            Close();
            return out;
        }
    }
}
}  // namespace melange::spotify::oauth
