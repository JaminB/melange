#pragma once
// The core's end of the helper pipe (docs/media-helper.md): core is the server, one connection per helper process.
// Two I/O threads per connection, so the game thread never touches the pipe: the reader decodes lines into the Inbox
// and stamps every pong (the watchdog's clock, Critique T1), the writer sends queued lines and a ping every 2 s.
// Game-free (media_selftest links it with the fake helper).
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/media_proto.h"
#include "media/pipe_codec.h"

namespace melange::media::pipeio {
// Helper->core messages waiting for the main thread. Props keep only the latest value per (player, name), so a
// stalled game thread costs one slot per property; everything else queues up to proto::kMaxEventQueue, dropping the
// oldest bt.status first and then the oldest message. Push on the reader thread, Drain on the main thread.
class Inbox {
public:
    void Push(pipe::Message&& m) {
        std::lock_guard lk(mu_);
        if (auto* p = std::get_if<pipe::PropEvt>(&m)) {
            for (pipe::PropEvt& q : props_)
                if (q.id == p->id && q.name == p->name) {
                    q = std::move(*p);
                    return;
                }
            if (props_.size() < kMaxProps) props_.push_back(std::move(*p));
            else ++dropped_;
            return;
        }
        if (q_.size() >= proto::kMaxEventQueue) {
            auto victim = q_.begin();
            for (auto it = q_.begin(); it != q_.end(); ++it)
                if (std::holds_alternative<pipe::BtStatusEvt>(*it)) {
                    victim = it;
                    break;
                }
            q_.erase(victim);
            ++dropped_;
        }
        q_.push_back(std::move(m));
    }
    // The queued messages in arrival order, then the coalesced props.
    void Drain(std::vector<pipe::Message>* out) {
        std::lock_guard lk(mu_);
        for (auto& m : q_) out->push_back(std::move(m));
        q_.clear();
        for (auto& p : props_) out->emplace_back(std::move(p));
        props_.clear();
    }
    size_t Pending() const {
        std::lock_guard lk(mu_);
        return q_.size() + props_.size();
    }
    uint64_t Dropped() const {
        std::lock_guard lk(mu_);
        return dropped_;
    }

    static constexpr size_t kMaxProps = 64 * 16;  // players x properties, with room for a helper that invents ids

private:
    mutable std::mutex mu_;
    std::deque<pipe::Message> q_;
    std::vector<pipe::PropEvt> props_;
    uint64_t dropped_ = 0;
};

struct Timings {
    uint32_t pingMs = proto::kPingIntervalMs;
    uint32_t hangKillMs = proto::kHangKillMs;
    uint32_t connectMs = 30000;  // the helper must connect this soon after its launch
};

// One pipe instance and its two threads. Created before the helper starts; the threads keep the object alive, so a
// Close() from the owner never waits for them (no joins: Event::Shutdown never fires).
class Connection : public std::enable_shared_from_this<Connection> {
public:
    enum class State : uint8_t { Waiting, Connected, Closed };

    // Creates \\.\pipe\<name> (first instance only, local clients only, current-user DACL, a Low label when
    // `lowLabel`). Null with a reason on failure.
    static std::shared_ptr<Connection> Create(const std::wstring& name, bool lowLabel, std::string* err);
    ~Connection();

    // Accepts the first client whose process id is `pid` (any other is disconnected and counted), then runs the I/O.
    // `alive` answers whether the helper process still runs; `onHang` is called once, on the reader thread, when it
    // has been silent (no pong) for hangKillMs by the reader's clock while alive. Called once.
    void Start(uint32_t pid, const Timings& t, std::function<bool()> alive, std::function<void()> onHang);
    // Queues one encoded line (with its '\n'). Any thread. Dropped when closed or when the queue is full.
    void Send(std::string line);
    void Close(const char* why);  // any thread; idempotent

    Inbox& In() { return inbox_; }
    State GetState() const { return state_.load(); }
    bool Hung() const { return hung_.load(); }
    std::string CloseReason() const;
    uint64_t Rejected() const { return rejected_.load(); }    // clients refused for the wrong pid
    uint64_t BadLines() const { return badLines_.load(); }    // lines that did not decode
    uint64_t LongLines() const { return longLines_.load(); }  // lines over the cap, dropped whole
    double MsSincePong() const;

    static constexpr size_t kMaxOutLines = 4096;

private:
    Connection() = default;
    void ReaderBody();
    void WriterBody();

    void* pipe_ = nullptr;   // HANDLE, overlapped
    void* stop_ = nullptr;   // manual-reset event, set by Close
    uint32_t pid_ = 0;
    Timings t_;
    std::function<bool()> alive_;
    std::function<void()> onHang_;
    Inbox inbox_;
    std::atomic<State> state_{State::Waiting};
    std::atomic<bool> hung_{false};
    std::atomic<double> lastPongMs_{0};
    std::atomic<uint64_t> rejected_{0}, badLines_{0}, longLines_{0};
    mutable std::mutex mu_;  // out_, why_
    std::condition_variable_any cv_;
    std::deque<std::string> out_;
    std::string why_;
};

double NowMs();  // QueryPerformanceCounter in ms, the same clock as peerbus::NowMs and the helper's qpc stamps
}  // namespace melange::media::pipeio
