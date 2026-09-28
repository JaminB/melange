// The `lobby` channel: local content identity and the current Steam lobby's peers, published on change and at
// least once a second (Coalesce: only the latest value matters to a client that has not drained).
#include <string>

#include "core/events.h"
#include "melange/mods.h"
#include "melange/oasis.h"
#include "oasis/providers.h"
#include "oasis/streams/wire.h"

namespace melange::oasis::providers {
namespace {
namespace streams = melange::oasis::streams;

constexpr uint64_t kCheckMs = 250;      // how often we look for a change
constexpr uint64_t kHeartbeatMs = 1000; // republish at least this often even with no change

ChannelId g_ch = 0;
uint64_t g_lastCheckMs = 0, g_lastPublishMs = 0;
std::string g_lastPayload;

// New subscribers would otherwise wait up to kHeartbeatMs for their first value; give them the latest one
// right away when we already have one (nothing to give before the first poll tick after Install).
void OnSub(ChannelId, int client, std::string_view, bool subscribed, void*) {
    if (subscribed && !g_lastPayload.empty()) PublishTo(g_ch, client, g_lastPayload);
}

void PollFrame() {
    if (!HasSubscribers(g_ch)) return;
    const uint64_t now = events::LastFrameTick();
    if (now - g_lastCheckMs < kCheckMs) return;
    g_lastCheckMs = now;

    mods::Peer peers[16];
    const int n = mods::Peers(peers, 16);
    std::string payload = streams::BuildLobbyPayload(mods::InLobby(), mods::LocalContent(), peers, n);
    if (payload == g_lastPayload && now - g_lastPublishMs < kHeartbeatMs) return;
    g_lastPayload = payload;
    g_lastPublishMs = now;
    Publish(g_ch, payload);
}

}  // namespace

void InstallLobby() {
    ChannelOptions opt;
    opt.overflow = Overflow::Coalesce;
    g_ch = AddChannel("lobby", opt);
    if (!g_ch) return;
    OnSubscribe(g_ch, &OnSub, nullptr);
    events::Subscribe(events::Event::Frame, &PollFrame);
}

}  // namespace melange::oasis::providers
