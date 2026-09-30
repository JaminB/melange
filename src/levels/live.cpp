// Map packs enabled or disabled at the menu, offline only. A call off the main thread is queued to the next frame.
#include "levels/live.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/log.h"
#include "levels/engine.h"
#include "levels/registry.h"
#include "levels/test.h"
#include "melange/levels.h"
#include "mods/lobby.h"
#include "mods/thumper_internal.h"
#include "net/net.h"

namespace melange::levels {
namespace {
struct Observer {
    int handle;
    PacksChangedFn fn;
    void* user;
};
struct Req {
    std::string mod;
    bool on;
};

std::mutex g_mx;
std::vector<Observer> g_obs;
std::vector<Req> g_queue;
int g_next = 1;

bool NetSession() {
    if (!wum::NetService()) return false;
    const uintptr_t st = wum::CurrentState();
    return st == wum::state::WaitingSimChannel || st == wum::state::WaitingGameStart || st == wum::state::WaitingConnections;
}

live::Session Now() {
    live::Session s;
    s.ini = registry::Settings().livePacks;
    s.enabled = Enabled();
    s.inLobby = handshake::lobby::Current() != 0;
    s.netSession = NetSession();
    s.atFrontend = engine::AtFrontend();
    const engine::FrontendState fs = engine::ReadFrontend();
    s.attract = fs.valid && fs.attractRunning;
    s.loading = engine::Loading();
    s.testBusy = test::Busy();
    return s;
}

bool Fail(char* err, size_t n, const std::string& why) {
    if (err && n) snprintf(err, n, "%s", why.c_str());
    return false;
}

void Fire() {
    std::vector<Observer> obs;
    {
        std::lock_guard lk(g_mx);
        obs = g_obs;
    }
    for (const auto& o : obs) o.fn(o.user);
}

bool Change(const std::string& mod, bool on, std::string* err) {
    std::string why = live::SessionRefusal(Now());
    if (why.empty() && !registry::PacksReady()) why = "the game is still starting; try again in a moment";
    thumper::Entry e;
    if (why.empty() && !thumper::FindEntry(mod, &e)) why = "no mod " + mod + " is installed";
    if (why.empty() && on) why = live::PackRefusal(e.manifest);
    if (why.empty() && !(on ? registry::EnableLive(mod, &why) : registry::DisableLive(mod, &why)) && why.empty())
        why = "the change failed; see Melange.log";
    if (!why.empty()) {
        LOG_WARN("[levels] live %s %s refused: %s", on ? "enable" : "disable", mod.c_str(), why.c_str());
        *err = why;
        return false;
    }
    thumper::SetLive(mod, on);
    Fire();
    return true;
}

void OnFrame() {
    std::vector<Req> todo;
    {
        std::lock_guard lk(g_mx);
        todo.swap(g_queue);
    }
    for (const auto& r : todo) {
        std::string err;
        Change(r.mod, r.on, &err);
    }
}

bool Request(const char* modId, bool on, char* err, size_t errLen) {
    if (!modId || !*modId) return Fail(err, errLen, "no mod id");
    if (GetCurrentThreadId() == events::MainThreadId()) {
        std::string why;
        return Change(modId, on, &why) || Fail(err, errLen, why);
    }
    std::lock_guard lk(g_mx);
    if (g_queue.size() >= 16) return Fail(err, errLen, "too many pack changes are waiting");
    g_queue.push_back({modId, on});
    Fail(err, errLen, "queued");
    return true;
}
}  // namespace

namespace live {
void Install() { events::Subscribe(events::Event::Frame, &OnFrame); }
}  // namespace live

bool EnablePackLive(const char* modId, char* err, size_t errLen) { return Request(modId, true, err, errLen); }
bool DisablePackLive(const char* modId, char* err, size_t errLen) { return Request(modId, false, err, errLen); }

int OnPacksChanged(PacksChangedFn fn, void* user) {
    if (!fn) return 0;
    std::lock_guard lk(g_mx);
    g_obs.push_back({g_next, fn, user});
    return g_next++;
}

void RemoveOnPacksChanged(int handle) {
    std::lock_guard lk(g_mx);
    std::erase_if(g_obs, [handle](const Observer& o) { return o.handle == handle; });
}
}  // namespace melange::levels
