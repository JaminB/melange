#pragma once
// The helper's end of the core's pipe (docs/media-helper.md "The pipe"). The core is the server; the helper connects,
// checks that the server is its parent (no squatted name), and runs one reader and one writer thread on an overlapped
// handle, so a read in progress never blocks a write. The reader answers `ping` itself (a busy main loop never looks
// hung), decodes every line with pipe::Decode(Dir::ToHelper) and queues only lines that decode; the rest are logged
// and ignored.
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "media/pipe_codec.h"

namespace holo {
class PipeClient {
public:
    PipeClient();
    ~PipeClient();
    // Connects within `timeoutMs` and checks the server's pid. False with a reason.
    bool Connect(const std::wstring& name, uint32_t serverPid, uint32_t timeoutMs, std::string* why);
    void Start();
    // Queues one encoded line ('\n' included). Droppable lines (time-pos and the like) are dropped first when the
    // core stops reading; nothing ever blocks the caller.
    void Send(std::string line, bool droppable = false);
    void Send(const melange::media::pipe::Message& m, bool droppable = false) { Send(melange::media::pipe::Encode(m), droppable); }
    bool Pop(melange::media::pipe::Message* out);
    HANDLE InboxEvent() const { return inboxEvent_; }  // auto-reset, signalled when a message was queued or the pipe broke
    bool Broken() const { return broken_.load(); }
    // Waits up to `ms` for the writer to drain (quit path).
    void Flush(uint32_t ms);
    void Close();

private:
    void ReadLoop();
    void WriteLoop();
    void Break(const char* why, DWORD err);

    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE inboxEvent_ = nullptr;
    HANDLE stopEvent_ = nullptr;  // manual reset: Close
    std::thread reader_, writer_;
    std::atomic<bool> broken_{false};

    std::mutex inMu_;
    std::deque<melange::media::pipe::Message> inbox_;

    std::mutex outMu_;
    std::condition_variable outCv_;
    std::deque<std::string> outbox_;
    size_t outBytes_ = 0;
    uint64_t dropped_ = 0, badLines_ = 0;
};
}  // namespace holo
