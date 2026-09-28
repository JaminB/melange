// The `bus` and `bus.counts` channels, and the `bus.names` RPC. One SubscribeAll(Post) handler is installed
// only while at least one client has a `bus` subscription; it matches the message id against the union of
// every client's filter (a bitset rebuilt on each subscribe/unsubscribe) before doing any per-client work, and
// copies the decoded payload only for clients that asked for it and only once per message.
#include <bitset>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/events.h"
#include "melange/bus.h"
#include "melange/oasis.h"
#include "oasis/providers.h"
#include "oasis/streams/wire.h"
#include "tools/json_mini.h"

namespace melange::oasis::providers {
namespace {
namespace streams = melange::oasis::streams;

const char* PathName(bus::Path p) { return p == bus::Path::Post ? "post" : "deliver"; }

// Writes bus::Decode's fields into a jsonmini object.
class JsonOutAdapter final : public bus::JsonOut {
  public:
    jsonmini::Obj obj;
    void Int(const char* k, int64_t v) override { obj.Int(k, v); }
    void Uint(const char* k, uint64_t v) override { obj.UInt(k, v); }
    void Hex(const char* k, uint64_t v) override {
        char buf[24];
        snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
        obj.Str(k, buf);
    }
    void Float(const char* k, double v) override { obj.Float(k, v); }
    void Str(const char* k, std::string_view v) override { obj.Str(k, v); }
    void Vec3(const char* k, const float v[3]) override {
        char buf[96];
        snprintf(buf, sizeof(buf), "[%.9g,%.9g,%.9g]", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]));
        obj.Raw(k, buf);
    }
};

ChannelId g_busCh = 0, g_countsCh = 0;

std::mutex g_mx;                                        // guards g_filters, g_wanted and g_hook together
std::unordered_map<int, streams::BusFilter> g_filters;  // client -> filter
std::bitset<0x10000> g_wanted;                          // union of every client's exact/prefix name matches
bus::SubId g_hook = 0;                                  // 0 = not installed
uint64_t g_busSeq = 0;                                  // this channel's own payload seq (not jlog's, not the wire seq)

void RebuildWanted() {
    g_wanted.reset();
    for (const auto& [client, f] : g_filters) {
        (void)client;
        for (const auto& pattern : f.names) {
            if (streams::IsPrefixPattern(pattern)) {
                const std::string prefix(streams::PrefixOf(pattern));
                bus::ForEachName([&](bus::MsgId id, const char* name) {
                    if (std::string_view(name).substr(0, prefix.size()) == prefix) g_wanted.set(id);
                });
            } else if (const bus::MsgId id = bus::IdOf(pattern); id != bus::kInvalidId) {
                g_wanted.set(id);
            }
        }
    }
}

// Main thread, synchronous, called from inside the engine's Post(): must stay fast. Only copies into per-client
// queues (Publish/PublishTo never touch the network or block), so this respects the "handover by queue" rule.
void OnBusMessage(const bus::MessageView& m, void*) {
    if (!HasSubscribers(g_busCh)) return;
    std::vector<std::pair<int, bool>> matches;  // client, wants decode
    {
        std::lock_guard lk(g_mx);
        if (!g_wanted.test(m.id)) return;
        for (const auto& [client, f] : g_filters) {
            if (!f.path.empty() && f.path != PathName(m.path)) continue;
            if (!streams::AnyNameMatches(m.name, f.names)) continue;
            matches.emplace_back(client, f.decode);
        }
    }
    if (matches.empty()) return;
    const uint64_t seq = ++g_busSeq;
    const std::string plain = streams::BuildBusPayload(seq, m.frame, m.name, m.className, PathName(m.path), m.handle, "");
    std::string decoded;
    bool haveDecoded = false;
    for (const auto& [client, wantDecode] : matches) {
        if (!wantDecode) {
            PublishTo(g_busCh, client, plain);
            continue;
        }
        if (!haveDecoded) {
            haveDecoded = true;
            JsonOutAdapter adapter;
            decoded = bus::Decode(m, adapter)
                          ? streams::BuildBusPayload(seq, m.frame, m.name, m.className, PathName(m.path), m.handle, adapter.obj.End())
                          : plain;
        }
        PublishTo(g_busCh, client, decoded);
    }
}

void OnBusSub(ChannelId, int client, std::string_view filterJson, bool subscribed, void*) {
    std::lock_guard lk(g_mx);
    if (subscribed) {
        g_filters[client] = streams::ParseBusFilter(filterJson);
        if (!g_hook) g_hook = bus::SubscribeAll(bus::Path::Post, &OnBusMessage, nullptr);
    } else {
        g_filters.erase(client);
        if (g_filters.empty() && g_hook) {
            bus::Unsubscribe(g_hook);
            g_hook = 0;
        }
    }
    RebuildWanted();
}

void BusNamesRpc(const Call&, Result& r, void*) {
    jsonmini::Arr arr;
    bus::ForEachName([&](bus::MsgId id, const char* name) {
        arr.Raw(jsonmini::Obj()
                    .UInt("id", id)
                    .Str("name", name)
                    .UInt("posts", bus::CountOf(id, bus::Path::Post))
                    .UInt("deliveries", bus::CountOf(id, bus::Path::Deliver))
                    .End());
    });
    r.json = arr.End();
}

// bus.counts: 1 Hz deltas since the last tick, for every registered name whose count moved. No hook: it reads
// the bus module's own always-on counters, independent of whether the `bus` channel's Post hook is installed.
uint64_t g_countsLastMs = 0;
std::unordered_map<std::string, uint64_t> g_lastCounts;

void CountsPollFrame() {
    if (!HasSubscribers(g_countsCh)) return;
    const uint64_t now = events::LastFrameTick();
    if (now - g_countsLastMs < 1000) return;
    g_countsLastMs = now;
    jsonmini::Obj deltas;
    bool any = false;
    bus::ForEachName([&](bus::MsgId id, const char* name) {
        const uint64_t cur = static_cast<uint64_t>(bus::CountOf(id, bus::Path::Post)) +
                              static_cast<uint64_t>(bus::CountOf(id, bus::Path::Deliver));
        uint64_t& last = g_lastCounts[name];
        if (cur != last) {
            deltas.UInt(name, cur - last);
            any = true;
        }
        last = cur;
    });
    if (any) Publish(g_countsCh, deltas.End());
}

}  // namespace

void InstallBus() {
    ChannelOptions busOpt;
    busOpt.overflow = Overflow::DropOldest;
    g_busCh = AddChannel("bus", busOpt);
    if (g_busCh) OnSubscribe(g_busCh, &OnBusSub, nullptr);

    ChannelOptions countsOpt;
    countsOpt.overflow = Overflow::Coalesce;
    g_countsCh = AddChannel("bus.counts", countsOpt);
    if (g_countsCh) events::Subscribe(events::Event::Frame, &CountsPollFrame);

    AddMethod("bus.names", &BusNamesRpc, nullptr);
}

}  // namespace melange::oasis::providers
