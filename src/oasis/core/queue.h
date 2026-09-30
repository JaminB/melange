#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "melange/oasis.h"

// One client's outbound data: never-dropped control messages (welcome, res, err) plus one bounded queue per
// subscribed channel. Not thread-safe; the owner locks. Pure, so the self-test drives it directly.
namespace melange::oasis::core {
class Outbox {
  public:
    struct Limits {
        size_t connectionBytes = 2u << 20;  // cap on control + channel bytes
        size_t batchItems = 256;
        size_t binaryBytes = 40u << 20;     // cap on queued binary frames (Erg level blobs)
        uint32_t flushMs = 50;              // a channel is flushed at most this often
    };
    explicit Outbox(Limits l = {}) : lim_(l) {}

    void Subscribe(ChannelId ch, std::string_view name, Overflow ov, size_t maxBytes);
    void Unsubscribe(ChannelId ch);
    bool Subscribed(ChannelId ch) const;

    // False when the control queue is over the connection cap (the caller closes the client with 4008).
    bool PushControl(std::string msg);
    // A binary frame, sent in order with the control messages. False when over the binary cap.
    bool PushBinary(std::string frame);
    // Returns false when not subscribed. Drops (DropOldest) or replaces (Coalesce) to stay under the channel cap
    // and the connection cap; `dropped` receives how many messages were discarded.
    bool Publish(ChannelId ch, std::string_view json, uint64_t* dropped = nullptr);

    // Appends due messages: every control message, then for each channel whose flush time has come a "drop"
    // notice (when messages were lost) and "ev" batches. Returns the milliseconds until the next channel is due
    // (UINT32_MAX when nothing is queued). `binary`, when given, gets one flag per message (1 = a binary frame).
    uint32_t Take(uint32_t nowMs, std::vector<std::string>* out, std::vector<uint8_t>* binary = nullptr);
    size_t Bytes() const { return controlBytes_ + chanBytes_; }

  private:
    struct Item { uint64_t seq; std::string json; };
    struct Chan {
        ChannelId id;
        std::string name;
        Overflow ov;
        size_t maxBytes, bytes = 0;
        std::deque<Item> items;
        uint64_t seq = 0, dropped = 0;
        uint32_t lastFlush = 0;
        bool flushedOnce = false;
    };
    Chan* Find(ChannelId ch);
    const Chan* Find(ChannelId ch) const;
    void DropFront(Chan& c);

    struct Msg { std::string data; bool binary; };
    Limits lim_;
    std::deque<Msg> control_;
    size_t controlBytes_ = 0, binaryBytes_ = 0, chanBytes_ = 0;
    std::vector<Chan> chans_;
};
}  // namespace melange::oasis::core
