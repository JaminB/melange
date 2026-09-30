// The "erg" channel: Test state changes and, at every level start, the level and (once the match has run for a
// moment) its water level. Coalesce: a slow client gets the latest message.
#include <string>

#include "core/events.h"
#include "levels/test.h"
#include "melange/levels.h"
#include "melange/oasis.h"
#include "melange/sim.h"
#include "oasis/providers.h"

namespace melange::oasis::providers {
namespace {
namespace lv = melange::levels;

constexpr uint64_t kWaterDelayFrames = 120;

ChannelId g_ch = 0;
std::string g_last;
struct Pending {
    bool armed = false;
    std::string key, stem;
    lv::Source source = lv::Source::Vanilla;
    bool online = false;
    uint64_t inMatchFrames = 0;
} g_start;

void Send(std::string payload) {
    g_last = std::move(payload);
    Publish(g_ch, g_last);
}

void OnSub(ChannelId, int client, std::string_view, bool subscribed, void*) {
    if (subscribed && !g_last.empty()) PublishTo(g_ch, client, g_last);
}

void OnTest(lv::TestState s, const char* key, const char* detail, void*) { Send(lv::test::StateJson(s, key, detail)); }

void OnStart(const lv::LevelStart& s, void*) {
    Send(lv::test::LevelJson(s.key, s.stem, s.source, s.online, nullptr));
    g_start = {true, s.key, s.stem, s.source, s.online, 0};
}

void OnFrame() {
    if (!g_start.armed) return;
    if (!sim::InMatch()) {
        g_start.inMatchFrames = 0;
        return;
    }
    if (++g_start.inMatchFrames < kWaterDelayFrames) return;
    g_start.armed = false;
    float w = 0;
    if (lv::WaterLevel(&w)) Send(lv::test::LevelJson(g_start.key, g_start.stem, g_start.source, g_start.online, &w));
}
}  // namespace

void InstallErgChannel() {
    ChannelOptions opt;
    opt.overflow = Overflow::Coalesce;
    opt.mainThreadSubscribe = true;
    g_ch = AddChannel("erg", opt);
    if (!g_ch) return;
    OnSubscribe(g_ch, &OnSub, nullptr);
    lv::OnTestState(&OnTest, nullptr);
    lv::OnLevelStart(&OnStart, nullptr);
    events::Subscribe(events::Event::Frame, &OnFrame);
}
}  // namespace melange::oasis::providers
