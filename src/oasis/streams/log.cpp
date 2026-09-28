// The `log` channel: every jlog record, filtered per client by level/category/text, with a backlog of the
// last 500 matching records on subscribe. Driven by jlog's own seq-indexed ring (core/jlog_ring.h behind
// jlog::Tail), polled once per frame -- cheap when nobody is subscribed (one atomic load) and O(new lines)
// otherwise.
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/events.h"
#include "melange/jlog.h"
#include "melange/oasis.h"
#include "oasis/providers.h"
#include "oasis/streams/wire.h"

namespace melange::oasis::providers {
namespace {
namespace streams = melange::oasis::streams;

constexpr size_t kBacklogMax = 500;
constexpr size_t kPollMax = 8192;  // one frame's worth even at the busiest configured rate

ChannelId g_ch = 0;
std::mutex g_mx;                                       // guards both of these together
uint64_t g_lastSeq = 0;                                 // highest jlog seq already delivered to live subscribers
std::unordered_map<int, streams::LogFilter> g_filters;  // client -> filter

// Fetches everything jlog currently retains, sends this client up to the last 500 matches as backlog, and
// fast-forwards the shared live cursor past it so the next poll tick never repeats it. A line that arrives
// between this snapshot and the fast-forward lands in the ordinary next-frame poll instead of the backlog;
// on the rare case of two clients subscribing in the same instant, that line could in principle be missed by
// the earlier one -- a narrow, low-stakes race against flooding a client with its whole session's history.
void OnSub(ChannelId, int client, std::string_view filterJson, bool subscribed, void*) {
    if (!subscribed) {
        std::lock_guard lk(g_mx);
        g_filters.erase(client);
        return;
    }
    streams::LogFilter f = streams::ParseLogFilter(filterJson);
    std::vector<jlog::Line> lines;
    jlog::Tail(0, lines, 1u << 20);
    const uint64_t head = lines.empty() ? 0 : lines.back().seq;
    {
        std::lock_guard lk(g_mx);
        g_filters[client] = f;
        if (head > g_lastSeq) g_lastSeq = head;
    }
    std::vector<const jlog::Line*> matched;
    for (auto it = lines.rbegin(); it != lines.rend() && matched.size() < kBacklogMax; ++it)
        if (streams::MatchesLog(f, *it)) matched.push_back(&*it);
    for (auto it = matched.rbegin(); it != matched.rend(); ++it) PublishTo(g_ch, client, streams::BuildLogPayload(**it));
}

void PollFrame() {
    if (!HasSubscribers(g_ch)) return;
    std::lock_guard lk(g_mx);
    std::vector<jlog::Line> lines;
    jlog::Tail(g_lastSeq, lines, kPollMax);
    if (lines.empty()) return;
    g_lastSeq = lines.back().seq;
    for (const auto& l : lines)
        for (const auto& [client, f] : g_filters)
            if (streams::MatchesLog(f, l)) PublishTo(g_ch, client, streams::BuildLogPayload(l));
}

}  // namespace

void InstallLog() {
    ChannelOptions opt;
    opt.overflow = Overflow::DropOldest;
    opt.maxQueueKB = 64;
    g_ch = AddChannel("log", opt);
    if (!g_ch) return;
    OnSubscribe(g_ch, &OnSub, nullptr);
    events::Subscribe(events::Event::Frame, &PollFrame);
}

}  // namespace melange::oasis::providers
