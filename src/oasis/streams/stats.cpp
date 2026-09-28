// The `stats` channel: server counters plus render timing (busy p50/p95, fps), once a second.
#include <string>

#include "core/events.h"
#include "melange/oasis.h"
#include "melange/render.h"
#include "oasis/providers.h"
#include "oasis/streams/wire.h"

namespace melange::oasis::providers {
namespace {
namespace streams = melange::oasis::streams;

constexpr uint64_t kIntervalMs = 1000;

ChannelId g_ch = 0;
uint64_t g_lastMs = 0;
std::string g_last;

void OnSub(ChannelId, int client, std::string_view, bool subscribed, void*) {
    if (subscribed && !g_last.empty()) PublishTo(g_ch, client, g_last);
}

void PollFrame() {
    if (!HasSubscribers(g_ch)) return;
    const uint64_t now = events::LastFrameTick();
    if (now - g_lastMs < kIntervalMs) return;
    g_lastMs = now;
    g_last = streams::BuildStatsPayload(GetStats(), render::GetTiming());
    Publish(g_ch, g_last);
}

}  // namespace

void InstallStats() {
    ChannelOptions opt;
    opt.overflow = Overflow::Coalesce;
    g_ch = AddChannel("stats", opt);
    if (!g_ch) return;
    OnSubscribe(g_ch, &OnSub, nullptr);
    events::Subscribe(events::Event::Frame, &PollFrame);
}

}  // namespace melange::oasis::providers
