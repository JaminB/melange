#pragma once
#include <cstdint>
#include <memory>
#include <string>

// The background writer thread behind the .wsr format (format.h). The main thread only enqueues chunk bytes; the
// thread deflates, appends and flushes every 10 s, so a crash loses at most that much of a recording.
namespace melange::wormsign::writer {
class Writer {
  public:
    Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer();

    bool Open(const std::wstring& path);
    // Queues a copy of [data, data+n); returns false if not open or the queue is over its memory cap (dropped,
    // counted, logged at most once per session).
    bool Enqueue(uint32_t type, const void* data, size_t n, bool deflate = true, uint32_t tickFrom = 0,
                 uint32_t tickTo = 0);
    void Flush();                 // blocks until the queue drains to disk
    bool Close();                 // flush, write INDX + trailer, join the thread
    void Abandon();               // stop without INDX, as a crash would leave it (tests only)
    bool IsOpen() const;
    uint64_t QueuedChunks() const;
    uint64_t DroppedChunks() const;
    uint64_t BytesWritten() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace melange::wormsign::writer
