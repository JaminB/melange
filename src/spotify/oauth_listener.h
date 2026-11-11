#pragma once
// The Spotify sign-in's redirect target (Melange 0.13.0): a one-shot HTTP listener on 127.0.0.1:<port>, bound
// exclusively, that exists only while a sign-in runs (at most 3 minutes). It answers each request with a fixed page
// (spotify_logic.h CallbackHttpResponse), accepts only GET /callback with Host 127.0.0.1:<port> and the sign-in's
// state, and closes itself once the code (or a refusal) arrived. Melange 0.14.1: the browser's request that carried the
// code is held open and answered with Reply() once the sign-in finished, so its page says whether it worked. Oasis is not used: it needs its own token on every
// request and its port floats. Winsock; no other dependency.
#include <atomic>
#include <cstdint>
#include <string>

namespace melange::spotify::oauth {
class Listener {
public:
    enum class Outcome { Code, Denied, OtherError, Timeout, Cancelled };
    // Binds 127.0.0.1:port (SO_EXCLUSIVEADDRUSE) and listens. False when the port is taken or winsock failed. Main thread.
    bool Open(int port);
    // Any thread, idempotent: closes the socket; Serve returns Cancelled within 500 ms.
    void Close();
    // Serves until a final answer, GetTickCount64() >= deadlineMs, Close() or `cancel`. Blocking (the "spotify-auth"
    // thread). The listening socket is always closed when it returns. On Code, *code holds the authorization code and
    // *held the browser's connection, not answered yet: the caller must Reply() to it (on every path). Otherwise *held is
    // ~0.
    Outcome Serve(const std::string& state, uint64_t deadlineMs, const std::atomic<bool>& cancel, std::string* code, uintptr_t* held);
    // Sends `response` on a held connection and closes it (letting the browser close first). Idempotent on ~0; sets *conn
    // to ~0.
    static void Reply(uintptr_t* conn, const std::string& response);

private:
    std::atomic<uintptr_t> sock_{~static_cast<uintptr_t>(0)};  // a SOCKET; ~0 = INVALID_SOCKET
    int port_ = 0;
};
}  // namespace melange::spotify::oauth
