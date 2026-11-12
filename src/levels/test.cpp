// The Erg Test workspace in the game: Test registration queued to the frontend, the one-shot override and the Test
// state events. The events are queued and fired on the main thread.
#include "levels/test.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "erg/names.h"
#include "erg/scene.h"
#include "levels/engine.h"
#include "levels/registry.h"
#include "lua/engine50.h"
#include "melange/gamestate.h"
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
bool g_ctxSub = false;
uint32_t g_attract = 0;
bool g_todSet = false;
std::string g_todOld;
uint64_t g_todPinUntil = 0;        // a pack level's pin holds the value until the match VM is made (or this, a fallback)
TodApplied g_todLast;   // under g_mx
std::string g_wormsKey;                // SetTeamWorms; under g_mx
std::array<uint8_t, 4> g_worms{};
bool g_wormsDue = false;               // the override for g_wormsKey was taken; written again at the VM's creation
std::string g_schemeKey, g_schemeName, g_schemeLua;   // SetScheme; under g_mx
bool g_schemeDue = false;              // the override for g_schemeKey was taken; run at the VM's creation
SchemeApplied g_schemeLast;            // under g_mx

bool Fail(char* err, size_t n, const std::string& why) {
    if (err && n) snprintf(err, n, "%s", why.c_str());
    return false;
}

void Queue(TestState s, const std::string& key, const std::string& detail) { g_events.push_back({s, key, detail}); }

// GM.GameInitData (0x190): NumberOfTeams u32 at +0x14, each team's NumOfWorms u32 at kWormsAt (re/notes gamestate-m3).
constexpr uint32_t kWormsAt[4] = {0x34, 0x9c, 0xfc, 0x154};

bool PokeWorms(uintptr_t init, const std::array<uint8_t, 4>& worms, uint32_t* teams, uint32_t was[4]) {
    __try {
        *teams = *reinterpret_cast<uint32_t*>(init + 0x14);
        if (*teams < 1 || *teams > 4) return false;
        for (uint32_t i = 0; i < *teams; ++i) {
            uint32_t* n = reinterpret_cast<uint32_t*>(init + kWormsAt[i]);
            was[i] = *n;
            if (worms[i]) *n = worms[i];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Main thread, under g_mx.
void WriteTeamWorms(const char* when) {
    gamestate::Var v;
    uintptr_t init = 0;
    if (gamestate::Var1("GM.GameInitData", &v) && v.type == gamestate::VarType::Container) {
        const char* a = std::strstr(v.value, "\"addr\":");
        if (a) init = static_cast<uintptr_t>(std::strtoull(a + 7, nullptr, 10));
    }
    uint32_t teams = 0, was[4] = {};
    if (!init || !PokeWorms(init, g_worms, &teams, was)) {
        LOG_WARN("[levels] test: %s: the worms per team could not be set (GM.GameInitData unreadable); the Quick Game's own count plays", when);
        return;
    }
    LOG_INFO("[levels] test: %s: worms per team %u,%u,%u,%u for %u teams (were %u,%u,%u,%u)", when, g_worms[0], g_worms[1], g_worms[2],
             g_worms[3], teams, was[0], was[1], was[2], was[3]);
}

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
bool ClearTod() {
    std::string old;
    if (!engine::GetString(kTodName, &old) || !engine::SetString(kTodName, "")) {
        LOG_WARN("[levels] test: the time of day could not be cleared; the frontend's value loads");
        return false;
    }
    g_todSet = true;
    g_todOld = old;
    return true;
}

void RestoreTod() {
    if (!g_todSet) return;
    g_todSet = false;
    g_todPinUntil = 0;
    const bool ok = engine::SetString(kTodName, g_todOld.c_str());
    LOG_INFO("[levels] test: %s restored to '%s': %s", kTodName, g_todOld.c_str(), ok ? "ok" : "FAILED");
}

// The Test's game style, in the VM that was just made: its own loadbuffer and pcall (a Lua error stays inside the pcall).
// Main thread, under g_mx.
void ApplyScheme(lua50::State* L) {
    SchemeApplied s;
    s.key = g_taken;
    s.name = g_schemeName;
    if (!L || !lua50::Check()) {
        s.error = "the engine's Lua is not available";
    } else {
        const auto& a = lua50::A();
        const int top = a.gettop(L);
        if (a.loadbuffer(L, g_schemeLua.data(), g_schemeLua.size(), "=testScheme") || a.pcall(L, 0, 0, 0)) {
            const char* e = a.tostring(L, -1);
            s.error = e ? e : "the chunk failed";
        } else {
            s.applied = true;
        }
        a.settop(L, top);
    }
    if (s.applied) LOG_INFO("[levels] test: game style %s set in GM.SchemeData for %s", s.name.c_str(), s.key.c_str());
    else LOG_WARN("[levels] test: the game style %s could not be set: %s; the Quick Game's own plays", s.name.c_str(), s.error.c_str());
    jlog::Rec("levels", s.applied ? jlog::Level::Info : jlog::Level::Warn, "test_scheme").Str("key", s.key).Str("name", s.name).Str("error", s.error);
    g_schemeLast = s;
}

// The match VM is created after SetUpLevelData has read the time of day: the frontend's value can go back now, before
// anything else reads it. OnFrame's restore stays as the fallback.
void OnContext(bool created, lua50::State* L, void*) {
    if (!created) return;
    std::lock_guard lk(g_mx);
    RestoreTod();
    if (g_wormsDue) {
        g_wormsDue = false;
        WriteTeamWorms("the match VM");
    }
    if (g_schemeDue) {
        g_schemeDue = false;
        ApplyScheme(L);
    }
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
    // a game level (a baseline Test) is checked against the frontend's levels when it is taken (Take)
    if (!mod && !Pending(key) && !(o.vanilla && std::strncmp(key, "Multi.", 6) == 0)) return false;
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

TodApplied LastTod() {
    std::lock_guard lk(g_mx);
    return g_todLast;
}

void SetTeamWorms(const std::string& key, const std::array<uint8_t, 4>& worms) {
    std::lock_guard lk(g_mx);
    g_wormsKey = key;
    g_worms = worms;
    g_wormsDue = false;
}

void SetScheme(const std::string& key, const std::string& name, const std::string& lua) {
    std::lock_guard lk(g_mx);
    g_schemeKey = key;
    g_schemeName = name;
    g_schemeLua = lua;
    g_schemeDue = false;
}

SchemeApplied LastScheme() {
    std::lock_guard lk(g_mx);
    return g_schemeLast;
}

// A pack level's pinned time of day (spice.json levels[].timeOfDay), at its level set-up in any match, Versus and
// network games too (every peer has the same pack, so the same pin): the frontend's value, which Versus picks at random,
// is replaced for the set-up's read and put back when the match VM is made, as a Test's is. "level" clears it, so the
// built level's own Databank.TimeOfDay loads.
void PinTod(const char* key, const std::string& tod) {
    std::lock_guard lk(g_mx);
    if (g_todSet || tod.empty()) return;   // a Test's own time of day is in already
    std::string old;
    const char* want = tod == "level" ? "" : tod.c_str();
    if (!engine::GetString(kTodName, &old) || !engine::SetString(kTodName, want)) {
        LOG_WARN("[levels] %s: its time of day (%s) could not be pinned; the frontend's value loads", key ? key : "?", tod.c_str());
        return;
    }
    g_todSet = true;
    g_todOld = old;
    g_todPinUntil = GetTickCount64() + 120000;
    LOG_INFO("[levels] %s: time of day pinned to %s (the frontend had '%s')", key ? key : "?", tod.c_str(), old.c_str());
    jlog::Rec("levels", jlog::Level::Info, "tod_pin").Str("key", key ? key : "").Str("tod", tod).Str("frontend", old);
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
        TodApplied t;
        t.key = k;
        t.asked = TodName(tod);
        if (tod == Tod::Default) t.why = "no time of day asked: the frontend's choice loads (Quick Game: DAY)";
        else if (online) t.why = "a network session: the frontend's time of day loads";
        else if (g_todSet || ClearTod()) t.applied = true;
        else t.why = "the frontend's time of day could not be cleared: its value loads";
        if (!t.applied && tod != Tod::Default) LOG_WARN("[levels] test: %s asked for %s: %s", k.c_str(), t.asked.c_str(), t.why.c_str());
        g_todLast = t;
        if (!g_wormsKey.empty() && g_wormsKey == k) {
            WriteTeamWorms("the level set-up");
            g_wormsDue = true;
            g_wormsKey.clear();
        }
        g_schemeLast = SchemeApplied{};
        if (!g_schemeKey.empty() && g_schemeKey == k) {
            g_schemeDue = true;
            g_schemeKey.clear();
        }
    }
    snprintf(g_taken, sizeof g_taken, "%s", k.c_str());
    return g_taken;
}

void OnFrame(bool atFrontend) {
    if (!g_startSub) g_startSub = engine::OnStartGame(&OnStartGame, nullptr);
    if (!g_ctxSub) {
        g_ctxSub = true;
        if (lua50::Track()) lua50::OnContext(&OnContext, nullptr);
    }
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
        if (g_todSet && g_ovr.phase() != Override::Phase::Starting && GetTickCount64() >= g_todPinUntil) RestoreTod();
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
