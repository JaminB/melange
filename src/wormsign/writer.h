#pragma once
#include <cstdint>
#include <memory>
#include <string>

// The background writer thread behind the .wsr format (format.h). The main thread only enqueues chunk bytes; the
// thread deflates, appends and flushes each batch to disk as it writes it. The recorder hands its buffers over at the
// start of a match (HEAD, SEED, PDRW, then SETP) and every 10 s after that, so a crash loses at most the last 10 s,
// and the crash path (CrashFlush) can push even those.
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
    void RequestFlush();          // the same without waiting
    bool Close();                 // flush, write INDX + trailer, join the thread
    void Abandon();               // stop without INDX, as a crash would leave it
    // For the crash path, on a thread other than the one that crashed: Enqueue that gives up instead of waiting for
    // the lock, and a flush that waits at most `timeoutMs` for the queue to reach the disk. The file stays without
    // INDX (incomplete), which the reader recovers.
    bool TryEnqueue(uint32_t type, const void* data, size_t n, uint32_t tickFrom = 0, uint32_t tickTo = 0);
    bool FlushWithin(uint32_t timeoutMs);
    bool IsOpen() const;
    uint64_t QueuedChunks() const;
    uint64_t DroppedChunks() const;
    uint64_t BytesWritten() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace melange::wormsign::writer
