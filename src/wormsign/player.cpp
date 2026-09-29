// The replay player. Arming loads a recording and forces the menu's seeds and pre-match draws per call site, so the
// Quick Game started next is the recorded one. In the match it blocks live inputs, injects the recorded ones at
// their call times, checks the setup at tick 1 and compares every tick's hash with the recording.
//   wormsign.replay arm <file>|disarm|pause|resume|speed <x>|runto <tick>|restart [tick]|status
#include "wormsign/player.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "melange/jlog.h"
#include "melange/mods.h"
#include "melange/testcmd.h"
#include "wormsign/capture.h"
#include "wormsign/clock.h"
#include "wormsign/contrib.h"
#include "wormsign/divergence.h"
#include "wormsign/format.h"
#include "wormsign/recording.h"
#include "wormsign/rngtap.h"
#include "wormsign/session.h"
#include "wormsign/setup.h"

namespace melange::wormsign {
namespace player {
void RegisterPanel();
}

namespace {
constexpr uintptr_t kRngState[2] = {0x96d034, 0x96d040};
constexpr const char* kCompName[kEngineComps] = {"time+rng", "turn", "worms", "tasks", "projectiles", "teams"};

std::recursive_mutex g_mu;
std::shared_ptr<Recording> g_rec;
std::wstring g_path;
ReplayCore g_core;
VirtualClock g_clock;
PlayState g_state = PlayState::Idle;
char g_error[128] = "";
std::string g_note;
uint32_t g_serial = 0, g_lastTick = 0, g_runTo = 0, g_speedMilli = 1000, g_restartTo = 0;
bool g_paused = false, g_clockOwned = false, g_haveDiv = false, g_restart = false, g_installed = false;
player::DivergenceDetail g_div{};
int g_tickObs = 0, g_sessObs = 0;
std::atomic<bool> g_gate{false}, g_passLocal{false}, g_onlineMatch{false};
std::atomic<uint32_t> g_blocked{0};

using Lock = std::lock_guard<std::recursive_mutex>;

bool Replaying() { return g_state == PlayState::Loading || g_state == PlayState::Playing || g_state == PlayState::Diverged; }
bool OurSession() { return g_serial && session::Open() && MatchSerial() == g_serial; }
bool OnlineNow() { return g_onlineMatch.load() || mods::InLobby(); }

void SetError(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_error, sizeof g_error, fmt, ap);
    va_end(ap);
}

// Hands every input back to the player; the scheduler clock stays ours (at 1x) until the session ends, since the
// offset it built up cannot be undone mid-match.
void StopSteering() {
    g_gate = false;
    capture::SetGate(nullptr);
    clock::SetPreTick(nullptr);
    rngtap::SetForcedDraw(nullptr);
    rngtap::SetForcedSeed(nullptr);
    if (g_tickObs) RemoveOnTickEnd(g_tickObs);
    g_tickObs = 0;
    g_paused = false;
    g_runTo = 0;
    g_clock.SetPaused(false);
    g_clock.SetFast(false);
    g_clock.SetCap(false);
    g_clock.SetSpeedMilli(1000);
}

void ReleaseClock() {
    clock::SetNowFilter(nullptr);
    g_clockOwned = false;
}

void Summary(const char* why) {
    const auto& n = g_core.Count();
    LOG_INFO("[wormsign] replay %s: compared %u, matched %u%s; inputs %u injected, %u skipped, %u late, %u failed, %u live "
             "blocked; seeds forced %u, pre-match draws forced %u (unforced %u)",
             why, n.compared, n.matched, g_haveDiv ? " (diverged)" : "", n.injected, n.skipped, n.late, n.failed,
             g_blocked.load(), n.seedsForced, n.drawsForced, n.drawsUnforced);
    jlog::Rec("wormsign", jlog::Level::Info, "replay end")
        .Str("why", why).Uint("compared", n.compared).Uint("matched", n.matched).Uint("firstDivergence", n.firstDivergence)
        .Uint("injected", n.injected).Uint("seedsForced", n.seedsForced).Uint("drawsForced", n.drawsForced);
}

void Fail(const char* msg) {
    StopSteering();
    g_state = PlayState::Failed;
    SetError("%s", msg);
    LOG_WARN("[wormsign] replay failed: %s", msg);
    jlog::Rec("wormsign", jlog::Level::Warn, "replay failed").Str("error", msg);
}

void Finish(const char* why) {
    StopSteering();
    g_state = PlayState::Finished;
    Summary(why);
}

void DisarmLocked(const char* why) {
    StopSteering();
    if (!OurSession()) ReleaseClock();
    const bool was = g_state != PlayState::Idle;
    g_state = PlayState::Idle;
    g_restart = false;
    if (why) {
        SetError("%s", why);
        if (was) LOG_WARN("[wormsign] replay %s", why);
    } else {
        g_error[0] = 0;
    }
}

bool SeedHook(int kind, uint32_t caller, uint32_t* value) {
    Lock lk(g_mu);
    if (g_state != PlayState::Armed || session::Open()) return false;
    if (OnlineNow()) {
        DisarmLocked("disarmed: replays run offline only");
        return false;
    }
    const uint32_t was = *value;
    if (!g_core.ForceSeed(kind, caller, value)) return false;
    LOG_INFO("[wormsign] replay: %s seed %08x -> %08x (caller %08x)", kind ? "second" : "logic", was, *value, caller);
    return true;
}

bool DrawHook(int rng, uint32_t ret, uint32_t* bits) {
    Lock lk(g_mu);
    if (g_state != PlayState::Armed || session::Open() || OnlineNow()) return false;
    uint32_t state;
    if (!g_core.ForceDraw(rng, ret, &state, bits)) return false;
    *reinterpret_cast<volatile uint32_t*>(kRngState[rng & 1]) = state;
    return true;
}

capture::Gate GateHook(int, uint16_t id, bool injected) {
    if (injected || !g_gate.load(std::memory_order_relaxed)) return capture::Gate::Pass;
    if (g_passLocal.load(std::memory_order_relaxed) && capture::LocalOnly(id)) return capture::Gate::Pass;
    g_blocked.fetch_add(1, std::memory_order_relaxed);
    return capture::Gate::Block;
}

void PreTick(uint32_t, uint32_t timeMs) {
    Lock lk(g_mu);
    if (!Replaying()) return;
    bool failed = false;
    g_core.Due(timeMs, [&](const rec::Input& i) {
        if (failed) return false;
        const char* str = i.type == rec::kSendString ? i.str.c_str() : nullptr;
        failed = !capture::InjectSend(i.type, i.id, i.a, i.b, str, i.time);
        return !failed;
    });
    if (failed) Fail("input injection is not available in this build");
}

int NowFilter(int realNow) {
    Lock lk(g_mu);
    if (g_runTo && Replaying()) {
        const int base = clock::TimeBase();
        g_clock.SetCap(base != 0, static_cast<int64_t>(base) + static_cast<int64_t>(kTickMs) * (g_runTo + 1));
    } else {
        g_clock.SetCap(false);
    }
    return g_clock.Filter(realNow, clock::Paused());
}

bool CheckSetup() {
    if (g_rec->setup.empty()) {
        g_note += g_note.empty() ? "" : "; ";
        g_note += "the recording has no setup fingerprint: setup not verified";
        return true;
    }
    setup::Data live;
    if (!setup::Capture(&live)) {
        g_note += g_note.empty() ? "" : "; ";
        g_note += "the setup could not be read here (GameState off?): not verified";
        return true;
    }
    std::string why;
    if (!setup::Compare(g_rec->setup, setup::ToJson(live), &why)) {
        Fail(("setup differs: " + why + ". Start a Quick Game from the main menu").c_str());
        return false;
    }
    if (!why.empty()) g_note += (g_note.empty() ? "" : "; ") + why;
    LOG_INFO("[wormsign] replay: setup matches the recording (%s, %zu teams)", live.landFile.c_str(), live.teams.size());
    return true;
}

void TickEnd(const TickHash& h, void*) {
    Divergence raise{};
    bool doRaise = false;
    {
        Lock lk(g_mu);
        if (!Replaying() || !OurSession()) return;
        g_lastTick = h.tick;
        if (g_state == PlayState::Loading) {
            if (!CheckSetup()) return;
            g_state = PlayState::Playing;
            jlog::Rec("wormsign", jlog::Level::Info, "replay playing").Uint("tick", h.tick).Uint("ticks", g_rec->lastTick);
        }
        TickHash rec{};
        uint8_t mask = 0;
        if (g_core.Compare(h, &rec, &mask) == ReplayCore::Cmp::Mismatch && !g_haveDiv) {
            g_haveDiv = true;
            g_state = PlayState::Diverged;
            Divergence& d = g_div.d;
            d = Divergence{};
            d.source = Source::Replay;
            d.serial = g_serial;
            d.tick = h.tick;
            d.oursEngine = h.engine;
            d.theirsEngine = rec.engine;
            d.oursMods = h.mods;
            d.theirsMods = rec.mods;
            d.compMask = mask;
            g_div.recorded = rec;
            g_div.live = h;
            std::string comps;
            for (int i = 0; i < kEngineComps; ++i)
                if (mask & (1u << i)) comps += (comps.empty() ? "" : ",") + std::string(kCompName[i]);
            if (comps.empty()) comps = "mods";
            SetError("diverged at tick %u (%s)", h.tick, comps.c_str());
            LOG_WARN("[wormsign] REPLAY DIVERGENCE at tick %u: recorded %016llx now %016llx, mods %016llx/%016llx; "
                     "differing: %s; rng %08x/%08x",
                     h.tick, rec.engine, h.engine, rec.mods, h.mods, comps.c_str(), rec.rngLogic, h.rngLogic);
            jlog::Rec("wormsign", jlog::Level::Warn, "replay divergence")
                .Uint("tick", h.tick).Hex("recorded", rec.engine).Hex("live", h.engine).Str("comps", comps);
            raise = d;
            doRaise = true;
        }
        if (g_runTo && h.tick >= g_runTo) {
            LOG_INFO("[wormsign] replay: reached tick %u, paused", h.tick);
            g_runTo = 0;
            g_paused = true;
            g_clock.SetFast(false);
            g_clock.SetCap(false);
            g_clock.SetPaused(true);
        }
        if (h.tick >= g_rec->lastTick) Finish("finished");
    }
    if (doRaise) divergence::Raise(raise);
}

void OnSessionFn(bool begin, uint32_t serial, void*) {
    std::wstring rearm;
    uint32_t rearmTo = 0;
    {
        Lock lk(g_mu);
        if (begin) {
            if (g_state != PlayState::Armed) return;
            rngtap::SetForcedDraw(nullptr);
            rngtap::SetForcedSeed(nullptr);
            if (OnlineNow()) {
                DisarmLocked("disarmed: replays run offline only");
                return;
            }
            g_serial = serial;
            g_state = PlayState::Loading;
            g_haveDiv = false;
            g_lastTick = 0;
            g_blocked = 0;
            if (!g_core.Count().seedsForced) {
                Fail("no seed was forced: arm the replay first, then start a Quick Game from the main menu");
                return;
            }
            g_clock.Reset();
            g_clock.SetSpeedMilli(g_speedMilli);
            g_clock.SetPaused(false);
            g_clock.SetFast(g_runTo != 0);
            g_clock.SetCap(false);
            clock::SetNowFilter(&NowFilter);
            g_clockOwned = true;
            g_gate = true;
            capture::SetGate(&GateHook);
            clock::SetPreTick(&PreTick);
            g_tickObs = OnTickEnd(&TickEnd, nullptr, -100);
            LOG_INFO("[wormsign] replay: session %u starts, %zu inputs to inject, ticks %u..%u", serial,
                     g_rec->inputs.size(), g_rec->firstTick, g_rec->lastTick);
            return;
        }
        if (!g_serial || serial != g_serial) return;
        if (g_clockOwned) ReleaseClock();
        if (Replaying()) {
            const uint32_t at = g_lastTick, last = g_rec ? g_rec->lastTick : 0;
            StopSteering();
            g_state = PlayState::Finished;
            SetError("the match ended at tick %u of %u", at, last);
            Summary("stopped: the match ended");
        }
        g_serial = 0;
        if (g_restart) {
            g_restart = false;
            rearm = g_path;
            rearmTo = g_restartTo;
        }
    }
    if (!rearm.empty()) {
        char err[128];
        if (Arm(rearm.c_str(), err, sizeof err)) {
            if (rearmTo) RunTo(rearmTo);
        } else {
            LOG_WARN("[wormsign] replay restart: %s", err);
        }
    }
}

bool LoadFile(const std::wstring& path, std::shared_ptr<Recording>* out, std::string* err) {
    wsr::Reader r;
    if (!r.OpenFile(path, err)) return false;
    auto rec = std::make_shared<Recording>();
    if (!LoadRecording(r, rec.get(), err)) return false;
    *out = std::move(rec);
    return true;
}

std::wstring Resolve(const std::wstring& p) {
    if (p.find_first_of(L"\\/:") != std::wstring::npos) return p;
    std::wstring f = player::ReplaysDir() + L"\\" + p;
    if (p.size() < 4 || _wcsicmp(p.c_str() + p.size() - 4, L".wsr") != 0) f += L".wsr";
    return f;
}

void ParseSkip(uint16_t* id, uint32_t* time) {
    const std::string s = config::GetString("Wormsign", "ReplaySkip", "");
    *id = 0;
    *time = 0;
    if (s.empty()) return;
    char* e = nullptr;
    const unsigned long v = strtoul(s.c_str(), &e, 0);
    if (v == 0 || v > 0xffff) return;
    *id = static_cast<uint16_t>(v);
    if (e && *e == '@') *time = strtoul(e + 1, nullptr, 10);
    LOG_WARN("[wormsign] replay: ReplaySkip drops input id %04x%s (test setting)", *id, *time ? " at one time" : "");
}

// ---------------------------------------------------------------- test verbs
void LogStatus() {
    const PlayStatus s = Status();
    const player::Info i = player::GetInfo();
    static const char* const kState[] = {"idle", "armed", "loading", "playing", "paused", "finished", "diverged", "failed"};
    LOG_INFO("[wormsign] replay status: %s tick %u/%u compared %u matched %u speed %.2f runTo %u inputs %zu/%zu "
             "(skipped %u, late %u) blocked %u seeds %u draws %u/%u%s%s%s%s",
             kState[static_cast<int>(s.state)], s.tick, s.ticks, s.compared, s.matched, static_cast<double>(s.speed),
             i.runTo, i.nextInput, i.inputs, i.n.skipped, i.n.late, i.blocked, i.n.seedsForced, i.n.drawsForced,
             i.n.drawsForced + i.n.drawsUnforced, s.error[0] ? " error: " : "", s.error, i.note.empty() ? "" : " note: ",
             i.note.c_str());
}

bool Verb(std::string_view args, void*) {
    while (!args.empty() && args.front() == ' ') args.remove_prefix(1);
    const size_t sp = args.find(' ');
    const std::string_view sub = args.substr(0, sp);
    std::string rest = sp == std::string_view::npos ? "" : std::string(args.substr(sp + 1));
    while (!rest.empty() && rest.back() == ' ') rest.pop_back();
    bool ok = true;
    char err[128] = "";
    if (sub == "arm") {
        ok = Arm(game::Widen(rest).c_str(), err, sizeof err);
        if (!ok) LOG_WARN("[wormsign] replay arm refused: %s", err);
    } else if (sub == "disarm") {
        Disarm();
    } else if (sub == "pause" || sub == "resume") {
        ok = SetPaused(sub == "pause");
    } else if (sub == "speed") {
        ok = SetSpeed(static_cast<float>(atof(rest.c_str())));
    } else if (sub == "runto") {
        ok = RunTo(strtoul(rest.c_str(), nullptr, 10));
    } else if (sub == "restart") {
        ok = player::Restart(strtoul(rest.c_str(), nullptr, 10), err, sizeof err);
        if (!ok) LOG_WARN("[wormsign] replay restart refused: %s", err);
    } else if (sub != "status") {
        LOG_WARN("[wormsign] wormsign.replay: unknown '%.*s'", static_cast<int>(sub.size()), sub.data());
        return false;
    }
    LogStatus();
    return ok;
}
}  // namespace

// ---------------------------------------------------------------- public API
bool Arm(const wchar_t* path, char* err, size_t errLen) {
    auto refuse = [&](const std::string& why) {
        if (err && errLen) snprintf(err, errLen, "%s", why.c_str());
        return false;
    };
    if (!Enabled()) return refuse("Wormsign is off");
    if (!path || !*path) return refuse("no recording given");
    const std::wstring full = Resolve(path);
    std::shared_ptr<Recording> rec;
    std::string why;
    if (!LoadFile(full, &rec, &why)) return refuse(why);
    ArmEnv env;
    const mods::ContentId content = mods::LocalContent();
    env.contentHash = content.vanilla ? "" : content.hash;
    env.anyContent = config::GetBool("Wormsign", "ReplayAnyContent", false);
    env.inLobby = mods::InLobby();
    env.online = g_onlineMatch.load();
    env.inMatch = session::Open();
    why = ArmRefusal(*rec, env);
    if (!why.empty()) return refuse(why);

    Lock lk(g_mu);
    if (Replaying()) return refuse("a replay is playing");
    StopSteering();
    if (g_clockOwned && !OurSession()) ReleaseClock();
    const bool sameEngine = rec->engineHash == static_cast<int>(kEngineHashVersion);
    const std::string key = contrib::ReplayKey();
    g_note.clear();
    if (!sameEngine) g_note = "recorded with engine hash v" + std::to_string(rec->engineHash) + ": ticks are not compared";
    else if (rec->contributors != key)
        g_note = "mod hash contributors differ (" + (rec->contributors.empty() ? std::string("none") : rec->contributors) +
                 " recorded): only the engine hash is compared";
    if (!rec->complete) g_note += std::string(g_note.empty() ? "" : "; ") + "the recording is incomplete";
    g_core.Start(rec, sameEngine, sameEngine && rec->contributors == key);
    uint16_t skipId;
    uint32_t skipTime;
    ParseSkip(&skipId, &skipTime);
    g_core.SetSkip(skipId, skipTime);
    g_rec = rec;
    g_path = full;
    g_state = PlayState::Armed;
    g_error[0] = 0;
    g_serial = 0;
    g_lastTick = 0;
    g_runTo = 0;
    g_paused = false;
    g_haveDiv = false;
    g_restart = false;
    g_blocked = 0;
    g_passLocal = config::GetBool("Wormsign", "ReplayPassLocal", false);
    rngtap::SetForcedSeed(&SeedHook);
    rngtap::SetForcedDraw(&DrawHook);
    LOG_INFO("[wormsign] replay armed: %s (%zu seeds, %zu pre-match draws, %zu inputs, ticks %u..%u)%s%s",
             game::Narrow(full).c_str(), rec->seeds.size(), rec->draws.size(), rec->inputs.size(), rec->firstTick,
             rec->lastTick, g_note.empty() ? "" : "; ", g_note.c_str());
    jlog::Rec("wormsign", jlog::Level::Info, "replay armed")
        .Str("file", game::Narrow(full)).Uint("inputs", rec->inputs.size()).Uint("ticks", rec->lastTick).Str("note", g_note);
    return true;
}

void Disarm() {
    Lock lk(g_mu);
    DisarmLocked(nullptr);
}

bool SetPaused(bool p) {
    Lock lk(g_mu);
    if (!Replaying()) return false;
    g_paused = p;
    g_clock.SetPaused(p);
    if (p) {
        g_runTo = 0;
        g_clock.SetFast(false);
    }
    return true;
}

bool SetSpeed(float x) {
    if (!(x >= 0.25f && x <= 8.0f)) return false;
    Lock lk(g_mu);
    if (g_state != PlayState::Armed && !Replaying()) return false;
    g_speedMilli = static_cast<uint32_t>(x * 1000.0f + 0.5f);
    g_clock.SetSpeedMilli(g_speedMilli);
    return true;
}

bool RunTo(uint32_t tick) {
    Lock lk(g_mu);
    if (!g_rec || (g_state != PlayState::Armed && !Replaying())) return false;
    const uint32_t target = (std::min)(tick, g_rec->lastTick);
    if (g_state == PlayState::Armed) {
        g_runTo = target;
        return target != 0;
    }
    if (target <= Tick()) return false;
    g_runTo = target;
    g_paused = false;
    g_clock.SetPaused(false);
    g_clock.SetFast(true);
    return true;
}

PlayStatus Status() {
    Lock lk(g_mu);
    PlayStatus s{};
    s.state = g_state;
    if (g_paused && (g_state == PlayState::Playing || g_state == PlayState::Diverged)) s.state = PlayState::Paused;
    s.tick = OurSession() ? Tick() : g_lastTick;
    s.ticks = g_rec && g_state != PlayState::Idle ? g_rec->lastTick : 0;
    s.compared = g_core.Count().compared;
    s.matched = g_core.Count().matched;
    s.speed = static_cast<float>(g_speedMilli) / 1000.0f;
    memcpy(s.error, g_error, sizeof s.error);
    return s;
}

namespace player {
std::wstring ReplaysDir() {
    std::wstring out;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) out = p;
    CoTaskMemFree(p);
    return out + L"\\Melange\\replays";
}

Info GetInfo() {
    Lock lk(g_mu);
    Info i;
    if (g_state == PlayState::Idle) return i;
    i.path = g_path;
    i.note = g_note;
    i.n = g_core.Count();
    i.inputs = g_rec ? g_rec->inputs.size() : 0;
    i.nextInput = g_core.NextInput();
    i.runTo = g_runTo;
    i.blocked = g_blocked.load();
    i.restartPending = g_restart;
    return i;
}

bool LastDivergence(DivergenceDetail* out) {
    Lock lk(g_mu);
    if (!g_haveDiv || g_state == PlayState::Idle) return false;
    *out = g_div;
    return true;
}

bool Restart(uint32_t runTo, char* err, size_t errLen) {
    std::wstring path;
    {
        Lock lk(g_mu);
        if (g_path.empty() || !g_rec) {
            if (err && errLen) snprintf(err, errLen, "nothing to restart");
            return false;
        }
        if (OurSession()) {
            g_restart = true;
            g_restartTo = runTo;
            SetError("restart: quit to the main menu and start the Quick Game again");
            LOG_INFO("[wormsign] replay restart pending: re-armed when this match ends");
            return true;
        }
        path = g_path;
    }
    if (!Arm(path.c_str(), err, errLen)) return false;
    if (runTo) RunTo(runTo);
    return true;
}

void Install() {
    if (g_installed) return;
    g_installed = true;
    config::EnsureKey("Wormsign", "ReplayPassLocal", "0");
    g_sessObs = OnSession(&OnSessionFn, nullptr);
    events::Subscribe(events::Event::MatchStart, [] { g_onlineMatch = true; });
    events::Subscribe(events::Event::MatchEnd, [] { g_onlineMatch = false; });
    events::Subscribe(events::Event::LobbyEnter, [] {
        Lock lk(g_mu);
        if (g_state == PlayState::Armed) DisarmLocked("disarmed: replays run offline only");
    });
    testcmd::Register("wormsign.replay", &Verb);
    RegisterPanel();
}

void Uninstall() {
    Lock lk(g_mu);
    DisarmLocked(nullptr);
    ReleaseClock();
    if (g_sessObs) RemoveOnSession(g_sessObs);
    g_sessObs = 0;
}
}  // namespace player
}  // namespace melange::wormsign
