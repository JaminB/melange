// The Erg Test workspace in the game: Test registration queued to the frontend, the one-shot override and the Test
// state events. The events are queued and fired on the main thread.
#include "levels/test.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "erg/names.h"
#include "erg/scene.h"
#include "levels/engine.h"
#include "levels/registry.h"
#include "melange/jlog.h"
#include "melange/sim.h"
#include "mods/lobby.h"
#include "net/net.h"

namespace melange::levels::test {
namespace {
struct Req {
    std::string stem, title;
};
struct Ev {
    TestState s;
    std::string key, detail;
};

constexpr const char* kTodName = "WXD.Level.TimeOfDay";

std::mutex g_mx;
std::vector<Req> g_queue;
std::vector<Ev> g_events;
Override g_ovr;
char g_taken[128] = {};
int g_startSub = 0;
uint32_t g_attract = 0;
bool g_todSet = false;
std::string g_todOld;

bool Fail(char* err, size_t n, const std::string& why) {
    if (err && n) snprintf(err, n, "%s", why.c_str());
    return false;
}

void Queue(TestState s, const std::string& key, const std::string& detail) { g_events.push_back({s, key, detail}); }

bool NetSession() {
    if (!wum::NetService()) return false;
    const uintptr_t st = wum::CurrentState();
    return st == wum::state::WaitingSimChannel || st == wum::state::WaitingGameStart ||
           st == wum::state::WaitingConnections;
}

void Flush() {
    std::vector<Ev> evs;
    {
        std::lock_guard lk(g_mx);
        evs.swap(g_events);
    }
    for (const auto& e : evs) {
        LOG_INFO("[levels] test %s %s%s%s", StateName(e.s), e.key.c_str(), e.detail.empty() ? "" : ": ", e.detail.c_str());
        jlog::Rec("levels", e.s == TestState::Failed ? jlog::Level::Warn : jlog::Level::Info, "test_state")
            .Str("state", StateName(e.s)).Str("key", e.key).Str("detail", e.detail);
        internal::FireTestState(e.s, e.key.c_str(), e.detail.c_str());
    }
}

void OnStartGame(const char* text, void*) {
    std::lock_guard lk(g_mx);
    g_ovr.NoteStartGame(text ? text : "");
}

// The Test build's Databank.TimeOfDay decides while the frontend's value is empty; it is put back once the match runs.
void ClearTod() {
    std::string old;
    if (!engine::GetString(kTodName, &old) || !engine::SetString(kTodName, "")) {
        LOG_WARN("[levels] test: the time of day could not be cleared; the frontend's value loads");
        return;
    }
    g_todSet = true;
    g_todOld = old;
}

void RestoreTod() {
    if (!g_todSet) return;
    g_todSet = false;
    const bool ok = engine::SetString(kTodName, g_todOld.c_str());
    LOG_INFO("[levels] test: %s restored to '%s': %s", kTodName, g_todOld.c_str(), ok ? "ok" : "FAILED");
}
}  // namespace

bool Register(const char* stem, const char* title, char* err, size_t errLen) {
    const std::string s = stem ? stem : "", t = title ? title : "";
    std::string why;
    if (!erg::names::ValidStem(s, erg::names::kTestPrefix, &why)) return Fail(err, errLen, why);
    if (!erg::PrintableAscii(t, 1, 40)) return Fail(err, errLen, "the title must be 1-40 printable ASCII characters");
    std::lock_guard lk(g_mx);
    if (g_queue.size() >= 16) return Fail(err, errLen, "too many Test registrations are waiting");
    g_queue.push_back({s, t});
    Queue(TestState::Registering, erg::names::Key(s), "");
    return true;
}

bool Pending(const std::string& key) {
    std::lock_guard lk(g_mx);
    for (const auto& r : g_queue)
        if (erg::names::Key(r.stem) == key) return true;
    return false;
}

bool Arm(const char* key, const ArmOptions& o) {
    if (!key || !*key) return false;
    Source src = Source::Vanilla;
    const bool mod = registry::Lookup(key, &src);
    if (!mod && !Pending(key)) return false;
    if (NetSession() || handshake::lobby::Current()) return false;
    std::lock_guard lk(g_mx);
    if (!g_ovr.Arm(key, o.timeoutS, GetTickCount64(), o.tod)) return false;
    Queue(TestState::Armed, key, "");
    return true;
}

void Disarm(const char* why) {
    std::lock_guard lk(g_mx);
    if (!g_ovr.armed()) return;
    const std::string key = g_ovr.key();
    g_ovr.Disarm();
    Queue(TestState::Idle, key, why ? why : "");
}

void Fail(const char* detail) {
    std::lock_guard lk(g_mx);
    if (!g_ovr.armed()) return;
    const std::string key = g_ovr.key();
    g_ovr.Disarm();
    if (detail && std::string(detail) == kAttractRefused) ++g_attract;
    Queue(TestState::Failed, key, detail ? detail : "");
}

bool Armed(char* key, size_t keyLen) {
    std::lock_guard lk(g_mx);
    if (key && keyLen) snprintf(key, keyLen, "%s", g_ovr.armed() ? g_ovr.key().c_str() : "");
    return g_ovr.armed();
}

bool Busy() {
    std::lock_guard lk(g_mx);
    return g_ovr.phase() != Override::Phase::Idle;
}

uint32_t AttractRefusals() {
    std::lock_guard lk(g_mx);
    return g_attract;
}

const char* Take(const char* frontendKey) {
    const engine::FrontendState fs = engine::ReadFrontend();
    Override::SetUp s;
    s.attract = fs.valid && fs.attractRunning;
    s.loading = engine::LoadingAtSetUp();
    const bool online = handshake::lobby::Current() || NetSession();
    std::lock_guard lk(g_mx);
    Override::Event ev = Override::Event::None;
    const std::string armedKey = g_ovr.key();
    const Tod tod = g_ovr.tod();
    const std::string k = g_ovr.Take(frontendKey ? frontendKey : "", GetTickCount64(), &ev, s);
    if (ev == Override::Event::Expired) Queue(TestState::Idle, "", "timed out");
    if (ev == Override::Event::AttractRefused) {
        ++g_attract;
        Queue(TestState::Failed, armedKey, kAttractRefused);
    }
    if (k.empty()) return nullptr;
    if (!engine::LevelDetails(k.c_str(), nullptr)) {
        g_ovr.Disarm();
        Queue(TestState::Failed, k, "the level is not registered; the frontend's choice loads");
        return nullptr;
    }
    if (ev == Override::Event::Started) {
        Queue(TestState::Starting, k, frontendKey ? frontendKey : "");
        if (tod != Tod::Default && !online && !g_todSet) ClearTod();
    }
    snprintf(g_taken, sizeof g_taken, "%s", k.c_str());
    return g_taken;
}

void OnFrame(bool atFrontend) {
    if (!g_startSub) g_startSub = engine::OnStartGame(&OnStartGame, nullptr);
    if (handshake::lobby::Current() || NetSession()) Disarm("a network session started");
    {
        std::lock_guard lk(g_mx);
        const std::string key = g_ovr.key();
        switch (g_ovr.Update(sim::InMatch(), GetTickCount64())) {
            case Override::Event::Expired: Queue(TestState::Idle, key, "timed out"); break;
            case Override::Event::Playing: Queue(TestState::Playing, key, ""); break;
            case Override::Event::Ended: Queue(TestState::Ended, key, ""); break;
            case Override::Event::Abandoned: Queue(TestState::Failed, key, "the match did not start"); break;
            default: break;
        }
        if (g_todSet && g_ovr.phase() != Override::Phase::Starting) RestoreTod();
    }
    if (atFrontend) {
        std::vector<Req> todo;
        {
            std::lock_guard lk(g_mx);
            todo.swap(g_queue);
        }
        for (const auto& r : todo) {
            std::string err;
            const std::string key = erg::names::Key(r.stem);
            const bool ok = registry::RegisterTestLevel(r.stem, r.title, &err);
            std::lock_guard lk(g_mx);
            Queue(ok ? TestState::Registered : TestState::Failed, key, ok ? r.title : err);
            if (!ok && g_ovr.armed() && g_ovr.key() == key) g_ovr.Disarm();
        }
    }
    Flush();
}
}  // namespace melange::levels::test
