#include "core/jlog_ring.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace melange::jlog::ring {
namespace {

std::mutex g_mx;
std::vector<Line> g_slots;  // size is always a power of two; empty until the first SetCapacity()
size_t g_mask = 0;
std::atomic<uint64_t> g_head{0};
uint64_t g_oldest = 1;  // smallest seq still guaranteed retained; meaningful only while g_slots is non-empty

size_t RoundUpPow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

}  // namespace

void SetCapacity(size_t capacity) {
    std::lock_guard lk(g_mx);
    const size_t cap = RoundUpPow2((std::max<size_t>)(capacity, 1));
    g_slots.assign(cap, Line{});
    g_mask = cap - 1;
    g_head.store(0, std::memory_order_relaxed);
    g_oldest = 1;
}

void Push(const Line& l) {
    std::lock_guard lk(g_mx);
    if (g_slots.empty()) return;
    g_slots[static_cast<size_t>(l.seq) & g_mask] = l;
    g_head.store(l.seq, std::memory_order_release);
    const uint64_t cap = g_slots.size();
    g_oldest = l.seq > cap ? l.seq - cap + 1 : 1;
}

uint64_t Head() { return g_head.load(std::memory_order_acquire); }

size_t Since(uint64_t afterSeq, std::vector<Line>& out, size_t max) {
    std::lock_guard lk(g_mx);
    if (g_slots.empty()) return 0;
    const uint64_t head = g_head.load(std::memory_order_relaxed);
    if (head == 0 || afterSeq >= head) return 0;
    const uint64_t start = (std::max)(afterSeq + 1, g_oldest);
    size_t added = 0;
    for (uint64_t s = start; s <= head && added < max; ++s) {
        const Line& l = g_slots[static_cast<size_t>(s) & g_mask];
        if (l.seq != s) continue;  // overwritten by a burst since `head` was read; skip rather than misreport
        out.push_back(l);
        ++added;
    }
    return added;
}

void Clear() {
    std::lock_guard lk(g_mx);
    for (auto& l : g_slots) l = Line{};
    g_head.store(0, std::memory_order_relaxed);
    g_oldest = 1;
}

}  // namespace melange::jlog::ring
