// The `net` channel: jlog records of categories `net` (NetSession's own state-change records) and `handshake`,
// filtered per client as `log` is. Same seq-polled design as log.cpp; kept as its own file and its own cursor
// per the frozen file layout, since the two channels have independent subscribers and independent backpressure.
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
constexpr size_t kPollMax = 8192;

bool InScope(const jlog::Line& l) { return l.category == "net" || l.category == "handshake"; }

ChannelId g_ch = 0;
std::mutex g_mx;
uint64_t g_lastSeq = 0;
struct Sub {
    streams::LogFilter filter;
    uint64_t from = 0;  // records up to this seq went out as this client's backlog
};
std::unordered_map<int, Sub> g_subs;

void OnSub(ChannelId, int client, std::string_view filterJson, bool subscribed, void*) {
    if (!subscribed) {
        std::lock_guard lk(g_mx);
        g_subs.erase(client);
        return;
    }
    streams::LogFilter f = streams::ParseLogFilter(filterJson);
    std::lock_guard lk(g_mx);
    std::vector<jlog::Line> lines;
    jlog::Tail(0, lines, 1u << 20);
    const uint64_t head = lines.empty() ? 0 : lines.back().seq;
    g_subs[client] = Sub{f, head};
    std::vector<const jlog::Line*> matched;
    for (auto it = lines.rbegin(); it != lines.rend() && matched.size() < kBacklogMax; ++it)
        if (InScope(*it) && streams::MatchesLog(f, *it)) matched.push_back(&*it);
    for (auto it = matched.rbegin(); it != matched.rend(); ++it) PublishTo(g_ch, client, streams::BuildLogPayload(**it));
}

void PollFrame() {
    if (!HasSubscribers(g_ch)) return;
    std::lock_guard lk(g_mx);
    std::vector<jlog::Line> lines;
    jlog::Tail(g_lastSeq, lines, kPollMax);
    if (lines.empty()) return;
    g_lastSeq = lines.back().seq;
    for (const auto& l : lines) {
        if (!InScope(l)) continue;
        for (const auto& [client, s] : g_subs)
            if (l.seq > s.from && streams::MatchesLog(s.filter, l)) PublishTo(g_ch, client, streams::BuildLogPayload(l));
    }
}

}  // namespace

void InstallNet() {
    ChannelOptions opt;
    opt.overflow = Overflow::DropOldest;
    opt.maxQueueKB = 256;
    g_ch = AddChannel("net", opt);
    if (!g_ch) return;
    OnSubscribe(g_ch, &OnSub, nullptr);
    events::Subscribe(events::Event::Frame, &PollFrame);
}

}  // namespace melange::oasis::providers
