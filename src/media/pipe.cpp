// The helper pipe's server end and its two I/O threads (pipe.h).
#include "media/pipe.h"

#include <windows.h>

#include <thread>

#include "core/log.h"
#include "core/thread_guard.h"
#include "media/shm.h"

namespace melange::media::pipeio {
double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / freq;
}

namespace {
constexpr DWORD kBufBytes = 64 * 1024;

// Waits for an overlapped operation on `pipe` or for `stop`. 1: done (*n bytes), 0: timed out (still pending),
// -1: failed or stopped (the operation is cancelled).
int Await(HANDLE pipe, OVERLAPPED* ov, HANDLE stop, DWORD timeoutMs, DWORD* n) {
    const HANDLE hs[2] = {ov->hEvent, stop};
    const DWORD w = WaitForMultipleObjects(2, hs, FALSE, timeoutMs);
    if (w == WAIT_TIMEOUT) return 0;
    if (w != WAIT_OBJECT_0) {
        CancelIoEx(pipe, ov);
        GetOverlappedResult(pipe, ov, n, TRUE);
        return -1;
    }
    return GetOverlappedResult(pipe, ov, n, FALSE) ? 1 : -1;
}
}  // namespace

std::shared_ptr<Connection> Connection::Create(const std::wstring& name, bool lowLabel, std::string* err) {
    shmio::Security sec;
    if (!sec.Build(lowLabel, err)) return nullptr;
    HANDLE h = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, kBufBytes,
                                kBufBytes, 0, static_cast<SECURITY_ATTRIBUTES*>(sec.Attributes()));
    if (h == INVALID_HANDLE_VALUE) {
        *err = "CreateNamedPipe (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    std::shared_ptr<Connection> c(new Connection());
    c->pipe_ = h;
    c->stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return c;
}

Connection::~Connection() {
    if (pipe_) CloseHandle(pipe_);
    if (stop_) CloseHandle(stop_);
}

void Connection::Start(uint32_t pid, const Timings& t, std::function<bool()> alive, std::function<void()> onHang) {
    pid_ = pid;
    t_ = t;
    alive_ = std::move(alive);
    onHang_ = std::move(onHang);
    auto self = shared_from_this();
    std::thread([self] { GuardedThreadBody("media pipe reader", [&] { self->ReaderBody(); }); }).detach();
    std::thread([self] { GuardedThreadBody("media pipe writer", [&] { self->WriterBody(); }); }).detach();
}

void Connection::Send(std::string line) {
    {
        std::lock_guard lk(mu_);
        if (state_.load() == State::Closed) return;
        if (out_.size() >= kMaxOutLines) {
            LOG_WARN("[media] pipe send queue full; a line was dropped");
            return;
        }
        out_.push_back(std::move(line));
    }
    cv_.notify_all();
}

void Connection::Close(const char* why) {
    {
        std::lock_guard lk(mu_);
        if (state_.load() == State::Closed) return;
        state_.store(State::Closed);
        why_ = why ? why : "";
    }
    cv_.notify_all();
    SetEvent(stop_);
    CancelIoEx(pipe_, nullptr);
}

std::string Connection::CloseReason() const {
    std::lock_guard lk(mu_);
    return why_;
}

double Connection::MsSincePong() const { return NowMs() - lastPongMs_.load(); }

void Connection::ReaderBody() {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct EventCloser {
        HANDLE h;
        ~EventCloser() { CloseHandle(h); }
    } closer{ov.hEvent};
    HANDLE pipe = pipe_, stop = stop_;

    // Accept: only the helper we started. Anyone else (a squatter racing for the name cannot exist, since the pipe
    // is the first instance, but any local process of this user may connect) is dropped, and we wait again.
    const double deadline = NowMs() + t_.connectMs;
    for (;;) {
        if (state_.load() == State::Closed) return;
        ResetEvent(ov.hEvent);
        DWORD n = 0;
        bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
        if (!connected) {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (e == ERROR_IO_PENDING) {
                for (;;) {
                    const double left = deadline - NowMs();
                    if (left <= 0) {
                        CancelIoEx(pipe, &ov);
                        GetOverlappedResult(pipe, &ov, &n, TRUE);
                        Close("the helper never connected");
                        return;
                    }
                    const int r = Await(pipe, &ov, stop, static_cast<DWORD>(left < 250 ? left : 250), &n);
                    if (r == 1) {
                        connected = true;
                        break;
                    }
                    if (r < 0) {
                        Close("connect failed");
                        return;
                    }
                    if (alive_ && !alive_()) {
                        CancelIoEx(pipe, &ov);
                        GetOverlappedResult(pipe, &ov, &n, TRUE);
                        Close("the helper exited before it connected");
                        return;
                    }
                }
            } else {
                Close("ConnectNamedPipe failed");
                return;
            }
        }
        ULONG client = 0;
        if (connected && GetNamedPipeClientProcessId(pipe, &client) && client == pid_) break;
        rejected_.fetch_add(1);
        LOG_WARN("[media] pipe client pid %lu is not the helper (%u); disconnected", client, pid_);
        DisconnectNamedPipe(pipe);
    }
    lastPongMs_.store(NowMs());
    {
        std::lock_guard lk(mu_);
        if (state_.load() != State::Closed) state_.store(State::Connected);
    }
    cv_.notify_all();

    pipe::LineSplitter split;
    std::unique_ptr<char[]> buf(new char[kBufBytes]);
    for (;;) {
        if (state_.load() == State::Closed) return;
        ResetEvent(ov.hEvent);
        DWORD n = 0;
        int r = 1;
        if (!ReadFile(pipe, buf.get(), kBufBytes, &n, &ov)) {
            if (GetLastError() != ERROR_IO_PENDING) {
                Close("the helper closed the pipe");
                return;
            }
            // The watchdog: wait in short steps and judge the pongs by this thread's clock, never the game's.
            while ((r = Await(pipe, &ov, stop, 250, &n)) == 0) {
                if (!hung_.load() && NowMs() - lastPongMs_.load() >= t_.hangKillMs && (!alive_ || alive_())) {
                    hung_.store(true);
                    LOG_WARN("[media] helper sent no pong for %u ms; stopping it", t_.hangKillMs);
                    if (onHang_) onHang_();
                }
            }
        }
        if (r < 0 || n == 0) {
            Close(hung_.load() ? "the helper hung" : "the helper closed the pipe");
            return;
        }
        const uint64_t longBefore = split.Dropped();
        split.Feed(buf.get(), n, [&](std::string_view line) {
            pipe::Message m;
            std::string why;
            if (!pipe::Decode(line, pipe::Dir::ToCore, &m, &why)) {
                if (badLines_.fetch_add(1) < 20) LOG_WARN("[media] helper line ignored: %s", why.c_str());
                return;
            }
            if (std::holds_alternative<pipe::PongEvt>(m)) {
                lastPongMs_.store(NowMs());
                return;
            }
            inbox_.Push(std::move(m), line.size());
        });
        if (split.Dropped() != longBefore) longLines_.fetch_add(split.Dropped() - longBefore);
    }
}

void Connection::WriterBody() {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    struct EventCloser {
        HANDLE h;
        ~EventCloser() { CloseHandle(h); }
    } closer{ov.hEvent};
    {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [&] { return state_.load() != State::Waiting; });
    }
    const std::string ping = pipe::Encode(pipe::PingCmd{});
    double nextPing = NowMs();
    std::deque<std::string> batch;
    for (;;) {
        {
            std::unique_lock lk(mu_);
            for (;;) {
                if (state_.load() == State::Closed) return;
                if (!out_.empty()) break;
                const double left = nextPing - NowMs();
                if (left <= 0) break;
                cv_.wait_for(lk, std::chrono::milliseconds(static_cast<long long>(left) + 1));
            }
            batch.swap(out_);
        }
        if (NowMs() >= nextPing) {
            batch.push_back(ping);
            nextPing = NowMs() + t_.pingMs;
        }
        for (const std::string& line : batch) {
            size_t off = 0;
            while (off < line.size()) {
                ResetEvent(ov.hEvent);
                DWORD n = 0;
                const DWORD want = static_cast<DWORD>(line.size() - off);
                if (!WriteFile(pipe_, line.data() + off, want, &n, &ov)) {
                    if (GetLastError() != ERROR_IO_PENDING) {
                        Close("write failed");
                        return;
                    }
                    // A helper that stops reading blocks this write; Close() (the watchdog's kill, or the owner)
                    // releases it through the stop event.
                    int r;
                    while ((r = Await(pipe_, &ov, stop_, 1000, &n)) == 0) {
                    }
                    if (r < 0) {
                        Close("write failed");
                        return;
                    }
                }
                off += n;
            }
        }
        batch.clear();
    }
}
}  // namespace melange::media::pipeio
