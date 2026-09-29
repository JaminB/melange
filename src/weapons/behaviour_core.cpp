#include <algorithm>
#include <cmath>
#include <vector>

#include "core/log.h"
#include "weapons/behaviour.h"

namespace melange::weapons {
namespace {
struct Observer {
    int handle, order;
    EventFn fn;
    void* user;
};
std::vector<Observer> g_obs;
int g_next = 1;
int g_limit = behaviour::kMaxExtraCap;

bool g_open = false;
float g_origin[3] = {};
float g_queue[behaviour::kMaxExtraCap][3] = {};
int g_queued = 0;
behaviour::Counters g_count = {};
}  // namespace

int On(EventFn fn, void* user, int order) {
    if (!fn) return 0;
    const Observer o{g_next++, order, fn, user};
    const auto at = std::upper_bound(g_obs.begin(), g_obs.end(), o,
                                     [](const Observer& a, const Observer& b) { return a.order < b.order; });
    g_obs.insert(at, o);
    return o.handle;
}

void RemoveOn(int handle) {
    std::erase_if(g_obs, [handle](const Observer& o) { return o.handle == handle; });
}

QueueResult QueueExplosion(const float d[3]) {
    if (!g_open) return QueueResult::NotInExplosion;
    if (!d) return QueueResult::OutOfRange;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(d[i]) || std::fabs(d[i]) > behaviour::kMaxOffset) return QueueResult::OutOfRange;
    if (g_queued >= g_limit) return QueueResult::Full;
    for (int i = 0; i < 3; ++i) g_queue[g_queued][i] = g_origin[i] + d[i];
    ++g_queued;
    return QueueResult::Ok;
}

namespace behaviour {
void SetExtraLimit(int n) { g_limit = std::clamp(n, 0, kMaxExtraCap); }
int ExtraLimit() { return g_limit; }

void Raise(const EventArgs& a) {
    if (g_obs.empty()) return;
    const std::vector<Observer> obs = g_obs;
    for (auto& o : obs) {
        if (std::none_of(g_obs.begin(), g_obs.end(), [&](const Observer& x) { return x.handle == o.handle; })) continue;
        try {
            o.fn(a, o.user);
        } catch (...) {
            LOG_ERROR("[weapons] an event observer threw (handle %d)", o.handle);
        }
    }
}

void OpenExplosion(const float origin[3]) {
    for (int i = 0; i < 3; ++i) g_origin[i] = origin ? origin[i] : 0.0f;
    g_queued = 0;
    g_open = true;
}

int CloseExplosion(float out[][3], int max) {
    g_open = false;
    const int n = std::min(g_queued, max);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = g_queue[i][j];
    g_queued = 0;
    return n;
}

bool InExplosion() { return g_open; }

Counters GetCounters() { return g_count; }

void CountEvent(Event e) {
    switch (e) {
        case Event::Fire: ++g_count.fires; break;
        case Event::Tick: ++g_count.ticks; break;
        case Event::Impact: ++g_count.impacts; break;
        case Event::Explosion: ++g_count.explosions; break;
    }
}

void CountExtra() { ++g_count.extras; }
void ResetCounters() { g_count = {}; }
}  // namespace behaviour
}  // namespace melange::weapons
