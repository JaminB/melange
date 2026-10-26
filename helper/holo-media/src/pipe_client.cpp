#include "pipe_client.h"

#include "log.h"
#include "util.h"

namespace holo {
namespace pipe = melange::media::pipe;
namespace proto = melange::media::proto;

namespace {
constexpr size_t kInboxCap = 4096;           // core lines waiting for the main loop
constexpr size_t kDropDroppableAt = 1u << 20;  // queued bytes: props are dropped from here
constexpr size_t kDropAllAt = 8u << 20;        // and everything from here (the core stopped reading altogether)
}  // namespace

PipeClient::PipeClient() {
    inboxEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
PipeClient::~PipeClient() {
    Close();
    CloseHandle(inboxEvent_);
    CloseHandle(stopEvent_);
}

bool PipeClient::Connect(const std::wstring& name, uint32_t serverPid, uint32_t timeoutMs, std::string* why) {
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    for (;;) {
        // SECURITY_IDENTIFICATION: the server may identify the helper but never act as it.
        h_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h_ != INVALID_HANDLE_VALUE) break;
        const DWORD e = GetLastError();
        if ((e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND) || GetTickCount64() >= until) {
            if (why) *why = "connect: " + WinError(e);
            return false;
        }
        if (e == ERROR_PIPE_BUSY) WaitNamedPipeW(name.c_str(), 250);
        else Sleep(50);
    }
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(h_, &pid) || pid != serverPid) {
        if (why) *why = "the pipe's server is not the parent process (pid " + std::to_string(pid) + ")";
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        return false;
    }
    return true;
}

void PipeClient::Start() {
    reader_ = std::thread([this] { ReadLoop(); });
    writer_ = std::thread([this] { WriteLoop(); });
}

void PipeClient::Break(const char* why, DWORD err) {
    if (!broken_.exchange(true)) HLOG("pipe: %s: %s", why, WinError(err).c_str());
    SetEvent(inboxEvent_);
    outCv_.notify_all();
}

void PipeClient::Send(std::string line, bool droppable) {
    {
        std::lock_guard lk(outMu_);
        if (broken_.load() || (droppable && outBytes_ > kDropDroppableAt) || outBytes_ > kDropAllAt) {
            if ((dropped_++ & 1023) == 0) HLOG("pipe: core is not reading, %llu lines dropped", static_cast<unsigned long long>(dropped_));
            return;
        }
        outBytes_ += line.size();
        outbox_.push_back(std::move(line));
    }
    outCv_.notify_one();
}

bool PipeClient::Pop(pipe::Message* out) {
    std::lock_guard lk(inMu_);
    if (inbox_.empty()) return false;
    *out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

void PipeClient::ReadLoop() {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pipe::LineSplitter split;
    std::string buf(64 * 1024, '\0');
    while (!broken_.load()) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        BOOL ok = ReadFile(h_, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &ov);
        DWORD e = ok ? 0 : GetLastError();
        if (!ok && e == ERROR_IO_PENDING) {
            const HANDLE w[2] = {ov.hEvent, stopEvent_};
            if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &got, TRUE);
                break;
            }
            ok = GetOverlappedResult(h_, &ov, &got, FALSE);
            e = ok ? 0 : GetLastError();
        } else if (ok) {
            GetOverlappedResult(h_, &ov, &got, FALSE);
        }
        if (!ok && e != ERROR_MORE_DATA) {  // a message-mode pipe reports a long message in parts
            Break("read", e);
            break;
        }
        split.Feed(buf.data(), got, [&](std::string_view line) {
            pipe::Message m;
            std::string why;
            if (!pipe::Decode(line, pipe::Dir::ToHelper, &m, &why)) {
                if ((badLines_++ & 255) == 0) HLOG("pipe: ignored a line from the core: %s", why.c_str());
                return;
            }
            if (std::holds_alternative<pipe::PingCmd>(m)) {
                Send(pipe::Message{pipe::PongEvt{}});
                return;
            }
            {
                std::lock_guard lk(inMu_);
                if (inbox_.size() >= kInboxCap && !std::holds_alternative<pipe::QuitCmd>(m)) {
                    HLOG("pipe: inbox full, a core message was dropped");
                    return;
                }
                inbox_.push_back(std::move(m));
            }
            SetEvent(inboxEvent_);
        });
    }
    CloseHandle(ov.hEvent);
}

void PipeClient::WriteLoop() {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        std::string line;
        {
            std::unique_lock lk(outMu_);
            outCv_.wait(lk, [&] { return !outbox_.empty() || broken_.load() || WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0; });
            if (outbox_.empty() || broken_.load()) break;
            line = std::move(outbox_.front());
            outbox_.pop_front();
        }
        ResetEvent(ov.hEvent);
        DWORD put = 0;
        BOOL ok = WriteFile(h_, line.data(), static_cast<DWORD>(line.size()), nullptr, &ov);
        DWORD e = ok ? 0 : GetLastError();
        if (!ok && e == ERROR_IO_PENDING) {
            const HANDLE w[2] = {ov.hEvent, stopEvent_};
            if (WaitForMultipleObjects(2, w, FALSE, INFINITE) != WAIT_OBJECT_0) {
                CancelIoEx(h_, &ov);
                GetOverlappedResult(h_, &ov, &put, TRUE);
                break;
            }
            ok = GetOverlappedResult(h_, &ov, &put, FALSE);
            e = ok ? 0 : GetLastError();
        }
        {
            std::lock_guard lk(outMu_);
            outBytes_ -= line.size();
        }
        if (!ok) {
            Break("write", e);
            break;
        }
    }
    CloseHandle(ov.hEvent);
}

void PipeClient::Flush(uint32_t ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until && !broken_.load()) {
        {
            std::lock_guard lk(outMu_);
            if (outBytes_ == 0) return;
        }
        Sleep(10);
    }
}

void PipeClient::Close() {
    if (h_ == INVALID_HANDLE_VALUE) return;
    SetEvent(stopEvent_);
    outCv_.notify_all();
    CancelIoEx(h_, nullptr);
    if (reader_.joinable()) reader_.join();
    if (writer_.joinable()) writer_.join();
    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
}
}  // namespace holo
