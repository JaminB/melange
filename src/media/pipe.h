#pragma once
// The core's end of the helper pipe (docs/media-helper.md): core is the server, one connection per helper process.
// Two I/O threads per connection, so the game thread never touches the pipe: the reader decodes lines into the Inbox
// and stamps every pong (the watchdog's clock, Critique T1), the writer sends queued lines and a ping every 2 s.
// Game-free (media_selftest links it with the fake helper).
#include <atomic>
#include <condition_variable>
#include <cstddef>
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
// oldest bt.status first and then the oldest message. All of it together is held to kMaxBytes of decoded lines
// (the oldest message goes first, then the oldest prop), and props and events for a player id core never handed
// out are dropped, so a hostile helper cannot pile heap up in the 32-bit game while its thread is stalled.
// Push on the reader thread, Drain and SetIdCeiling on the main thread.
class Inbox {
public:
    // `bytes` is the line the message was decoded from (its cost against kMaxBytes).
    void Push(pipe::Message&& m, size_t bytes = 0) {
        const uint32_t ceiling = ceiling_.load();
        if (auto* p = std::get_if<pipe::PropEvt>(&m); p && p->id > ceiling) return Drop();
        if (auto* e = std::get_if<pipe::EventEvt>(&m); e && e->id > ceiling) return Drop();
        std::lock_guard lk(mu_);
        if (auto* p = std::get_if<pipe::PropEvt>(&m)) {
            for (size_t i = 0; i < props_.size(); ++i)
                if (props_[i].id == p->id && props_[i].name == p->name) {
                    props_[i] = std::move(*p);
                    bytes_ += bytes;
                    bytes_ -= propBytes_[i];
                    propBytes_[i] = bytes;
                    Trim();
                    return;
                }
            if (props_.size() < kMaxProps) {
                props_.push_back(std::move(*p));
                propBytes_.push_back(bytes);
                bytes_ += bytes;
                Trim();
            } else {
                ++dropped_;
            }
            return;
        }
        if (q_.size() >= proto::kMaxEventQueue) {
            size_t victim = 0;
            for (size_t i = 0; i < q_.size(); ++i)
                if (std::holds_alternative<pipe::BtStatusEvt>(q_[i])) {
                    victim = i;
                    break;
                }
            EraseQueued(victim);
            ++dropped_;
        }
        q_.push_back(std::move(m));
        qBytes_.push_back(bytes);
        bytes_ += bytes;
        Trim();
    }
    // The queued messages in arrival order, then the coalesced props.
    void Drain(std::vector<pipe::Message>* out) {
        std::lock_guard lk(mu_);
        for (auto& m : q_) out->push_back(std::move(m));
        q_.clear();
        qBytes_.clear();
        for (auto& p : props_) out->emplace_back(std::move(p));
        props_.clear();
        propBytes_.clear();
        bytes_ = 0;
    }
    // The highest player id core has handed out; props and events for a higher one are dropped. Any value until set.
    void SetIdCeiling(uint32_t id) { ceiling_.store(id); }
    size_t Pending() const {
        std::lock_guard lk(mu_);
        return q_.size() + props_.size();
    }
    size_t Bytes() const {
        std::lock_guard lk(mu_);
        return bytes_;
    }
    uint64_t Dropped() const {
        std::lock_guard lk(mu_);
        return dropped_;
    }

    static constexpr size_t kMaxProps = 64 * 16;     // players x properties, with room for a helper that invents ids
    static constexpr size_t kMaxBytes = 4u << 20;     // decoded lines held at once (Critique S11: hostile helper)

private:
    void Drop() {
        std::lock_guard lk(mu_);
        ++dropped_;
    }
    void EraseQueued(size_t i) {
        bytes_ -= qBytes_[i];
        q_.erase(q_.begin() + static_cast<std::ptrdiff_t>(i));
        qBytes_.erase(qBytes_.begin() + static_cast<std::ptrdiff_t>(i));
    }
    // Over budget: the oldest queued message goes first, then the oldest prop. Always keeps the newest entry.
    void Trim() {
        while (bytes_ > kMaxBytes && q_.size() + props_.size() > 1) {
            if (!q_.empty()) {
                EraseQueued(0);
            } else {
                bytes_ -= propBytes_.front();
                props_.erase(props_.begin());
                propBytes_.erase(propBytes_.begin());
            }
            ++dropped_;
        }
    }

    mutable std::mutex mu_;
    std::deque<pipe::Message> q_;
    std::deque<size_t> qBytes_;
    std::vector<pipe::PropEvt> props_;
    std::vector<size_t> propBytes_;
    size_t bytes_ = 0;
    uint64_t dropped_ = 0;
    std::atomic<uint32_t> ceiling_{UINT32_MAX};
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
