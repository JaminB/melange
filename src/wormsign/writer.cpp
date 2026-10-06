#include "wormsign/writer.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "wormsign/format.h"

namespace melange::wormsign::writer {
namespace {
constexpr uint64_t kQueueCapBytes = 8ull << 20;  // budget for chunks not yet on disk between two 10 s hand-overs
constexpr uint32_t kWakeMs = 10000;
}  // namespace

struct Writer::Impl {
    struct Item {
        uint32_t type, tickFrom, tickTo;
        bool deflate;
        std::vector<uint8_t> bytes;
    };
    wsr::Writer w;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Item> q;
    uint64_t qBytes = 0, dropped = 0, queuedTotal = 0;
    uint64_t taken = 0, flushed = 0;  // batches taken off the queue, and the last one of them on disk
    bool open = false, stop = false, flushRequested = false;
    std::thread th;

    void Run() {
        std::unique_lock<std::mutex> lk(mu);
        while (true) {
            cv.wait_for(lk, std::chrono::milliseconds(kWakeMs), [&] { return stop || flushRequested || !q.empty(); });
            std::deque<Item> batch;
            batch.swap(q);
            const uint64_t seq = ++taken;
            qBytes = 0;
            const bool doFlush = flushRequested;
            flushRequested = false;
            const bool doStop = stop;
            lk.unlock();
            for (auto& it : batch) w.Chunk(it.type, it.bytes.data(), it.bytes.size(), it.deflate, it.tickFrom, it.tickTo);
            // Every batch goes to the disk, not just into the CRT's buffer: the recorder hands chunks over at most a
            // few times every 10 s, and a crash must not take a recording's start with it (a 0-byte .wsr).
            if (!batch.empty() || doFlush || doStop) w.Flush();
            lk.lock();
            flushed = seq;
            cv.notify_all();
            if (doStop && q.empty()) return;
        }
    }

    Item Make(uint32_t type, const void* data, size_t n, bool deflate, uint32_t tickFrom, uint32_t tickTo) {
        return Item{type, tickFrom, tickTo, deflate,
                    std::vector<uint8_t>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + n)};
    }
};

Writer::Writer() : impl_(std::make_unique<Impl>()) {}
Writer::~Writer() {
    if (impl_->open) Close();
}

bool Writer::Open(const std::wstring& path) {
    if (impl_->open) return false;
    if (!impl_->w.Open(path)) return false;
    impl_->open = true;
    impl_->stop = false;
    impl_->th = std::thread([this] { impl_->Run(); });
    return true;
}

bool Writer::Enqueue(uint32_t type, const void* data, size_t n, bool deflate, uint32_t tickFrom, uint32_t tickTo) {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->open) return false;
    if (impl_->qBytes + n > kQueueCapBytes) {
        if (impl_->dropped == 0) LOG_ERROR("[wormsign] writer queue over budget, dropping chunks (type=%08x)", type);
        ++impl_->dropped;
        return false;
    }
    impl_->qBytes += n;
    ++impl_->queuedTotal;
    impl_->q.push_back(impl_->Make(type, data, n, deflate, tickFrom, tickTo));
    impl_->cv.notify_all();
    return true;
}

bool Writer::TryEnqueue(uint32_t type, const void* data, size_t n, uint32_t tickFrom, uint32_t tickTo) {
    std::unique_lock<std::mutex> lk(impl_->mu, std::try_to_lock);
    if (!lk.owns_lock() || !impl_->open || impl_->qBytes + n > kQueueCapBytes) return false;
    impl_->qBytes += n;
    ++impl_->queuedTotal;
    impl_->q.push_back(impl_->Make(type, data, n, true, tickFrom, tickTo));
    impl_->cv.notify_all();
    return true;
}

bool Writer::FlushWithin(uint32_t timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lk(impl_->mu, std::defer_lock);
    while (!lk.try_lock()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        Sleep(1);
    }
    if (!impl_->open) return false;
    // The next batch the writer takes holds everything queued so far.
    const uint64_t target = impl_->taken + 1;
    impl_->flushRequested = true;
    impl_->cv.notify_all();
    return impl_->cv.wait_until(lk, deadline, [&] { return impl_->flushed >= target; });
}

void Writer::Flush() {
    std::unique_lock<std::mutex> lk(impl_->mu);
    if (!impl_->open) return;
    const uint64_t target = impl_->taken + 1;
    impl_->flushRequested = true;
    impl_->cv.notify_all();
    impl_->cv.wait(lk, [&] { return impl_->flushed >= target; });
}

void Writer::RequestFlush() {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->open) return;
    impl_->flushRequested = true;
    impl_->cv.notify_all();
}

bool Writer::Close() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->open) return false;
        impl_->stop = true;
        impl_->open = false;  // blocks any further Enqueue before the thread is joined, not just after
        impl_->cv.notify_all();
    }
    if (impl_->th.joinable()) impl_->th.join();
    return impl_->w.Close();
}

void Writer::Abandon() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu);
        if (!impl_->open) return;
        impl_->stop = true;
        impl_->open = false;
        impl_->cv.notify_all();
    }
    if (impl_->th.joinable()) impl_->th.join();
    impl_->w.Abandon();
}

bool Writer::IsOpen() const { return impl_->open; }
uint64_t Writer::QueuedChunks() const { return impl_->queuedTotal; }
uint64_t Writer::DroppedChunks() const { return impl_->dropped; }
uint64_t Writer::BytesWritten() const { return impl_->w.Bytes(); }
}  // namespace melange::wormsign::writer
