#include "launcher/autobuild.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/thread_guard.h"
#include "erg/stamp.h"
#include "launcher/app.h"
#include "launcher/rpc.h"
#include "launcher/util.h"
#include "melange/oasis.h"
#include "oasis/standalone/level_provider.h"
#include "oasis/standalone/mods_provider.h"
#include "oasis/standalone/register.h"
#include "store/store.h"
#include "tools/json_mini.h"
#include "version.h"

namespace melange::launcher::autobuild {
namespace {
namespace sa = oasis::standalone;
using Clock = std::chrono::steady_clock;

struct Row {
    std::string id, name, state, error;   // state: built missing stale failed queued building
};
struct Todo {
    std::string id, name, inputs;
    std::wstring dir;
};

std::mutex g_mx;   // guards everything below but the atomics
std::vector<Row> g_rows;
bool g_enabled = true;
int g_step = 0, g_of = 0;
std::string g_current;
std::function<void()> g_launch;
Clock::time_point g_launchSince;
std::wstring g_lastDir;

std::atomic<bool> g_due{false}, g_running{false}, g_finished{false};
oasis::ChannelId g_channel = 0;
// A launch that waits this long without a batch starting (the folder is busy) starts anyway.
constexpr auto kLaunchWait = std::chrono::seconds(10);

bool Enabled(const std::wstring& game) { return sa::IniGet(game, "Erg", "AutoBuildMaps", "1") != "0"; }

// Every enabled map pack that ships levels as source, with its state; `todo` gets the ones to build now (unbuilt or
// stale; a failed one is retried only when its inputs change, or by the Build button).
std::vector<Row> Scan(const std::wstring& game, std::vector<Todo>* todo) {
    std::vector<Row> rows;
    for (const auto& m : sa::modsprov::Enabled(game, MELANGE_VERSION)) {
        if (!m.content || !erg::stamp::HasSources(m)) continue;
        const std::wstring dir = game + L"\\Mods\\" + Widen(m.id);
        std::string inputs, why;
        const erg::stamp::State st = erg::stamp::Check(dir, m, MELANGE_VERSION, game, &inputs, &why);
        Row r{m.id, m.name.empty() ? m.id : m.name, erg::stamp::Name(st), st == erg::stamp::State::Built ? "" : why};
        if (st == erg::stamp::State::Missing || st == erg::stamp::State::Stale) {
            if (todo) todo->push_back({m.id, r.name, inputs, dir});
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

std::string StatusLocked() {
    jsonmini::Arr packs;
    for (const Row& r : g_rows) {
        jsonmini::Obj o;
        o.Str("id", r.id).Str("name", r.name).Str("state", r.state);
        if (!r.error.empty()) o.Str("error", r.error);
        packs.Raw(o.End());
    }
    return jsonmini::Obj()
        .Bool("enabled", g_enabled)
        .Bool("active", g_running.load())
        .Int("step", g_step)
        .Int("of", g_of)
        .Str("current", g_current)
        .Bool("waitingLaunch", static_cast<bool>(g_launch))
        .Raw("packs", packs.End())
        .End();
}

void Publish() {
    if (!g_channel || !oasis::HasSubscribers(g_channel)) return;
    std::string s;
    {
        std::lock_guard lk(g_mx);
        s = StatusLocked();
    }
    oasis::Publish(g_channel, "{\"status\":" + s + "}");
}

void SetRow(const std::string& id, const std::string& state, const std::string& error) {
    std::lock_guard lk(g_mx);
    for (Row& r : g_rows)
        if (r.id == id) r.state = state, r.error = error;
}

void Run(std::wstring game) {
    std::unique_lock tx(app::Tx(), std::try_to_lock);   // no setup change or plugin write in the same folder meanwhile
    if (!tx.owns_lock()) {
        g_due = true;
        g_running = false;
        return;
    }
    std::vector<Todo> todo;
    std::vector<Row> rows = Scan(game, &todo);
    {
        std::lock_guard lk(g_mx);
        g_rows = rows;
        for (Row& r : g_rows)
            for (const Todo& t : todo)
                if (r.id == t.id) r.state = "queued";
        g_of = static_cast<int>(todo.size());
        g_step = 0;
    }
    if (todo.empty()) {
        g_running = false;
        g_finished = true;
        Publish();
        return;
    }
    LOG_INFO("[maps] %zu map pack(s) to build", todo.size());
    int built = 0;
    for (size_t i = 0; i < todo.size(); ++i) {
        const Todo& t = todo[i];
        {
            std::lock_guard lk(g_mx);
            g_step = static_cast<int>(i) + 1;
            g_current = t.name;
        }
        SetRow(t.id, "building", "");
        Publish();
        const auto t0 = Clock::now();
        std::string reply, err;
        // The game may have started since the check: the service refuses to write then (Mods is read-only).
        const bool ok = app::WriteGate().empty() ? sa::levelprov::Build(t.id, &reply, &err) : (err = "the game is running", false);
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        if (ok) {
            ++built;
            SetRow(t.id, "built", "");
            LOG_INFO("[maps] built %s (%zu/%zu) in %lld ms", t.id.c_str(), i + 1, todo.size(), ms);
        } else {
            SetRow(t.id, "failed", err);
            LOG_WARN("[maps] could not build %s: %s", t.id.c_str(), err.c_str());
            // Remembered with today's inputs so the next start does not try again; a change to the pack, Melange or
            // the game (new inputs), or the Build button, does. Not when the game simply started meanwhile.
            if (err != "the game is running" && !t.inputs.empty()) {
                erg::stamp::Record rec;
                rec.ok = false;
                rec.inputs = t.inputs;
                rec.melange = MELANGE_VERSION;
                rec.error = err;
                rec.at = erg::stamp::Now();
                erg::stamp::Write(t.dir, rec);
            }
        }
    }
    LOG_INFO("[maps] %d of %zu map pack(s) built", built, todo.size());
    {
        std::lock_guard lk(g_mx);
        g_current.clear();
    }
    tx.unlock();
    g_running = false;
    g_finished = true;
    Publish();
}

void Status(const oasis::Call&, oasis::Result& r, void*) {
    const std::wstring game = app::GameDir();
    if (!g_running && !game.empty()) {
        std::vector<Row> rows = Scan(game, nullptr);
        std::lock_guard lk(g_mx);
        g_rows = std::move(rows);
        g_enabled = Enabled(game);
    }
    std::lock_guard lk(g_mx);
    r.json = StatusLocked();
}

void OnSub(oasis::ChannelId ch, int client, std::string_view, bool subscribed, void*) {
    if (!subscribed) return;
    std::string s;
    {
        std::lock_guard lk(g_mx);
        s = StatusLocked();
    }
    oasis::PublishTo(ch, client, "{\"status\":" + s + "}");
}

void RunLaunch() {
    std::function<void()> fn;
    {
        std::lock_guard lk(g_mx);
        fn = std::move(g_launch);
        g_launch = nullptr;
    }
    if (fn) fn();
}
}  // namespace

void Install() {
    g_channel = oasis::AddChannel("maps");
    oasis::OnSubscribe(g_channel, &OnSub, nullptr);
    oasis::AddMethod("maps.status", &Status, nullptr, oasis::kRpcServerThread);
    Request("start");
}

void Request(const char* why) {
    LOG_INFO("[maps] check requested (%s)", why);
    g_due = true;
}

bool Busy() { return g_running.load(); }

void Tick() {
    if (g_finished.exchange(false)) RunLaunch();
    const std::wstring game = app::GameDir();
    {
        std::lock_guard lk(g_mx);
        if (PathKey(game) != PathKey(g_lastDir)) {
            g_lastDir = game;
            g_due = true;
        }
    }
    bool waiting;
    Clock::time_point since;
    {
        std::lock_guard lk(g_mx);
        waiting = static_cast<bool>(g_launch);
        since = g_launchSince;
    }
    if (waiting && !g_running && Clock::now() - since > kLaunchWait) {
        LOG_WARN("[maps] map packs could not be built yet; starting the game anyway");
        g_due = false;
        RunLaunch();
        Publish();
        return;
    }
    if (!g_due || g_running) return;
    if (game.empty() || !DirExists(game)) {
        g_due = false;
        return;
    }
    {
        std::lock_guard lk(g_mx);
        g_enabled = Enabled(game);
    }
    if (!Enabled(game)) {
        g_due = false;
        if (waiting) RunLaunch();
        return;
    }
    if (PathKey(sa::levelprov::GameDir()) != PathKey(game)) {   // the level service is off, or bound to another folder
        g_due = false;
        if (waiting) RunLaunch();
        return;
    }
    // The game running, a setup change, an import or a Store job: wait (a Store install asks again when it ends).
    {
        static std::string lastWait;
        const std::string gate = app::CachedGate();
        const std::string wait = !gate.empty() ? gate : rpc::ImportRunning() ? "an import is running" : store::GetStatus().busy ? "a Store job is running" : "";
        if (wait != lastWait && !wait.empty()) LOG_INFO("[maps] the check waits: %s", wait.c_str());
        lastWait = wait;
        if (!wait.empty()) return;
    }
    g_due = false;
    g_running = true;
    std::thread([game] { GuardedThreadBody("maps-build", [&] { Run(game); }); }).detach();
}

bool DeferLaunch(std::function<void()> start) {
    const std::wstring game = app::GameDir();
    if (game.empty() || !Enabled(game) || PathKey(sa::levelprov::GameDir()) != PathKey(game)) return false;
    if (!g_running) {
        if (!app::WriteGate().empty()) return false;   // nothing can be built in this folder now
        std::vector<Todo> todo;
        std::vector<Row> rows = Scan(game, &todo);
        if (todo.empty()) return false;
        g_due = true;
    }
    {
        std::lock_guard lk(g_mx);
        g_launch = std::move(start);
        g_launchSince = Clock::now();
    }
    LOG_INFO("[maps] the game starts once the map packs are built");
    Publish();
    return true;
}

void LaunchDone(bool ok, const std::string& error) {
    if (!ok) LOG_WARN("[maps] the game did not start after the map builds: %s", error.c_str());
    if (!g_channel) return;
    jsonmini::Obj o;
    o.Bool("ok", ok);
    if (!ok) o.Str("error", error);
    oasis::Publish(g_channel, jsonmini::Obj().Raw("launch", o.End()).End());
    Publish();
}

std::string StatusJson() {
    std::lock_guard lk(g_mx);
    return StatusLocked();
}
}  // namespace melange::launcher::autobuild
