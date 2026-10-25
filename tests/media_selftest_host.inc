// media_selftest cases for the media host (owner: C4b): the media_sync.h follow policy (deadband, nudge, seek, lead,
// grace, stale epoch, paused host, clock not ready, 0-150 ms of time-pos staleness), the pipe client's coalescing and
// bounded queue, the helper's environment and command line, and the fake helper end to end (tests/fake_helper.cpp):
// a Low-integrity launch in a job, the pipe's pid check, frames through the section, props, resolve and torrents, a
// 15 s main-thread stall that must not restart anything, and the scripted faults (hang, crash, hostile frame headers,
// oversized and malformed lines). Included at file scope; defines host_cases::Run(). Sources: cmake/holo/media_host.cmake.
#include <shlobj.h>

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "media/helper.h"
#include "media/media_sync.h"
#include "media/pipe.h"
#include "media/shm.h"

namespace host_cases {
namespace sync = melange::media::sync;
namespace pipe = melange::media::pipe;
namespace pipeio = melange::media::pipeio;
namespace helper = melange::media::helper;
namespace shmio = melange::media::shmio;
namespace shm = melange::media::shm;
namespace mproto = melange::media::proto;

// ---------------------------------------------------------------- follow policy
sync::Target Host(double e, const char* item, bool play, double p, double a, double r = 1) {
    sync::Target t;
    t.e = e;
    t.i = item;
    t.play = play;
    t.p = p;
    t.r = r;
    t.a = a;
    return t;
}
sync::Local Playing(double pos, double speed = 1) {
    sync::Local l;
    l.loaded = true;
    l.pos = pos;
    l.speed = speed;
    return l;
}

inline void Policy() {
    const sync::Clock ready{true, 0};
    const sync::Clock noClock{false, 0};
    // First apply: a hard seek to the target plus the lead, then 1 s of grace.
    {
        sync::Follower f;
        CHECK(f.Update(Host(1, "a", true, 10, 1000), 1000), "follow: first update accepted");
        sync::Command c = f.Step(1000, Playing(3), ready);
        CHECK(c.seek && std::fabs(c.seekTo - 10.5) < 1e-9 && f.LastAction() == sync::Action::Seek, "follow: first apply hard-seeks to target + lead");
        CHECK(c.setPause == false, "follow: already playing, no pause change");
        c = f.Step(1500, Playing(3), ready);
        CHECK(!c.seek && f.LastAction() == sync::Action::None, "follow: no correction inside the grace period");
    }
    // Deadband and nudge (clock ready), against a target that moves with the host clock.
    {
        sync::Follower f;
        f.Update(Host(1, "a", true, 10, 1000), 1000);
        f.Step(1000, Playing(10), ready);  // the first apply's seek
        sync::Local l = Playing(0);
        l.sampleQpcMs = 0;
        // 10 s later the host is at 20 (seek pending ends after 8 s without a matching sample)
        l.pos = 20.05;
        sync::Command c = f.Step(11000, l, ready);
        CHECK(!c.seek && !c.setSpeed && f.LastAction() == sync::Action::None && std::fabs(f.Drift() - 0.05) < 1e-6, "follow: |e| < 0.10: nothing");
        l.pos = 20.5;
        c = f.Step(11000, l, ready);
        CHECK(c.setSpeed && std::fabs(c.speed - 0.95) < 1e-9 && f.LastAction() == sync::Action::Nudge, "follow: ahead by 0.5 s: 5 % slower");
        l.pos = 19.5;
        c = f.Step(11000, l, ready);
        CHECK(c.setSpeed && std::fabs(c.speed - 1.05) < 1e-9, "follow: behind by 0.5 s: 5 % faster");
        l.pos = 20.02;
        l.speed = 1.05;
        c = f.Step(11000, l, ready);
        CHECK(c.setSpeed && c.speed == 1.0 && f.LastAction() == sync::Action::None, "follow: back in the deadband: the nudge ends");
        l.pos = 22;
        l.speed = 1;
        c = f.Step(11000, l, ready);
        CHECK(c.seek && std::fabs(c.seekTo - 20.5) < 1e-9, "follow: |e| >= 1.5 s: seek");
    }
    // Rate: the host's rate is the base of the nudge and of the target's motion.
    {
        sync::Follower f;
        f.Update(Host(1, "a", true, 0, 0, 2), 0);
        f.Step(0, Playing(0, 2), ready);
        sync::Local l = Playing(20.4, 2);  // 10 s at 2x = 20
        const sync::Command c = f.Step(10000, l, ready);
        CHECK(c.setSpeed && std::fabs(c.speed - 1.9) < 1e-9, "follow: a nudge at rate 2 is 1.9");
    }
    // Clock not ready: no nudging, hard seek only above 3 s; the target moves by my own clock since arrival.
    {
        sync::Follower f;
        f.Update(Host(1, "a", true, 10, 999999), 5000);  // `a` is on the host's clock, unusable without an offset
        f.Step(5000, Playing(10), noClock);
        sync::Local l = Playing(19 + 0.5);  // 9 s after arrival the host is at 19 (past the first seek's wait)
        sync::Command c = f.Step(14000, l, noClock);
        CHECK(!c.setSpeed && f.LastAction() == sync::Action::None, "follow: no clock: no nudge at 0.5 s");
        l.pos = 21.5;
        c = f.Step(14000, l, noClock);
        CHECK(!c.seek, "follow: no clock: no seek at 2.5 s");
        l.pos = 22.5;
        c = f.Step(14000, l, noClock);
        CHECK(c.seek, "follow: no clock: seek above 3 s");
    }
    // Stale input.
    {
        sync::Follower f;
        CHECK(f.Update(Host(2, "a", true, 10, 1000), 0), "follow: epoch 2");
        CHECK(!f.Update(Host(1, "a", true, 50, 9000), 0), "follow: a lower epoch is dropped");
        CHECK(!f.Update(Host(2, "a", true, 50, 900), 0), "follow: the same epoch with an older timestamp is dropped");
        CHECK(f.Update(Host(2, "a", true, 11, 2000), 0), "follow: the same epoch, newer: applied");
        CHECK(f.Current().p == 11, "follow: the newer state is current");
    }
    // Paused host: pause, seek to p only beyond 0.25 s.
    {
        sync::Follower f;
        f.Update(Host(1, "a", false, 42, 0), 0);
        sync::Local l = Playing(42.1);
        sync::Command c = f.Step(0, l, ready);
        CHECK(c.setPause && c.pause && c.seek && c.seekTo == 42, "follow: a paused host pauses us; the first apply seeks to p");
        l.paused = true;
        l.pos = 42.2;
        c = f.Step(5000, l, ready);
        CHECK(!c.seek && !c.setPause, "follow: paused, 0.2 s off: nothing");
        l.pos = 42.4;
        c = f.Step(5000, l, ready);
        CHECK(c.seek && c.seekTo == 42, "follow: paused, 0.4 s off: seek to p");
        f.Update(Host(1, "a", true, 42, 6000), 6000);
        c = f.Step(6000, l, ready);
        CHECK(c.setPause && !c.pause, "follow: the host plays again: unpause");
    }
    // Buffering never pauses the host; a stall we did not cause ends in a hard seek.
    {
        sync::Follower f;
        f.Update(Host(1, "a", true, 0, 0), 0);
        f.Step(0, Playing(0), ready);
        sync::Local l = Playing(10);
        l.buffering = true;
        sync::Command c = f.Step(10000, l, ready);
        CHECK(!c.seek && !c.setPause && f.LastAction() == sync::Action::None, "follow: buffering: no action, no pause");
        l.buffering = false;
        l.pos = 12.0;  // host at 12.5
        c = f.Step(12500, l, ready);
        CHECK(c.seek && std::fabs(c.seekTo - 13.0) < 1e-9, "follow: after a stall: hard seek even inside 1.5 s");
    }
    // An item change hard-seeks.
    {
        sync::Follower f;
        f.Update(Host(1, "a", true, 0, 0), 0);
        f.Step(0, Playing(0), ready);
        f.Step(20000, Playing(20), ready);
        f.Update(Host(2, "b", true, 0, 20000), 20000);
        const sync::Command c = f.Step(20000, Playing(0.05), ready);
        CHECK(c.seek, "follow: a new item starts with a hard seek");
    }
    // The lead is learned from how long a seek takes (EWMA 0.7), clamped to 0.2..3.
    {
        sync::Follower f;
        f.Update(Host(1, "a", true, 0, 0), 0);
        f.Step(0, Playing(0), ready);  // seek to 0.5 at t=0
        sync::Local l = Playing(0.5);
        l.sampleQpcMs = 800;
        l.sampleValue = 0.5;
        f.Step(900, l, ready);
        CHECK(std::fabs(f.Lead() - (0.7 * 0.5 + 0.3 * 0.8)) < 1e-9, "follow: lead learned from an 800 ms seek");
        sync::Follower g;
        g.Update(Host(1, "a", true, 0, 0), 0);
        g.Step(0, Playing(0), ready);
        for (int i = 0; i < 40; ++i) {
            g.Reopened();
            sync::Local s = Playing(0);
            g.Step(i * 100000.0, s, ready);
            s.sampleQpcMs = i * 100000.0 + 7000;
            s.sampleValue = g.TargetPos(i * 100000.0, ready) + g.Lead();
            g.Step(i * 100000.0 + 7100, s, ready);
        }
        CHECK(g.Lead() <= sync::kLeadMax + 1e-9 && g.Lead() > 2.5, "follow: a slow machine's lead grows to the 3 s cap");
    }
    // Live: play and pause only.
    {
        sync::Follower f;
        f.Update(Host(1, "a", true, 0, 0), 0);
        sync::Local l = Playing(100);
        l.live = true;
        const sync::Command c = f.Step(0, l, ready);
        CHECK(!c.seek && !c.setSpeed && !f.HasDrift(), "follow: live: no seek, no nudge, no drift");
    }
    // Extrapolation (Y1).
    CHECK(std::fabs(sync::Extrapolate(10, 1000, 1130, 1, true, 0) - 10.13) < 1e-9, "extrapolate: 130 ms stale sample");
    CHECK(sync::Extrapolate(10, 1000, 1130, 1, false, 0) == 10, "extrapolate: paused stays");
    CHECK(sync::Extrapolate(10, 1000, 3000, 1, true, 11) == 11, "extrapolate: capped at the duration");
}

// A simulated local player behind a 10 Hz sample stream delayed by 0-150 ms (Y1). With a perfect player the follow
// settles after the first seek and never nudges; with one that runs 2 % fast it nudges but never seeks again.
struct SimResult {
    int nudgesLate = 0, seeksLate = 0;
    double maxDriftLate = 0;
};
SimResult Simulate(double playerError, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> delay(0, 150);
    const double offset = 12345;  // host clock = mine + offset
    const sync::Clock clock{true, offset};
    sync::Follower f;
    f.Update(Host(1, "x", true, 5, 0 + offset), 0);  // the host was at 5 s at my time 0
    double pos = 0, speed = 1, bufferUntil = -1;
    bool buffering = false;
    struct Sample {
        double at, value, qpc;
    };
    std::deque<Sample> wire;
    double lastSample = 0, lastQpc = 0, nextSample = 0;
    SimResult r;
    for (double now = 0; now < 60000; now += 16) {
        // the player
        if (buffering && now >= bufferUntil) buffering = false;
        if (!buffering) pos += 0.016 * speed * (1 + playerError);
        // the helper samples at 10 Hz; the core sees each one 0-150 ms later
        if (now >= nextSample) {
            wire.push_back({now + delay(rng), pos, now});
            nextSample = now + 100;
        }
        while (!wire.empty() && wire.front().at <= now) {
            lastSample = wire.front().value;
            lastQpc = wire.front().qpc;
            wire.pop_front();
        }
        sync::Local l;
        l.loaded = true;
        l.buffering = buffering;
        l.speed = speed;
        l.pos = sync::Extrapolate(lastSample, lastQpc, now, speed, !buffering, 0);
        l.sampleQpcMs = lastQpc;
        l.sampleValue = lastSample;
        const sync::Command c = f.Step(now, l, clock);
        if (c.setSpeed) speed = c.speed;
        if (c.seek) {
            pos = c.seekTo;
            buffering = true;
            bufferUntil = now + 500;  // a seek takes 0.5 s here (the starting lead)
            lastSample = c.seekTo;
            lastQpc = now;
        }
        if (now > 10000) {
            r.nudgesLate += f.LastAction() == sync::Action::Nudge && c.setSpeed;
            r.seeksLate += c.seek;
            if (f.HasDrift()) r.maxDriftLate = std::max(r.maxDriftLate, std::fabs(f.Drift()));
        }
    }
    return r;
}

inline void Staleness() {
    for (uint32_t seed = 1; seed <= 5; ++seed) {
        const SimResult a = Simulate(0, seed);
        CHECK(a.nudgesLate == 0 && a.seeksLate == 0 && a.maxDriftLate < 0.10,
              "follow: 0-150 ms stale samples: steady, no nudge (max drift " + std::to_string(a.maxDriftLate) + ")");
        const SimResult b = Simulate(0.02, seed);
        CHECK(b.seeksLate == 0 && b.nudgesLate > 0 && b.maxDriftLate < 0.2,
              "follow: a 2 % fast player is nudged, never seeked (max drift " + std::to_string(b.maxDriftLate) + ")");
    }
}

// ---------------------------------------------------------------- inbox, environment, restart policy
pipe::PropEvt Prop(uint32_t id, const char* name, double v) {
    pipe::PropEvt p;
    p.id = id;
    p.name = name;
    p.kind = pipe::PropEvt::Kind::Number;
    p.number = v;
    return p;
}

inline void InboxCases() {
    pipeio::Inbox in;
    for (int i = 0; i < 1000; ++i) in.Push(Prop(1, mproto::prop::kTimePos, i));
    in.Push(Prop(2, mproto::prop::kTimePos, 7));
    pipe::BtStatusEvt st;
    st.ih = std::string(40, 'a');
    for (int i = 0; i < 300; ++i) in.Push(st);
    pipe::EventEvt ev;
    ev.id = 1;
    ev.name = mproto::event::kFileLoaded;
    in.Push(ev);
    CHECK(in.Pending() == mproto::kMaxEventQueue + 2, "inbox: props coalesce, the queue is bounded");
    std::vector<pipe::Message> out;
    in.Drain(&out);
    bool lastIsEvent = false;
    int statuses = 0;
    double p1 = -1;
    for (const auto& m : out) {
        statuses += std::holds_alternative<pipe::BtStatusEvt>(m);
        if (auto* p = std::get_if<pipe::PropEvt>(&m))
            if (p->id == 1) p1 = p->number;
    }
    for (size_t i = 0; i < out.size(); ++i)
        if (std::holds_alternative<pipe::EventEvt>(out[i])) lastIsEvent = true;
    CHECK(p1 == 999, "inbox: the latest time-pos wins");
    CHECK(lastIsEvent && statuses == static_cast<int>(mproto::kMaxEventQueue) - 1, "inbox: bt.status dropped first, the event kept");
    CHECK(in.Dropped() == 300 + 1 - mproto::kMaxEventQueue, "inbox: drops counted");
    CHECK(in.Pending() == 0, "inbox: drained");

    const helper::EnvVars parent = {{L"SystemRoot", L"C:\\Windows"}, {L"PATH", L"C:\\evil;C:\\Windows\\System32"},
                                    {L"PYTHONPATH", L"C:\\x"},        {L"PYTHONSTARTUP", L"C:\\x\\s.py"},
                                    {L"YTDLP_CONFIG", L"x"},          {L"MPV_HOME", L"C:\\mpv"},
                                    {L"DENO_DIR", L"C:\\deno"},        {L"APPDATA", L"C:\\Users\\u\\AppData\\Roaming"},
                                    {L"USERPROFILE", L"C:\\Users\\u"}, {L"NUMBER_OF_PROCESSORS", L"8"},
                                    {L"LUA_INIT", L"os.execute()"}};
    const helper::EnvVars env = helper::BuildEnvironment(parent, L"C:\\cache\\1-ab");
    auto get = [&](const wchar_t* k) -> std::wstring {
        for (const auto& [a, b] : env)
            if (_wcsicmp(a.c_str(), k) == 0) return b;
        return L"<none>";
    };
    CHECK(get(L"PYTHONPATH") == L"<none>" && get(L"PYTHONSTARTUP") == L"<none>" && get(L"YTDLP_CONFIG") == L"<none>" &&
              get(L"MPV_HOME") == L"<none>" && get(L"LUA_INIT") == L"<none>",
          "env: no PYTHON*, YTDLP*, MPV_HOME or LUA_* from the parent");
    CHECK(get(L"PATH") == L"C:\\Windows\\System32;C:\\Windows", "env: PATH is the system folders only");
    CHECK(get(L"DENO_DIR") == L"C:\\cache\\1-ab\\deno" && get(L"DENO_NO_UPDATE_CHECK") == L"1" && get(L"DENO_NO_PROMPT") == L"1",
          "env: deno pointed at the cache");
    CHECK(get(L"APPDATA") == L"C:\\cache\\1-ab\\home\\AppData\\Roaming" && get(L"HOME") == L"C:\\cache\\1-ab\\home" &&
              get(L"USERPROFILE") == L"C:\\cache\\1-ab\\home" && get(L"XDG_CONFIG_HOME") == L"C:\\cache\\1-ab\\home\\.config",
          "env: every config home is the empty cache home (no yt-dlp user config)");
    CHECK(get(L"NUMBER_OF_PROCESSORS") == L"8" && get(L"SystemRoot") == L"C:\\Windows", "env: the system variables stay");
    bool sorted = true;
    for (size_t i = 1; i < env.size(); ++i) sorted &= _wcsicmp(env[i - 1].first.c_str(), env[i].first.c_str()) <= 0;
    CHECK(sorted, "env: sorted");
    CHECK(helper::QuoteArg(L"C:\\a b\\") == L"\"C:\\a b\\\\\"" && helper::QuoteArg(L"x\"y") == L"\"x\\\"y\"" && helper::QuoteArg(L"plain") == L"plain" &&
              helper::QuoteArg(L"") == L"\"\"",
          "command line quoting");

    helper::RestartPolicy rp;
    CHECK(rp.Allow(1000) && !rp.Allow(30000) && !rp.Allow(60999) && rp.Allow(61000), "restart: once per 60 s");
}

// ---------------------------------------------------------------- the fake helper, end to end
std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L'\\'));
}

std::wstring g_root;  // a fresh cache root under LocalLow (Low-writable) for this run

std::wstring LocalLow() {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &p)) && p) s = p;
    CoTaskMemFree(p);
    return s;
}

helper::LaunchSpec Spec(const std::wstring& name, const std::vector<std::wstring>& extra, bool low = true) {
    helper::LaunchSpec s;
    s.exe = ExeDir() + L"\\fake_helper.exe";
    s.cacheDir = g_root + L"\\" + name;
    std::string err;
    helper::PrepareCacheDir(s.cacheDir, &err);
    s.lowIntegrity = low;
    s.args = {mproto::arg::kParent, std::to_wstring(GetCurrentProcessId()), mproto::arg::kCache, s.cacheDir, mproto::arg::kLibmpv,
              L"C:\\none\\libmpv-2.dll", mproto::arg::kYtdlp, L"C:\\none\\yt-dlp.exe", mproto::arg::kDeno, L"C:\\none\\deno.exe"};
    s.args.insert(s.args.end(), extra.begin(), extra.end());
    return s;
}

// Drains into `got` until `pred` holds or `ms` pass.
template <class P>
bool WaitFor(helper::Session& s, std::vector<pipe::Message>& got, uint32_t ms, P&& pred) {
    const ULONGLONG end = GetTickCount64() + ms;
    for (;;) {
        s.Drain(&got);
        if (pred()) return true;
        if (GetTickCount64() >= end) return false;
        Sleep(10);
    }
}

bool StartReady(helper::Session& s, const helper::LaunchSpec& spec, std::vector<pipe::Message>& got, const pipeio::Timings& t = {}) {
    std::string err;
    if (!s.Start(spec, pipe::HelloCmd{}, t, &err)) {
        printf("  start failed: %s\n", err.c_str());
        return false;
    }
    return WaitFor(s, got, 10000, [&] { return s.GetState() == helper::Session::State::Ready; });
}

int IntegrityRid(void* process) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(static_cast<HANDLE>(process), TOKEN_QUERY, &tok)) return -1;
    DWORD n = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &n);
    std::vector<uint8_t> buf(n ? n : 1);
    int rid = -1;
    if (n && GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), n, &n)) {
        auto* tml = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
        rid = static_cast<int>(*GetSidSubAuthority(tml->Label.Sid, *GetSidSubAuthorityCount(tml->Label.Sid) - 1));
    }
    CloseHandle(tok);
    return rid;
}

template <class T>
const T* Last(const std::vector<pipe::Message>& got, bool (*match)(const T&) = nullptr) {
    for (auto it = got.rbegin(); it != got.rend(); ++it)
        if (auto* x = std::get_if<T>(&*it))
            if (!match || match(*x)) return x;
    return nullptr;
}
const pipe::PropEvt* LastProp(const std::vector<pipe::Message>& got, const char* name) {
    for (auto it = got.rbegin(); it != got.rend(); ++it)
        if (auto* x = std::get_if<pipe::PropEvt>(&*it))
            if (x->name == name) return x;
    return nullptr;
}

inline void EndToEnd() {
    if (GetFileAttributesW((ExeDir() + L"\\fake_helper.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        CHECK(false, "fake_helper.exe next to media_selftest.exe");
        return;
    }
    const std::wstring ll = LocalLow();
    CHECK(!ll.empty(), "LocalLow folder");
    g_root = ll + L"\\Melange\\media-selftest-" + std::to_wstring(GetCurrentProcessId());
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring medium = std::wstring(tmp) + L"melange-media-selftest-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(medium.c_str(), nullptr);

    // ---- a normal session: Low integrity, frames, props, resolve, torrents
    {
        helper::Session s;
        std::vector<pipe::Message> got;
        CHECK(StartReady(s, Spec(L"main", {L"--probe-deny", medium}), got), "e2e: the fake helper connects and says hello");
        CHECK(s.GotHello() && s.Hello().mpv == "fake", "e2e: hello parsed");
        CHECK(IntegrityRid(s.ProcessHandle()) == SECURITY_MANDATORY_LOW_RID, "e2e: the helper runs at Low integrity");
        BOOL inJob = FALSE;
        CHECK(IsProcessInJob(static_cast<HANDLE>(s.ProcessHandle()), nullptr, &inJob) && inJob, "e2e: the helper is in the job");
        std::ifstream probe(g_root + L"\\main\\probe.txt");
        std::string line;
        std::getline(probe, line);
        CHECK(line == "cache=ok deny=denied", "e2e: Low IL writes its cache, not a medium folder (" + line + ")");
        std::getline(probe, line);
        CHECK(line == "parent=ok", "e2e: a Low IL helper can still wait on the game's process (" + line + ")");

        shmio::Section sec;
        std::string err;
        const std::string name = shmio::SectionName(GetCurrentProcessId(), helper::RandomHex(8), 1);
        CHECK(sec.Create(name, true, &err), "e2e: section created (" + err + ")");
        shmio::Section dup;
        CHECK(!dup.Create(name, true, &err), "e2e: an existing section name is refused");
        pipe::OpenCmd o;
        o.id = 1;
        o.gen = 1;
        o.shm = name;
        o.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
        o.paused = false;
        o.start = 3;
        s.Send(o);
        CHECK(WaitFor(s, got, 3000, [&] { return Last<pipe::EventEvt>(got) != nullptr; }), "e2e: file-loaded");
        const pipe::PropEvt* vp = LastProp(got, mproto::prop::kVideoParams);
        CHECK(vp && vp->video.w == 1920 && vp->id == 1 && vp->gen == 1, "e2e: video-params for player 1, gen 1");
        shm::Frame f;
        const ULONGLONG fEnd = GetTickCount64() + 3000;
        shm::ReadResult rr = shm::ReadResult::None;
        while (GetTickCount64() < fEnd && (rr = sec.Reader().Acquire(&f)) != shm::ReadResult::New) Sleep(5);
        CHECK(rr == shm::ReadResult::New && f.meta.w == 320 && f.meta.h == 180 && f.data[4] == 1, "e2e: a 320x180 frame through the section");
        if (rr == shm::ReadResult::New) sec.Reader().Release();
        pipe::SizeCmd z;
        z.id = 1;
        z.w = 640;
        z.h = 360;
        s.Send(z);
        bool big = false;
        const ULONGLONG zEnd = GetTickCount64() + 3000;
        while (!big && GetTickCount64() < zEnd) {
            if (sec.Reader().Acquire(&f) == shm::ReadResult::New) {
                big = f.meta.w == 640 && f.meta.h == 360;
                sec.Reader().Release();
            }
            Sleep(5);
        }
        CHECK(big, "e2e: size changes the frame size");
        CHECK(sec.Reader().BadFrames() == 0, "e2e: no bad frames from a sane helper");

        // time-pos with qpc: the extrapolated position tracks a fresh sample.
        got.clear();
        Sleep(600);
        s.Drain(&got);
        const pipe::PropEvt* tp = LastProp(got, mproto::prop::kTimePos);
        CHECK(tp && tp->qpcMs > 0 && tp->number > 3.2, "e2e: time-pos advances, stamped with qpc");
        if (tp) {
            const double then = tp->number, qpc = tp->qpcMs;
            Sleep(300);
            got.clear();
            s.Drain(&got);
            const pipe::PropEvt* tp2 = LastProp(got, mproto::prop::kTimePos);
            const double ex = sync::Extrapolate(then, qpc, pipeio::NowMs(), 1, true, 0);
            CHECK(tp2 && std::fabs(ex - sync::Extrapolate(tp2->number, tp2->qpcMs, pipeio::NowMs(), 1, true, 0)) < 0.05,
                  "e2e: QPC extrapolation agrees with a fresher sample");
        }
        // seek: paused-for-cache on, then off
        pipe::SeekCmd sk;
        sk.id = 1;
        sk.t = 60;
        s.Send(sk);
        got.clear();
        bool on = false, off = false;
        WaitFor(s, got, 2000, [&] {
            for (const auto& m : got)
                if (auto* p = std::get_if<pipe::PropEvt>(&m))
                    if (p->name == mproto::prop::kPausedForCache) (p->boolean ? on : off) = true;
            return on && off;
        });
        CHECK(on && off, "e2e: a seek buffers, then plays");
        // resolve
        pipe::ResolveCmd rc;
        rc.rid = 5;
        rc.url = "https://www.youtube.com/playlist?list=PL0123456789ab";
        s.Send(rc);
        rc.rid = 6;
        rc.url = "https://www.youtube.com/playlist?list=PLprivate0123";
        s.Send(rc);
        got.clear();
        WaitFor(s, got, 2000, [&] {
            int n = 0;
            for (const auto& m : got) n += std::holds_alternative<pipe::ResolvedEvt>(m);
            return n == 2;
        });
        const pipe::ResolvedEvt* r5 = nullptr;
        const pipe::ResolvedEvt* r6 = nullptr;
        for (const auto& m : got)
            if (auto* r = std::get_if<pipe::ResolvedEvt>(&m)) (r->rid == 5 ? r5 : r6) = r;
        CHECK(r5 && r5->items.size() == 3 && r5->items[0].url == "https://www.youtube.com/watch?v=aaaaaaaaaaa", "e2e: resolve lists the playlist");
        CHECK(r6 && r6->error == mproto::err::kPrivate, "e2e: resolve error");
        // torrents
        pipe::BtAddCmd ba;
        ba.ih = "0123456789abcdef0123456789abcdef01234567";
        ba.magnet = "magnet:?xt=urn:btih:" + ba.ih;
        s.Send(ba);
        got.clear();
        CHECK(WaitFor(s, got, 3000, [&] { return Last<pipe::BtFilesEvt>(got) && Last<pipe::BtStatusEvt>(got); }), "e2e: bt.files then bt.status");
        pipe::BtRemoveCmd br;
        br.ih = ba.ih;
        s.Send(br);
        CHECK(WaitFor(s, got, 2000, [&] { return Last<pipe::BtRemovedEvt>(got) != nullptr; }), "e2e: bt.removed");

        // ---- a 15 s stall of the game thread: nothing restarts, the inbox stays bounded
        const uint32_t pid = s.Pid();
        Sleep(15000);
        CHECK(s.GetState() == helper::Session::State::Ready && !s.Hung(), "e2e: a 15 s main-thread stall: still ready, not hung");
        CHECK(s.Conn()->In().Pending() <= pipeio::Inbox::kMaxProps + mproto::kMaxEventQueue, "e2e: the inbox stayed bounded");
        CHECK(s.Pid() == pid && s.Conn()->MsSincePong() < 5000, "e2e: the same helper, pongs kept coming");
        got.clear();
        s.Drain(&got);
        CHECK(!got.empty(), "e2e: draining after the stall works");
        pipe::CloseCmd cc;
        cc.id = 1;
        s.Send(cc);
        CHECK(WaitFor(s, got, 2000, [&] { return Last<pipe::EventEvt>(got, [](const pipe::EventEvt& e) { return e.reason == "stop"; }); }),
              "e2e: close ends the file");
        HANDLE ph = nullptr;
        DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(s.ProcessHandle()), GetCurrentProcess(), &ph, SYNCHRONIZE, FALSE, 0);
        s.Stop(2000);
        CHECK(ph && WaitForSingleObject(ph, 3000) == WAIT_OBJECT_0, "e2e: quit ends the helper");
        if (ph) CloseHandle(ph);
    }

    // ---- the pipe accepts only the helper's pid; a second instance of the name is refused
    {
        std::string err;
        const std::wstring name = L"\\\\.\\pipe\\melange-holo-selftest-" + std::to_wstring(GetCurrentProcessId());
        auto c = pipeio::Connection::Create(name, false, &err);
        CHECK(c != nullptr, "pipe: created (" + err + ")");
        CHECK(pipeio::Connection::Create(name, false, &err) == nullptr, "pipe: the first instance only (no squatting)");
        if (c) {
            c->Start(GetCurrentProcessId() + 4, pipeio::Timings{}, [] { return true; }, [] {});
            HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            const ULONGLONG end = GetTickCount64() + 2000;
            while (c->Rejected() == 0 && GetTickCount64() < end) Sleep(10);
            CHECK(c->Rejected() == 1 && c->GetState() == pipeio::Connection::State::Waiting, "pipe: another process's connection is refused");
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            c->Close("test");
        }
    }

    // ---- faults
    {
        helper::Session s;
        std::vector<pipe::Message> got;
        CHECK(StartReady(s, Spec(L"hang", {L"--fault", L"hang"}), got), "hang: ready");
        shmio::Section sec;
        std::string err;
        const std::string name = shmio::SectionName(GetCurrentProcessId(), helper::RandomHex(8), 2);
        sec.Create(name, true, &err);
        pipe::OpenCmd o;
        o.id = 2;
        o.shm = name;
        o.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
        s.Send(o);
        const ULONGLONG t0 = GetTickCount64();
        const bool broke = WaitFor(s, got, 12000, [&] { return s.GetState() == helper::Session::State::Broken; });
        const ULONGLONG took = GetTickCount64() - t0;
        const double silent = s.Conn()->MsSincePong();
        CHECK(broke && s.Hung(), "hang: the watchdog kills a silent helper");
        CHECK(took >= 3000 && took < 10000 && silent >= 6000 && silent < 9000,
              "hang: after about 6 s without a pong (" + std::to_string(took) + " ms after the open, " + std::to_string(silent) + " ms silent)");
    }
    {
        helper::Session s;
        std::vector<pipe::Message> got;
        StartReady(s, Spec(L"crash", {L"--fault", L"crash"}), got);
        CHECK(WaitFor(s, got, 3000, [&] { return s.GetState() == helper::Session::State::Broken; }), "crash: noticed");
        CHECK(!s.Hung() && s.BrokenReason().find("0xc0000005") != std::string::npos, "crash: reported with its exit code (" + s.BrokenReason() + ")");
    }
    {
        helper::Session s;
        std::vector<pipe::Message> got;
        CHECK(StartReady(s, Spec(L"hostile", {L"--fault", L"hostile"}), got), "hostile: ready");
        shmio::Section sec;
        std::string err;
        const std::string name = shmio::SectionName(GetCurrentProcessId(), helper::RandomHex(8), 3);
        sec.Create(name, true, &err);
        pipe::OpenCmd o;
        o.id = 3;
        o.shm = name;
        o.url = "https://www.youtube.com/watch?v=dQw4w9WgXcQ";
        s.Send(o);
        int fresh = 0;
        const ULONGLONG end = GetTickCount64() + 1500;
        shm::Frame f;
        while (GetTickCount64() < end) {
            if (sec.Reader().Acquire(&f) == shm::ReadResult::New) {
                ++fresh;
                sec.Reader().Release();
            }
            Sleep(3);
        }
        CHECK(fresh == 0 && sec.Reader().BadFrames() >= 10, "hostile: every hostile frame is skipped and counted (" +
                                                                std::to_string(sec.Reader().BadFrames()) + ")");
        s.Stop(500);
    }
    {
        helper::Session s;
        std::vector<pipe::Message> got;
        CHECK(StartReady(s, Spec(L"oversize", {L"--fault", L"oversize"}), got), "oversize: hello still arrives");
        CHECK(s.Conn()->LongLines() == 1, "oversize: the 70 KB line dropped whole");
        s.Stop(500);
    }
    {
        helper::Session s;
        std::vector<pipe::Message> got;
        CHECK(StartReady(s, Spec(L"garbage", {L"--fault", L"garbage"}), got), "garbage: hello still arrives");
        CHECK(s.Conn()->BadLines() >= 6, "garbage: malformed lines ignored (" + std::to_string(s.Conn()->BadLines()) + ")");
        s.Stop(500);
    }
    {
        // A medium-integrity launch ([Media] HelperLowIntegrity=0) writes where Low cannot.
        helper::Session s;
        std::vector<pipe::Message> got;
        CHECK(StartReady(s, Spec(L"medium", {L"--probe-deny", medium}, false), got), "medium: ready");
        CHECK(IntegrityRid(s.ProcessHandle()) >= SECURITY_MANDATORY_MEDIUM_RID, "medium: not lowered");
        std::ifstream probe(g_root + L"\\medium\\probe.txt");
        std::string line;
        std::getline(probe, line);
        CHECK(line == "cache=ok deny=written", "medium: the probe sees the difference (" + line + ")");
        s.Stop(500);
    }
    Sleep(800);  // the stopped helpers let go of their folders
    std::error_code ec;
    std::filesystem::remove_all(g_root, ec);
    std::filesystem::remove_all(medium, ec);
}

// The real holo-media.exe from an installed dev pack (pack_cases::Compiled), launched as the core launches it: Low
// integrity, job, the core's pipe server and a core-made frame section. Offline: the helper's hello (real mpv and
// libtorrent versions), the integrity level, an open that the helper must refuse without touching the network, and a
// clean quit. Playback of a local file through the same player path is `holo-media.exe --selftest --play <file>`.
void RealHelper(const std::wstring& packDir, const std::wstring& ytdlp) {
    const std::wstring exe = packDir + L"\\holo-media.exe";
    {
        // A dev pack made from dummy files (make-dev-pack's own tests) has no runnable helper: only a real one runs.
        std::ifstream f(exe, std::ios::binary);
        char mz[2] = {};
        f.read(mz, 2);
        if (mz[0] != 'M' || mz[1] != 'Z') {
            printf("    dev pack helper is not an executable; real helper case skipped\n");
            return;
        }
    }
    const std::wstring ll = LocalLow();
    const std::wstring root = ll + L"\\Melange\\media-selftest-real-" + std::to_wstring(GetCurrentProcessId());
    helper::LaunchSpec spec;
    spec.exe = exe;
    spec.cacheDir = root + L"\\inst";
    std::string err;
    CHECK(helper::PrepareCacheDir(spec.cacheDir, &err), "real: cache folder (" + err + ")");
    spec.lowIntegrity = true;
    spec.args = {mproto::arg::kParent, std::to_wstring(GetCurrentProcessId()), mproto::arg::kCache, spec.cacheDir, mproto::arg::kLibmpv,
                 packDir + L"\\libmpv-2.dll", mproto::arg::kYtdlp, ytdlp.empty() ? packDir + L"\\yt-dlp.exe" : ytdlp, mproto::arg::kDeno,
                 packDir + L"\\deno.exe", mproto::arg::kSweep};
    {
        helper::Session s;
        std::vector<pipe::Message> got;
        CHECK(StartReady(s, spec, got), "real: the installed holo-media.exe connects and says hello");
        printf("    real helper: mpv '%s', ffmpeg '%s', libtorrent '%s'\n", s.Hello().mpv.c_str(), s.Hello().ffmpeg.c_str(),
               s.Hello().libtorrent.c_str());
        CHECK(s.GotHello() && !s.Hello().mpv.empty() && s.Hello().mpv != "fake", "real: hello names the real mpv");
        CHECK(IntegrityRid(s.ProcessHandle()) == SECURITY_MANDATORY_LOW_RID, "real: the helper runs at Low integrity");
        BOOL inJob = FALSE;
        CHECK(IsProcessInJob(static_cast<HANDLE>(s.ProcessHandle()), nullptr, &inJob) && inJob, "real: the helper is in the job");
        shmio::Section sec;
        const std::string name = shmio::SectionName(GetCurrentProcessId(), helper::RandomHex(8), 7);
        CHECK(sec.Create(name, true, &err), "real: section created (" + err + ")");
        // A torrent item before any bt.add: the engine does not exist yet (no listen socket), so the helper refuses it.
        pipe::OpenCmd o;
        o.id = 7;
        o.gen = 1;
        o.shm = name;
        o.ih = "0123456789abcdef0123456789abcdef01234567";
        o.file = 0;
        s.Send(o);
        const bool refused = WaitFor(s, got, 5000, [&] {
            const pipe::ErrorEvt* e = Last<pipe::ErrorEvt>(got);
            return e && e->id == 7 && e->code == mproto::err::kTorrent;
        });
        CHECK(refused, "real: a torrent open before bt.add is refused with error torrent");
        CHECK(s.GetState() == helper::Session::State::Ready, "real: the helper is still up after the refusal");
        s.Stop(10000);  // a kill only after 10 s (it exits 0 too), so exit 0 within 5 s is the helper's own quit
        const ULONGLONG end = GetTickCount64() + 5000;
        DWORD code = STILL_ACTIVE;
        while (GetTickCount64() < end && GetExitCodeProcess(static_cast<HANDLE>(s.ProcessHandle()), &code) && code == STILL_ACTIVE) Sleep(20);
        CHECK(code == 0, "real: quit exits 0 (" + std::to_string(code) + ")");
    }
    Sleep(500);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

inline void Run() {
    Policy();
    Staleness();
    InboxCases();
    EndToEnd();
}
}  // namespace host_cases
