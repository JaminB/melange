#include "wormsign/writer.h"

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
constexpr uint64_t kQueueCapBytes = 8ull << 20;  // budget for chunks not yet on disk between two 10 s flushes
constexpr uint32_t kFlushMs = 10000;
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
    bool open = false, stop = false, flushRequested = false;
    std::thread th;

    void Run() {
        std::unique_lock<std::mutex> lk(mu);
        while (true) {
            cv.wait_for(lk, std::chrono::milliseconds(kFlushMs), [&] { return stop || flushRequested || !q.empty(); });
            std::deque<Item> batch;
            batch.swap(q);
            const uint64_t batchBytes = qBytes;
            qBytes = 0;
            const bool doFlush = flushRequested;
            flushRequested = false;
            const bool doStop = stop;
            lk.unlock();
            for (auto& it : batch) w.Chunk(it.type, it.bytes.data(), it.bytes.size(), it.deflate, it.tickFrom, it.tickTo);
            if (doFlush || doStop) w.Flush();
            (void)batchBytes;
            lk.lock();
            cv.notify_all();
            if (doStop && q.empty()) return;
        }
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
    Impl::Item it{type, tickFrom, tickTo, deflate, std::vector<uint8_t>(static_cast<const uint8_t*>(data),
                                                                         static_cast<const uint8_t*>(data) + n)};
    impl_->qBytes += n;
    ++impl_->queuedTotal;
    impl_->q.push_back(std::move(it));
    impl_->cv.notify_all();
    return true;
}

void Writer::Flush() {
    std::unique_lock<std::mutex> lk(impl_->mu);
    if (!impl_->open) return;
    impl_->flushRequested = true;
    impl_->cv.notify_all();
    impl_->cv.wait(lk, [&] { return !impl_->flushRequested; });
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
