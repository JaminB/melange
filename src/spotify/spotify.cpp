// The Spotify module (Melange 0.13.0): wum.spotify's host (host.h). It holds the sign-in, runs the browser sign-in
// (PKCE, a loopback listener), and sends every mod's Web API requests through one worker with a shared rate gate.
// Presentation only, client VM only: nothing here reaches the match, the handshake or Wormsign.
//
// Network: nothing is sent until a mod's connect() runs on a user click (the binding checks it), except the token
// refresh for an account the user already linked. Hosts: accounts.spotify.com and api.spotify.com only, https only,
// redirects never followed. Tokens never reach Lua and are never logged; the refresh token is kept DPAPI-encrypted
// (token_store.h), the access token in memory only.
//
// Threads: the main thread (host.h, the Frame pump), "spotify-auth" (one per sign-in: the listener and the code
// exchange) and "spotify-api" (the request worker, started on the first request). They share Shared (below) under
// one mutex; results reach Lua only from the main thread's pump. Lock order: saveMx, then mx. Nothing is joined, and no
// thread is started once the process is exiting (Melange 0.12.2: Windows creates no threads then).
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "core/thread_guard.h"
#include "net/peerbus.h"
#include "spotify/config_keys.h"
#include "spotify/host.h"
#include "spotify/oauth_listener.h"
#include "spotify/spotify_logic.h"
#include "spotify/token_store.h"
#include "store/fetch.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::spotify {
namespace {
namespace fetch = store::fetch;
using logic::Err;
using logic::Op;
using logic::Request;

constexpr uint64_t kJobTimeoutMs = 15000;    // a request fails with "timeout" this long after Submit
constexpr uint64_t kFlowTimeoutMs = 180000;  // the browser sign-in
constexpr int kMaxPerMod = 8;                // queued or running, per mod
constexpr size_t kMaxQueued = 64;            // queued or running, all mods

uint64_t SteadyMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::atomic<bool> g_quit{false};

void Wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

fetch::Options HttpOptions() {
    fetch::Options o;
    o.cap = 1u << 20;
    o.totalMs = 10000;
    o.stallMs = 10000;
    o.registerActive = false;  // the Store's CancelActive leaves these alone
    o.cancel = &g_quit;
    return o;
}

bool TimedOut(const std::string& err) { return err.find("timed out") != std::string::npos; }

bool Has(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// ---------------------------------------------------------------- shared state (worker, auth thread, main thread)
struct Job {
    uint32_t rid = 0;
    std::string mod;
    uint32_t gen = 0;  // the mod generation that asked (host::CancelGen)
    uint32_t token = 0;
    Request req;
    uint64_t deadline = 0;   // SteadyMs
    bool background = false;  // the start-up refresh of the token and the profile
};

struct Result {
    enum class Kind { Answer, Revoked, Profile, Flow } kind = Kind::Answer;
    std::string mod;
    uint32_t gen = 0;
    uint32_t rid = 0, token = 0;
    std::string json;      // Answer: the callback's argument
    uint64_t mid = 0;      // Answer: SteadyMs midpoint of the HTTP call
    bool patchAt = false;  // Answer: a playback result whose "at" the pump sets
    uint64_t flowId = 0;   // Flow
    bool ok = false, cancelled = false;
    std::string error;     // Flow: the status error code
};

struct Shared {
    std::mutex mx;
    std::condition_variable cv;
    std::mutex saveMx;  // orders token-file writes (taken before mx, never while holding it)
    // the sign-in
    bool linked = false;
    tokens::Stored stored;
    uint64_t epoch = 1;  // bumped on every sign-in and sign-out, so a stale refresh is dropped
    std::string access;
    uint64_t accessExpires = 0;  // SteadyMs
    // requests
    std::deque<Job> control, read;
    std::map<std::string, int> perMod;  // queued or running
    size_t queued = 0;                   // queued or running, all mods
    uint32_t running = 0;
    std::string runningMod;  // the running job's mod and generation
    uint32_t runningGen = 0;
    bool runningDropped = false;
    int controlStreak = 0;  // control jobs taken in a row (logic::TakeControl)
    std::vector<Result> results;
    logic::RateGate gate{90};
};
// Never destroyed: no destructor of a mutex or condition variable runs while a detached thread may still use it.
Shared& S() {
    static Shared* s = new Shared();
    return *s;
}

// Writes the current sign-in, or nothing when signed out. saveMx orders the writes, so the file ends with the newest.
void SaveCurrent() {
    Shared& s = S();
    std::lock_guard sl(s.saveMx);
    tokens::Stored copy;
    bool linked = false;
    {
        std::lock_guard lk(s.mx);
        linked = s.linked;
        if (linked) copy = s.stored;
    }
    if (linked) tokens::Save(copy);
    Wipe(copy.refresh);
}

std::string AuthError() { return logic::ShapeError(Err::Auth, "Spotify is not connected", 0); }

void Push(Shared& s, Result r) { s.results.push_back(std::move(r)); }

void Answer(Shared& s, const Job& j, std::string json, uint64_t mid = 0, bool patchAt = false) {
    if (!j.token) return;
    Result r;
    r.mod = j.mod;
    r.gen = j.gen;
    r.rid = j.rid;
    r.token = j.token;
    r.json = std::move(json);
    r.mid = mid;
    r.patchAt = patchAt;
    Push(s, std::move(r));
}

void Uncount(Shared& s, const Job& j) {
    if (j.background) return;
    auto it = s.perMod.find(j.mod);
    if (it != s.perMod.end() && --it->second <= 0) s.perMod.erase(it);
    if (s.queued) --s.queued;
}

// Every queued request fails with "auth" (signed out or revoked). Caller holds mx.
void FailQueuedLocked(Shared& s) {
    for (std::deque<Job>* q : {&s.control, &s.read}) {
        for (const Job& j : *q) {
            if (j.background) continue;
            Uncount(s, j);
            Answer(s, j, AuthError());
        }
        q->clear();
    }
}

// Signed out by Spotify (invalid_grant): forget the sign-in unless it changed meanwhile. Worker thread.
void Revoke(uint64_t epoch) {
    Shared& s = S();
    {
        std::lock_guard sl(s.saveMx);
        {
            std::lock_guard lk(s.mx);
            if (s.epoch != epoch || !s.linked) return;
            s.linked = false;
            Wipe(s.stored.refresh);
            s.stored = {};
            Wipe(s.access);
            s.accessExpires = 0;
            ++s.epoch;
            FailQueuedLocked(s);
            Result r;
            r.kind = Result::Kind::Revoked;
            Push(s, std::move(r));
        }
        tokens::Delete();
    }
    LOG_WARN("[spotify] Spotify no longer accepts the stored sign-in (revoked); sign in again");
}

// Caps a call's limits to what is left of its request's time (logic::BudgetMs). False when nothing is left.
bool CapToDeadline(fetch::Options* o, uint64_t deadline) {
    const uint32_t left = logic::BudgetMs(SteadyMs(), deadline, o->totalMs);
    if (!left) return false;
    o->totalMs = left;
    o->stallMs = std::min(o->stallMs, left);
    return true;
}

// The access token, refreshed when less than 60 s are left (or `force`). Worker thread. A 429 from the token endpoint
// pauses the gate and is Err::Rate (*retryAfter set), not "auth": the account is still linked. A refresh for a request
// counts against that request's `deadline` (0: none).
Err Access(bool force, std::string* out, int* retryAfter = nullptr, uint64_t deadline = 0) {
    Shared& s = S();
    std::string refresh, clientId;
    uint64_t epoch = 0;
    {
        std::lock_guard lk(s.mx);
        if (!s.linked) return Err::Auth;
        if (!force && !logic::NeedsRefresh(SteadyMs(), s.accessExpires, !s.access.empty())) {
            *out = s.access;
            return Err::None;
        }
        refresh = s.stored.refresh;
        clientId = s.stored.clientId;
        epoch = s.epoch;
    }
    fetch::Options opts = HttpOptions();
    if (deadline && !CapToDeadline(&opts, deadline)) {
        Wipe(refresh);
        return Err::Timeout;
    }
    fetch::HttpRequest rq;
    rq.method = "POST";
    rq.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    rq.body = logic::TokenRefreshForm(refresh, clientId);
    Wipe(refresh);
    fetch::HttpResponse rs;
    std::string err;
    const bool sent = fetch::Send(logic::kTokenUrl, rq, &rs, opts, &err);
    Wipe(rq.body);
    if (!sent) return TimedOut(err) ? Err::Timeout : Err::Network;
    logic::Tokens t;
    std::string oe;
    const bool parsed = logic::ParseTokenResponse(rs.body, &t, &oe);  // also reads an error body (invalid_grant)
    const bool ok = rs.status == 200 && parsed;
    Wipe(rs.body);
    if (!ok) {
        Wipe(t.access);
        Wipe(t.refresh);
        if (oe == "invalid_grant") {
            Revoke(epoch);
            return Err::Auth;
        }
        LOG_WARN("[spotify] the token refresh failed (HTTP %d%s%s)", rs.status, oe.empty() ? "" : ", ", oe.c_str());
        if (rs.status == 429) {
            const int ra = logic::RetryAfterSeconds(rs.retryAfter);
            if (retryAfter) *retryAfter = ra;
            std::lock_guard lk(s.mx);
            s.gate.PauseUntil(SteadyMs() + static_cast<uint64_t>(ra) * 1000);
            return Err::Rate;
        }
        return rs.status >= 500 ? Err::Server : Err::Auth;
    }
    bool rotated = false;
    {
        std::lock_guard lk(s.mx);
        if (s.epoch != epoch || !s.linked) {
            Wipe(t.access);
            Wipe(t.refresh);
            return Err::Auth;  // signed out meanwhile
        }
        s.access = t.access;
        s.accessExpires = logic::ExpiresAtMs(SteadyMs(), t.expiresIn);
        if (!t.refresh.empty() && t.refresh != s.stored.refresh) {  // rotated: the old one may stop working
            s.stored.refresh = t.refresh;
            rotated = true;
        }
        *out = s.access;
    }
    if (rotated) SaveCurrent();
    Wipe(t.access);
    Wipe(t.refresh);
    return Err::None;
}

// GET /v1/me with an access token.
bool FetchMe(const std::string& access, logic::Me* me, bool* transport) {
    fetch::HttpRequest rq;
    rq.headers.emplace_back("Authorization", "Bearer " + access);
    fetch::HttpResponse rs;
    std::string err;
    {
        Shared& s = S();
        std::lock_guard lk(s.mx);
        s.gate.Take(SteadyMs());
    }
    const bool sent = fetch::Send(std::string(logic::kApiBase) + "me", rq, &rs, HttpOptions(), &err);
    Wipe(rq.headers[0].second);
    *transport = !sent;
    return sent && rs.status == 200 && logic::ParseMe(rs.body, me);
}

// ---------------------------------------------------------------- the worker ("spotify-api")
void SleepFor(uint64_t ms) {
    Shared& s = S();
    std::unique_lock lk(s.mx);
    s.cv.wait_for(lk, std::chrono::milliseconds(ms), [] { return g_quit.load(); });
}

void Finish(const Job& j, std::string json, uint64_t mid = 0, bool patchAt = false) {
    Shared& s = S();
    std::lock_guard lk(s.mx);
    Uncount(s, j);
    const bool dropped = s.running == j.rid && s.runningDropped;
    s.running = 0;
    s.runningMod.clear();
    s.runningGen = 0;
    s.runningDropped = false;
    if (!dropped) Answer(s, j, std::move(json), mid, patchAt);
}

// The start-up refresh: a fresh token and the account's name and product. Saves and reports a change.
void RunBackground() {
    Shared& s = S();
    uint64_t epoch = 0;
    {
        std::lock_guard lk(s.mx);
        epoch = s.epoch;
    }
    std::string access;
    if (Access(true, &access) != Err::None) return;
    logic::Me me;
    bool transport = false;
    const bool ok = FetchMe(access, &me, &transport);
    Wipe(access);
    if (!ok) return;
    bool changed = false;
    {
        std::lock_guard lk(s.mx);
        me.product = logic::ProductAtStartup(s.stored.product, me.product);  // not sent to Development-mode apps
        if (s.linked && s.epoch == epoch && (s.stored.user != me.name || s.stored.product != me.product || s.stored.userId != me.id ||
                         s.stored.country != me.country)) {
            s.stored.user = me.name;
            s.stored.product = me.product;
            s.stored.userId = me.id;
            s.stored.country = me.country;
            changed = true;
            Result r;
            r.kind = Result::Kind::Profile;
            Push(s, std::move(r));
        }
    }
    if (changed) SaveCurrent();
}

// A control call's answer tells the account's plan (logic::ProductAfter): saved, and a status event when it changed.
void LearnProduct(uint64_t epoch, Op op, bool ok, Err e) {
    Shared& s = S();
    {
        std::lock_guard lk(s.mx);
        if (!s.linked || s.epoch != epoch) return;
        std::string p = logic::ProductAfter(s.stored.product, op, ok, e);
        if (p == s.stored.product) return;
        s.stored.product = std::move(p);
        Result r;
        r.kind = Result::Kind::Profile;
        Push(s, std::move(r));
    }
    SaveCurrent();
}

void Run(const Job& j) {
    if (j.background) {
        RunBackground();
        return;
    }
    Shared& s = S();
    uint64_t epoch = 0;
    {
        std::lock_guard lk(s.mx);
        epoch = s.epoch;
    }
    std::string method, url, body;
    if (!logic::BuildCall(j.req, &method, &url, &body)) {
        Finish(j, logic::ShapeError(Err::BadRequest, "malformed request", 0));
        return;
    }
    logic::Attempts attempts;
    bool forceRefresh = false;
    for (;;) {
        if (g_quit) return;
        const uint64_t now = SteadyMs();
        if (now >= j.deadline) {
            Finish(j, logic::ShapeError(Err::Timeout, "no answer within 15 seconds", 0));
            return;
        }
        uint64_t wait = 0;
        {
            std::lock_guard lk(s.mx);
            wait = s.gate.WaitMs(now);
            if (!wait) s.gate.Take(now);
        }
        if (wait) {
            if (now + wait > j.deadline) {
                Finish(j, logic::ShapeError(Err::Rate, "too many Spotify requests", static_cast<int>(std::max<uint64_t>(1, (wait + 999) / 1000))));
                return;
            }
            SleepFor(wait);
            continue;
        }
        std::string access;
        int tokenRetry = 0;
        const Err aerr = Access(forceRefresh, &access, &tokenRetry, j.deadline);
        forceRefresh = false;
        if (aerr == Err::Timeout) {
            Finish(j, logic::ShapeError(Err::Timeout, "no answer within 15 seconds", 0));
            return;
        }
        if (aerr != Err::None) {
            Finish(j, logic::ShapeError(aerr, aerr == Err::Auth ? "Spotify is not connected" : "the Spotify sign-in could not be refreshed",
                                        tokenRetry));
            return;
        }
        fetch::Options opts = HttpOptions();
        if (!CapToDeadline(&opts, j.deadline)) {  // the refresh used up the request's time: it is not sent late
            Wipe(access);
            Finish(j, logic::ShapeError(Err::Timeout, "no answer within 15 seconds", 0));
            return;
        }
        fetch::HttpRequest rq;
        rq.method = method;
        rq.headers.emplace_back("Authorization", "Bearer " + access);
        Wipe(access);
        if (!body.empty()) {
            rq.headers.emplace_back("Content-Type", "application/json");
            rq.body = body;
        }
        fetch::HttpResponse rs;
        std::string err;
        const uint64_t t0 = SteadyMs();
        const bool sent = fetch::Send(url, rq, &rs, opts, &err);
        const uint64_t t1 = SteadyMs();
        Wipe(rq.headers[0].second);
        if (!sent) {
            if (g_quit) return;
            Finish(j, logic::ShapeError(TimedOut(err) ? Err::Timeout : Err::Network, err, 0));
            return;
        }
        switch (logic::Decide(rs.status, &attempts)) {
            case logic::Action::Shape: {
                std::string out;
                if (!logic::ShapeSuccess(j.req, rs.status, rs.body, &out)) {
                    Finish(j, logic::ShapeError(Err::BadResponse, "Spotify's answer could not be read", 0));
                } else {
                    LearnProduct(epoch, j.req.op, true, Err::None);
                    Finish(j, std::move(out), (t0 + t1) / 2, j.req.op == Op::Playback);
                }
                return;
            }
            case logic::Action::RefreshRetry: forceRefresh = true; continue;
            case logic::Action::RateRetry: {
                const int ra = logic::RetryAfterSeconds(rs.retryAfter);
                std::lock_guard lk(s.mx);
                s.gate.PauseUntil(SteadyMs() + static_cast<uint64_t>(ra) * 1000);
                continue;  // the gate waits, or fails with "rate" when the wait passes the deadline
            }
            case logic::Action::ServerRetry:
                if (SteadyMs() + 1000 >= j.deadline) break;
                SleepFor(1000);
                continue;
            case logic::Action::Fail: break;
        }
        std::string message;
        const Err e = logic::MapApiError(rs.status, rs.body, &message);
        LearnProduct(epoch, j.req.op, false, e);
        Finish(j, logic::ShapeError(e == Err::None ? Err::BadResponse : e, message, logic::RetryAfterSeconds(rs.retryAfter)));
        return;
    }
}

// The next job. Queued requests whose 15 s passed fail with "timeout" here, so a waiting one never outlives its deadline
// unanswered; control goes first, but a waiting read gets every few turns (logic::TakeControl).
bool Next(Job* out) {
    Shared& s = S();
    std::unique_lock lk(s.mx);
    for (;;) {
        s.cv.wait(lk, [&] { return g_quit.load() || !s.control.empty() || !s.read.empty(); });
        if (g_quit) return false;
        const uint64_t now = SteadyMs();
        for (std::deque<Job>* q : {&s.control, &s.read})
            logic::ExpireQueued(*q, now, [&](const Job& j) {
                Uncount(s, j);
                Answer(s, j, logic::ShapeError(Err::Timeout, "no answer within 15 seconds", 0));
            });
        if (s.control.empty() && s.read.empty()) continue;
        std::deque<Job>& q = logic::TakeControl(!s.control.empty(), !s.read.empty(), &s.controlStreak) ? s.control : s.read;
        *out = std::move(q.front());
        q.pop_front();
        s.running = out->rid;
        s.runningMod = out->mod;
        s.runningGen = out->gen;
        s.runningDropped = false;
        return true;
    }
}

void WorkerLoop() {
    Job j;
    while (Next(&j)) {
        try {
            Run(j);
        } catch (const std::exception& e) {
            LOG_ERROR("[spotify] a request failed: %s", e.what());
            if (!j.background) Finish(j, logic::ShapeError(Err::Network, "internal error", 0));
        }
        j = Job{};
    }
}

bool g_workerStarted = false;  // main thread
bool EnsureWorker() {
    if (g_workerStarted) return true;
    if (g_quit) return false;
    try {
        std::thread([] { GuardedThreadBody("spotify-api", [] { WorkerLoop(); }); }).detach();
    } catch (const std::system_error&) {
        return false;  // no new thread (the process is exiting)
    }
    g_workerStarted = true;
    return true;
}

// ---------------------------------------------------------------- the sign-in ("spotify-auth")
struct FlowArgs {
    uint64_t id = 0;
    std::string mod, clientId, verifier, state;
    int port = 0;
    std::shared_ptr<oauth::Listener> listener;
    std::shared_ptr<std::atomic<bool>> cancel;
};

// The code exchange, the profile, then the sign-in is committed and saved. Sets r->ok or r->error.
void Exchange(FlowArgs& a, const std::string& code, Result* r) {
    fetch::HttpRequest rq;
    rq.method = "POST";
    rq.headers.emplace_back("Content-Type", "application/x-www-form-urlencoded");
    rq.body = logic::TokenExchangeForm(code, logic::RedirectUri(a.port), a.clientId, a.verifier);
    fetch::HttpResponse rs;
    std::string err;
    const bool sent = fetch::Send(logic::kTokenUrl, rq, &rs, HttpOptions(), &err);
    Wipe(rq.body);
    if (!sent) {
        r->error = "network";
        LOG_WARN("[spotify] sign-in: the token request failed (%s)", err.c_str());
        return;
    }
    logic::Tokens t;
    std::string oe;
    const bool parsed = logic::ParseTokenResponse(rs.body, &t, &oe);  // also reads an error body (invalid_client)
    const bool ok = rs.status == 200 && parsed && !t.refresh.empty();
    Wipe(rs.body);
    if (!ok) {
        r->error = oe == "invalid_client" ? "invalid-client" : "exchange";
        LOG_WARN("[spotify] sign-in: the code exchange failed (HTTP %d%s%s)", rs.status, oe.empty() ? "" : ", ", oe.c_str());
        Wipe(t.access);
        Wipe(t.refresh);
        return;
    }
    logic::Me me;
    bool transport = false;
    if (!FetchMe(t.access, &me, &transport)) {
        r->error = transport ? "network" : "exchange";
        LOG_WARN("[spotify] sign-in: the account could not be read");
        Wipe(t.access);
        Wipe(t.refresh);
        return;
    }
    Shared& s = S();
    {
        std::lock_guard lk(s.mx);
        if (a.cancel->load() || g_quit) {
            r->cancelled = true;
        } else {
            s.linked = true;
            s.stored = tokens::Stored{a.clientId, t.refresh, t.scope, me.id, me.name, me.product, me.country, {a.mod}};
            s.access = t.access;
            s.accessExpires = logic::ExpiresAtMs(SteadyMs(), t.expiresIn);
            ++s.epoch;
        }
    }
    Wipe(t.access);
    Wipe(t.refresh);
    if (r->cancelled) return;
    SaveCurrent();
    r->ok = true;
}

void RunFlow(FlowArgs& a) {
    Result r;
    r.kind = Result::Kind::Flow;
    r.flowId = a.id;
    r.mod = a.mod;
    std::string code;
    switch (a.listener->Serve(a.state, GetTickCount64() + kFlowTimeoutMs, *a.cancel, &code)) {
        case oauth::Listener::Outcome::Code:
            if (a.cancel->load() || g_quit) r.cancelled = true;
            else Exchange(a, code, &r);
            break;
        case oauth::Listener::Outcome::Denied: r.error = "denied"; break;
        case oauth::Listener::Outcome::OtherError: r.error = "exchange"; break;
        case oauth::Listener::Outcome::Timeout: r.error = "timeout"; break;
        case oauth::Listener::Outcome::Cancelled: r.cancelled = true; break;
    }
    Wipe(code);
    Wipe(a.verifier);
    Wipe(a.state);
    Shared& s = S();
    std::lock_guard lk(s.mx);
    Push(s, std::move(r));
}

bool Random(uint8_t* p, size_t n) { return BCryptGenRandom(nullptr, p, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0; }

// ---------------------------------------------------------------- main-thread state
bool g_installed = false;
cfg::Spotify g_cfg;
uint64_t g_cfgAt = 0;
host::Sink* g_sink = nullptr;
std::set<std::string> g_touched;
std::map<std::string, std::string> g_sent;  // the last status each touched mod was told
std::string g_error;                        // state "error": its code
uint32_t g_nextRid = 1;
bool g_bgQueued = false;  // the start-up refresh was queued (or is not needed)
bool g_loadPending = false;  // Install ran; the stored sign-in is not read yet (EnsureLoaded)
struct Flow {
    uint64_t id = 0;
    std::string mod;
    std::shared_ptr<oauth::Listener> listener;
    std::shared_ptr<std::atomic<bool>> cancel;
};
std::unique_ptr<Flow> g_flow;
uint64_t g_nextFlow = 1;

host::Status StatusFor(const std::string& mod) {
    Shared& s = S();
    host::Status st;
    bool granted = false;
    std::string user, product;
    {
        std::lock_guard lk(s.mx);
        st.linked = s.linked;
        granted = s.linked && (mod.empty() || Has(s.stored.grants, mod));
        user = s.stored.user;
        product = s.stored.product;
    }
    if (g_flow) {
        st.state = "connecting";
    } else if (!g_error.empty()) {
        st.state = "error";
        st.error = g_error;
    } else if (granted) {
        st.state = "connected";
        st.user = user;
        st.premium = logic::PremiumOf(product);
    } else {
        st.state = "disconnected";
    }
    return st;
}

std::string Signature(const host::Status& st) {
    return st.state + '\n' + st.user + '\n' + (!st.premium ? '?' : *st.premium ? '1' : '0') + (st.linked ? '1' : '0') + '\n' + st.error;
}

json::Value StatusJson(const host::Status& st) {
    jsonmini::Obj o;
    o.Str("type", "status").Str("state", st.state);
    if (!st.user.empty()) o.Str("user", st.user);
    if (st.premium) o.Bool("premium", *st.premium);
    o.Bool("linked", st.linked);
    if (!st.error.empty()) o.Str("error", st.error);
    json::Value v;
    json::Error e;
    json::Parse(o.End(), &v, &e);
    return v;
}

// A melange.spotify status event to every touched mod whose view changed.
void EmitChanged() {
    for (const std::string& m : g_touched) {
        const host::Status st = StatusFor(m);
        std::string sig = Signature(st);
        std::string& last = g_sent[m];
        if (last == sig) continue;
        last = std::move(sig);
        if (g_sink) g_sink->Event(m, StatusJson(st));
    }
}

// The stored sign-in, read once on the main thread (never in DllMain: DPAPI and the shell are not loader-lock safe).
// A sign-in made with another ClientId does not count (the file is left alone). No network here.
void EnsureLoaded() {
    if (!g_loadPending || !g_installed) return;
    g_loadPending = false;
    Shared& s = S();
    tokens::Stored st;
    bool have = false;
    {
        std::lock_guard sl(s.saveMx);
        have = !g_cfg.clientId.empty() && tokens::Load(&st) && st.clientId == g_cfg.clientId;
    }
    {
        std::lock_guard lk(s.mx);
        if (have && !s.linked) {
            s.linked = true;
            s.stored = std::move(st);
            ++s.epoch;
        }
    }
    Wipe(st.refresh);
    LOG_INFO("[spotify] %s", g_cfg.clientId.empty() ? "no ClientId: off" : have ? "signed in" : "not signed in");
}

// The ClientId changed: the stored sign-in counts only for the app it was made with.
void Relink() {
    Shared& s = S();
    {
        std::lock_guard lk(s.mx);
        if (s.linked && s.stored.clientId == g_cfg.clientId) return;
    }
    tokens::Stored st;
    bool have = false;
    {
        std::lock_guard sl(s.saveMx);
        have = !g_cfg.clientId.empty() && tokens::Load(&st) && st.clientId == g_cfg.clientId;
    }
    {
        std::lock_guard lk(s.mx);
        if (s.linked) FailQueuedLocked(s);
        s.linked = have;
        s.stored = have ? std::move(st) : tokens::Stored{};
        Wipe(s.access);
        s.accessExpires = 0;
        ++s.epoch;
    }
    Wipe(st.refresh);
    g_bgQueued = false;
    EmitChanged();
}

void RefreshCfg(bool force) {
    EnsureLoaded();
    const uint64_t now = GetTickCount64();
    if (!force && g_cfgAt && now - g_cfgAt < 2000) return;
    g_cfgAt = now;
    const std::string before = g_cfg.clientId;
    g_cfg = cfg::ReadSpotify();
    {
        Shared& s = S();
        std::lock_guard lk(s.mx);
        s.gate.SetPerMinute(g_cfg.requestsPerMinute);
    }
    if (g_cfg.clientId != before) Relink();
}

// The start-up refresh, once, when a granted mod first asks.
void MaybeBackground(const std::string& mod) {
    if (g_bgQueued || mod.empty() || !host::Granted(mod) || !EnsureWorker()) return;
    g_bgQueued = true;
    Shared& s = S();
    {
        std::lock_guard lk(s.mx);
        Job j;
        j.background = true;
        j.deadline = SteadyMs() + kJobTimeoutMs;
        s.read.push_back(std::move(j));
    }
    s.cv.notify_one();
}

void Pump() {
    if (!g_installed) return;
    EnsureLoaded();
    Shared& s = S();
    std::vector<Result> rs;
    {
        std::lock_guard lk(s.mx);
        if (s.results.empty()) return;
        rs.swap(s.results);
    }
    const uint64_t steadyNow = SteadyMs();
    const double peerNow = peerbus::NowMs();
    bool emit = false;
    for (Result& r : rs) {
        switch (r.kind) {
            case Result::Kind::Answer: {
                if (!g_sink || !r.token) break;
                json::Value v;
                json::Error e;
                if (!json::Parse(r.json, &v, &e)) break;
                if (r.patchAt)
                    for (auto& [k, m] : v.members)
                        if (k == "at" && m.IsNumber()) m.number = peerNow - static_cast<double>(steadyNow - std::min(steadyNow, r.mid));
                g_sink->Callback(r.mod, r.token, v, true);
                break;
            }
            case Result::Kind::Revoked:
                g_error = "revoked";
                g_bgQueued = false;
                emit = true;
                break;
            case Result::Kind::Profile: emit = true; break;
            case Result::Kind::Flow:
                if (!g_flow || g_flow->id != r.flowId) break;  // a cancelled sign-in
                g_flow.reset();
                if (r.ok) {
                    g_error.clear();
                    g_bgQueued = true;  // the token and the profile are fresh
                    LOG_INFO("[spotify] signed in (for %s)", r.mod.c_str());
                } else if (!r.cancelled) {
                    g_error = r.error.empty() ? "exchange" : r.error;
                    LOG_INFO("[spotify] sign-in failed: %s", g_error.c_str());
                }
                emit = true;
                break;
        }
    }
    if (emit) EmitChanged();
}

class Spotify final : public Module {
public:
    const char* Name() const override { return "Spotify"; }
    const char* Description() const override {
        return "Spotify for client mods (wum.spotify): browser sign-in, library and playback on the user's own device";
    }
    int Order() const override { return 61; }  // after the Sandbox (50) and Media (60)
    // Runs inside DllMain (loader lock): no file, shell or DPAPI calls here. The stored sign-in is read on the main
    // thread at the first frame or the first wum.spotify call (EnsureLoaded).
    bool Install() override {
        g_cfg = cfg::ReadSpotify();  // documents the [Spotify] keys in a fresh ini
        g_cfgAt = GetTickCount64();
        {
            Shared& s = S();
            std::lock_guard lk(s.mx);
            s.gate.SetPerMinute(g_cfg.requestsPerMinute);
        }
        g_loadPending = true;
        events::Subscribe(events::Event::Frame, [] { Pump(); });
        g_installed = true;
        LOG_INFO("[spotify] installed (%s)", g_cfg.clientId.empty() ? "no ClientId: off" : "the stored sign-in is read at the first frame");
        return true;
    }
    // Reached only from DllMain on FreeLibrary: stop the threads at their next check, free the redirect port. No joins.
    void Uninstall() override {
        g_installed = false;
        g_quit = true;
        if (g_flow) {
            g_flow->cancel->store(true);
            g_flow->listener->Close();
        }
        S().cv.notify_all();
    }
};
}  // namespace
MELANGE_MODULE(Spotify);

// ---------------------------------------------------------------- host.h (wum.spotify)
namespace host {
bool Available(std::string* reason) {
    if (!g_installed) {
        *reason = config::GetBool("Spotify", "Enabled", true) ? "unavailable" : "disabled";
        return false;
    }
    RefreshCfg(false);
    if (g_cfg.clientId.empty()) {
        *reason = "no-client-id";
        return false;
    }
    return true;
}

void Touch(const std::string& mod) {
    if (mod.empty()) return;
    EnsureLoaded();
    if (g_touched.insert(mod).second) g_sent[mod] = Signature(StatusFor(mod));
}

Status GetStatus(const std::string& mod) {
    EnsureLoaded();
    if (g_installed) MaybeBackground(mod);
    return StatusFor(mod);
}

bool Granted(const std::string& mod) {
    EnsureLoaded();
    Shared& s = S();
    std::lock_guard lk(s.mx);
    return s.linked && Has(s.stored.grants, mod);
}

bool Connect(const std::string& mod, std::string* reason) {
    if (!g_installed || g_quit) {
        *reason = "unavailable";
        return false;
    }
    RefreshCfg(true);
    if (g_cfg.clientId.empty()) {
        *reason = "no-client-id";
        return false;
    }
    if (!g_flow && !g_error.empty()) {  // an "error" state ends at the next connect()
        g_error.clear();
        EmitChanged();
    }
    Shared& s = S();
    // 1. Linked already: grant it to this mod, no browser.
    bool linked = false, added = false;
    {
        std::lock_guard lk(s.mx);
        linked = s.linked;
        if (linked && !Has(s.stored.grants, mod) && s.stored.grants.size() < 256) {
            s.stored.grants.push_back(mod);
            added = true;
        }
    }
    if (linked) {
        if (added) {
            SaveCurrent();
            LOG_INFO("[spotify] the linked account was granted to %s", mod.c_str());
            EmitChanged();
        }
        return true;
    }
    // 2. One sign-in at a time.
    if (g_flow) {
        *reason = "busy";
        return false;
    }
    // 3. The redirect listener, bound here so a taken port is reported at once.
    auto listener = std::make_shared<oauth::Listener>();
    if (!listener->Open(g_cfg.redirectPort)) {
        *reason = "port-busy";
        return false;
    }
    // 4. The secrets.
    uint8_t rnd[48], st[16];
    if (!Random(rnd, sizeof rnd) || !Random(st, sizeof st)) {
        listener->Close();
        *reason = "unavailable";
        return false;
    }
    FlowArgs a;
    a.id = g_nextFlow++;
    a.mod = mod;
    a.clientId = g_cfg.clientId;
    a.port = g_cfg.redirectPort;
    a.verifier = logic::PkceVerifier(rnd, sizeof rnd);
    a.state = logic::Base64Url(st, sizeof st);
    SecureZeroMemory(rnd, sizeof rnd);
    SecureZeroMemory(st, sizeof st);
    a.listener = listener;
    a.cancel = std::make_shared<std::atomic<bool>>(false);
    // 5. The authorize page, on Spotify's own host only.
    const std::string url = logic::AuthorizeUrl(a.clientId, logic::RedirectUri(a.port), a.state, logic::PkceChallenge(a.verifier));
    if (url.rfind(logic::kAuthorizePrefix, 0) != 0) {
        listener->Close();
        *reason = "browser";
        return false;
    }
    auto flow = std::make_unique<Flow>();
    flow->id = a.id;
    flow->mod = mod;
    flow->listener = listener;
    flow->cancel = a.cancel;
    // The listener's thread starts before the browser opens, so a page never leads nowhere.
    try {
        std::thread([a]() mutable { GuardedThreadBody("spotify-auth", [&] { RunFlow(a); }); }).detach();
    } catch (const std::system_error&) {
        Wipe(a.verifier);
        listener->Close();
        *reason = "unavailable";
        return false;
    }
    Wipe(a.verifier);
    Wipe(a.state);
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", game::Widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        flow->cancel->store(true);
        listener->Close();
        *reason = "browser";
        LOG_WARN("[spotify] the browser could not be opened for the sign-in");
        return false;
    }
    // 6. Connecting.
    g_flow = std::move(flow);
    LOG_INFO("[spotify] sign-in opened in the browser (for %s)", mod.c_str());
    EmitChanged();
    return true;
}

void Disconnect(const std::string& mod) {
    EnsureLoaded();
    Shared& s = S();
    if (g_flow) {
        g_flow->cancel->store(true);
        g_flow->listener->Close();
        g_flow.reset();
    }
    {
        std::lock_guard sl(s.saveMx);
        {
            std::lock_guard lk(s.mx);
            s.linked = false;
            Wipe(s.stored.refresh);
            s.stored = {};
            Wipe(s.access);
            s.accessExpires = 0;
            ++s.epoch;
            FailQueuedLocked(s);
        }
        tokens::Delete();
    }
    g_error.clear();
    g_bgQueued = false;
    LOG_INFO("[spotify] signed out (by %s)", mod.c_str());
    EmitChanged();
}

uint32_t Submit(const std::string& mod, uint32_t gen, const logic::Request& r, uint32_t token, std::string* reason) {
    Shared& s = S();
    if (!g_installed || !Granted(mod)) {
        *reason = "auth";
        return 0;
    }
    {
        std::lock_guard lk(s.mx);
        auto it = s.perMod.find(mod);
        if ((it != s.perMod.end() && it->second >= kMaxPerMod) || s.queued >= kMaxQueued) {
            *reason = "busy";
            return 0;
        }
    }
    // A free account's control calls are sent too: Spotify answers "premium" (403 PREMIUM_REQUIRED), and an upgraded
    // account is learned from its first call that works.
    const uint32_t rid = g_nextRid++;
    if (!g_nextRid) g_nextRid = 1;
    if (!EnsureWorker()) {
        *reason = "busy";
        return 0;
    }
    {
        std::lock_guard lk(s.mx);
        Job j;
        j.rid = rid;
        j.mod = mod;
        j.gen = gen;
        j.token = token;
        j.req = r;
        j.deadline = SteadyMs() + kJobTimeoutMs;
        ++s.perMod[mod];
        ++s.queued;
        (logic::IsControl(r.op) ? s.control : s.read).push_back(std::move(j));
    }
    s.cv.notify_one();
    MaybeBackground(mod);
    return rid;
}

void CancelGen(const std::string& mod, uint32_t gen) {
    Shared& s = S();
    std::lock_guard lk(s.mx);
    for (std::deque<Job>* q : {&s.control, &s.read}) {
        for (auto it = q->begin(); it != q->end();) {
            if (!it->background && it->gen == gen && it->mod == mod) {
                Uncount(s, *it);
                it = q->erase(it);
            } else {
                ++it;
            }
        }
    }
    s.results.erase(std::remove_if(s.results.begin(), s.results.end(),
                                   [&](const Result& r) { return r.kind == Result::Kind::Answer && r.gen == gen && r.mod == mod; }),
                    s.results.end());
    if (s.running && s.runningGen == gen && s.runningMod == mod) s.runningDropped = true;
}

void SetSink(Sink* sink) { g_sink = sink; }
}  // namespace host
}  // namespace melange::spotify
