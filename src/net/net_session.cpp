// NetSession: instruments the online match lifecycle and repairs state the game forgets to reset
// between back-to-back matches in the same lobby (the "second game freezes / session no longer
// available" bug). Analysis and addresses: docs/re-notes.md, docs/netcode.md.
//
// Every fix is a separate ini switch so they can be A/B tested; all default on except the
// behaviour-changing DeadPeerForfeit.
#include <windows.h>

#include <safetyhook.hpp>

#include <cstdio>
#include <string>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "net/net.h"
#include "wumfix/jlog.h"  // component C's only edit to NetSession: mirror state transitions into the JSONL log

namespace {
namespace A = wf::wum::addr;
namespace S = wf::wum::state;
namespace O = wf::wum::off;
using wf::wum::Read;

// ---------------------------------------------------------------- state dump
std::string DescribePlayers(uintptr_t ns) {
    std::string out;
    int n = wf::wum::PlayerCount(ns);
    for (int i = 0; i < n && i < 16; ++i) {
        uintptr_t p = wf::wum::PlayerAt(ns, i);
        char b[160];
        snprintf(b, sizeof(b), "\r\n      player[%d] %08x local=%u loaded=%u surrenderNext=%u flag53=%u channel=%08x", i,
                 static_cast<unsigned>(p), Read<uint8_t>(p + O::PlayerIsLocal), Read<uint8_t>(p + O::PlayerLoaded),
                 Read<uint8_t>(p + O::PlayerSurrenderNext), Read<uint8_t>(p + O::PlayerSurrender2),
                 Read<uint32_t>(p + O::PlayerChannel));
        out += b;
    }
    return out;
}

void DumpState(const char* why) {
    uintptr_t ns = wf::wum::NetService();
    if (!ns) {
        WF_INFO("[net] %s: NetService not created", why);
        return;
    }
    uintptr_t session = Read<uint32_t>(ns + O::Session);
    uintptr_t throttle = Read<uint32_t>(ns + O::Throttle);
    WF_INFO("[net] %s: state=%s viabilityArmed=%u begin=%u ingame=%u ended=%u curSurrendered=%u throttleMask=%02x "
            "session=%08x(bits=%04x players=%d nOffset=%d) players=%d%s",
            why, wf::wum::StateName(Read<uint32_t>(ns + O::State)), Read<uint8_t>(ns + O::ViabilityArmed),
            Read<uint8_t>(ns + O::BeginGameDone), Read<uint8_t>(ns + O::InGameFlag), Read<uint8_t>(ns + O::GameEnded),
            Read<uint8_t>(ns + O::CurrentSurrendered), Read<uint8_t>(throttle + O::ThrottleMask),
            static_cast<unsigned>(session), Read<uint32_t>(session + O::SessionStateBits),
            Read<int32_t>(session + O::SessionPlayerCount), Read<int32_t>(session + O::SessionNOffset),
            wf::wum::PlayerCount(ns), DescribePlayers(ns).c_str());
}

// ---------------------------------------------------------------- fixes
struct Fixes {
    bool resetSurrender = true;
    bool resetThrottleMask = true;
    bool resetViabilityOffset = true;
    bool releaseStuckPause = true;
} g_fix;

int g_matchNumber = 0;

// Called on every return to the lobby and again as the next match begins: clears per-match state
// that the game only resets when the process restarts.
// Candidate D (re/notes/freeze.md): a NetThrottle pause taken after ProcessWinOrDraw (turn-start or
// host-migration pause) survives into the lobby because WaitingConnections turns autopause off without
// unpausing - the app pause refcount stays >0 and the next match's task manager never runs.
// Mirrors what AbortGame (0x70864c) does: release the throttle, then drain the pause refcount.
int ReleaseStuckPause(uintptr_t ns, const char* when) {
    int changes = 0;
    uintptr_t thr = Read<uint32_t>(ns + O::Throttle);
    if (thr && Read<uint8_t>(thr + O::ThrottlePaused)) {
        WF_WARN("[fix] %s: NetThrottle still paused -> SetPaused(0)", when);
        reinterpret_cast<void(__thiscall*)(uintptr_t, bool)>(A::ThrottleSetPaused)(thr, false);
        ++changes;
    }
    uintptr_t tm = Read<uint32_t>(A::TaskManagerPtr);
    for (int i = 0; i < 16 && tm && Read<uint8_t>(tm + 0x3c); ++i) {
        WF_WARN("[fix] %s: task manager still paused (app refcount %d) -> Unpause()", when,
                Read<int32_t>(Read<uint32_t>(A::AppPtr) + 0x70));
        reinterpret_cast<void(__cdecl*)()>(A::Unpause)();
        ++changes;
    }
    return changes;
}

void ResetStaleMatchState(const char* when, bool inLobby) {
    uintptr_t ns = wf::wum::NetService();
    if (!ns) return;
    int changes = 0;

    if (g_fix.resetSurrender) {
        // Hypothesis #1: NetPlayer+0x52 "surrender at next turn" survives into the next match; at that
        // player's first turn only some machines force a surrender -> desync -> OutOfSynch / stall.
        int n = wf::wum::PlayerCount(ns);
        for (int i = 0; i < n && i < 16; ++i) {
            uintptr_t p = wf::wum::PlayerAt(ns, i);
            if (!p) continue;
            for (uintptr_t f : {O::PlayerSurrenderNext, O::PlayerSurrender2}) {
                if (Read<uint8_t>(p + f)) {
                    WF_WARN("[fix] %s: player[%d] stale surrender flag +0x%02x=1 -> 0", when, i, static_cast<unsigned>(f));
                    wf::wum::WriteByte(p + f, 0);
                    ++changes;
                }
            }
        }
        if (Read<uint8_t>(ns + O::CurrentSurrendered)) {
            WF_WARN("[fix] %s: stale currentSurrendered=1 -> 0", when);
            wf::wum::WriteByte(ns + O::CurrentSurrendered, 0);
            ++changes;
        }
    }

    if (g_fix.resetThrottleMask) {
        // Hypothesis #6: a host-migration discard zeroes the NetThrottle received-types mask and nothing
        // restores it, so the throttle pauses every tick of the next match.
        uintptr_t t = Read<uint32_t>(ns + O::Throttle);
        uint8_t mask = Read<uint8_t>(t + O::ThrottleMask, 0x3f);
        if (t && mask != 0x3f) {
            WF_WARN("[fix] %s: NetThrottle mask %02x -> 3f", when, mask);
            wf::wum::WriteByte(t + O::ThrottleMask, 0x3f);
            ++changes;
        }
    }

    if (g_fix.resetViabilityOffset) {
        // Hypothesis #3: session nOffset stays -1 after a host loss, making CheckViability fail forever.
        uintptr_t s = Read<uint32_t>(ns + O::Session);
        int32_t off = Read<int32_t>(s + O::SessionNOffset);
        if (s && off != 0) {
            WF_WARN("[fix] %s: session nOffset %d -> 0", when, off);
            wf::wum::WriteInt(s + O::SessionNOffset, 0);
            ++changes;
        }
    }
    if (g_fix.releaseStuckPause && inLobby) changes += ReleaseStuckPause(ns, when);
    WF_INFO("[net] %s: stale-state reset done (%d fields repaired)", when, changes);
}

// ---------------------------------------------------------------- state machine tracking (polled per frame)
uintptr_t g_lastState = 0;

void OnStateChange(uintptr_t from, uintptr_t to) {
    WF_INFO("[net] state %s -> %s", wf::wum::StateName(from), wf::wum::StateName(to));
    wf::jlog::Rec("net", wf::jlog::Level::Info, "state").Int("from", static_cast<int64_t>(from)).Int("to", static_cast<int64_t>(to)).Emit();
    using wf::events::Event;
    if (to == S::WaitingGameStart) {
        if (from == S::WaitingUnload) {
            DumpState("back in lobby after match");
            ResetStaleMatchState("return-to-lobby", true);
        }
        wf::events::Fire(Event::LobbyEnter);
    } else if (to == S::WaitingConnections || (to == S::WaitingLoad && from != S::WaitingConnections)) {
        // WaitingConnections can finish within one frame (seen on the joiner), so the poll may go straight to WaitingLoad.
        ++g_matchNumber;
        DumpState("match starting");
        ResetStaleMatchState("match-start", false);
    } else if (to == S::InGame) {
        WF_INFO("[net] ===== match %d in progress =====", g_matchNumber);
        wf::events::Fire(Event::MatchStart);
    } else if (to == S::ProcessWinOrDraw || (to == S::WaitingUnload && from != S::ProcessWinOrDraw)) {
        // ProcessWinOrDraw usually runs within one frame, so the poll often sees InGame -> WaitingUnload.
        DumpState("match over");
        wf::events::Fire(Event::MatchEnd);
    } else if (to == 0 && from != 0) {
        DumpState("net session closed");
        wf::events::Fire(Event::LobbyLeave);
    }
}

// Logs every change of the values that decide whether a networked match can make progress
// (throttle pause/mask, task-manager pause, surrender flags...). One line per change, so a freeze
// shows exactly which value got stuck.
void WatchProgressState() {
    static std::string last;
    uintptr_t ns = wf::wum::NetService();
    if (!ns) return;
    uintptr_t thr = Read<uint32_t>(ns + O::Throttle), tm = Read<uint32_t>(A::TaskManagerPtr),
              app = Read<uint32_t>(A::AppPtr);
    char b[256];
    snprintf(b, sizeof(b),
             "throttle paused=%u auto=%u mask=%02x | TM paused=%u appPauseRef=%d | curSurrendered=%u ended=%u "
             "ingame=%u",
             Read<uint8_t>(thr + O::ThrottlePaused), Read<uint8_t>(thr + O::ThrottleAuto),
             Read<uint8_t>(thr + O::ThrottleMask), Read<uint8_t>(tm + 0x3c), Read<int32_t>(app + 0x70),
             Read<uint8_t>(ns + O::CurrentSurrendered), Read<uint8_t>(ns + O::GameEnded),
             Read<uint8_t>(ns + O::InGameFlag));
    std::string cur = b;
    int n = wf::wum::PlayerCount(ns);
    for (int i = 0; i < n && i < 8; ++i) {
        uintptr_t p = wf::wum::PlayerAt(ns, i);
        snprintf(b, sizeof(b), " | p%d loaded=%u surr=%u", i, Read<uint8_t>(p + O::PlayerLoaded),
                 Read<uint8_t>(p + O::PlayerSurrenderNext));
        cur += b;
    }
    if (cur != last) {
        last = cur;
        WF_INFO("[watch] %s", cur.c_str());
    }
}

void PollState() {
    uintptr_t s = wf::wum::CurrentState();
    if (s != g_lastState) {
        uintptr_t prev = g_lastState;
        g_lastState = s;
        OnStateChange(prev, s);
    }
}

// ---------------------------------------------------------------- instrumentation hooks (mid-function, at entry)
std::vector<SafetyHookMid> g_hooks;

uint32_t Arg(const safetyhook::Context& c, int i) { return Read<uint32_t>(c.esp + 4 + 4 * i); }
uint32_t RetAddr(const safetyhook::Context& c) { return Read<uint32_t>(c.esp); }

const char* AbortSite(uint32_t ret) {
    switch (ret) {
        case 0x70a7f9: return "A: remote channel dead (NetService::Update)";
        case 0x70ad01: return "B: session not viable";
        case 0x70a375: return "C: host lost while in lobby";
        case 0x7092c0: return "D: time-sync check failed";
        case 0x709336: return "E: turn-end validation failed";
        default: return "other";
    }
}

const char* AbortCode(uint32_t hr) {
    switch (hr) {
        case 0x8021012D: return "Net.NotViable";
        case 0x8021012C: return "Net.OutOfSynch";
        case 0x802100CD: return "Net.RemovedFromSession";
        case 0x802100C9: return "Net.Connect";
        default: return "?";
    }
}

void OnAbortGame(safetyhook::Context& c) {
    uint32_t hr = Arg(c, 0), ret = RetAddr(c);
    WF_ERROR("[net] ===== AbortGame(%08x %s) from %s  [site %s] match %d", hr, AbortCode(hr),
             wf::game::DescribeAddress(ret).c_str(), AbortSite(ret), g_matchNumber);
    DumpState("at abort");
}

void OnSurrender(safetyhook::Context& c) {
    WF_INFO("[net] SurrenderPlayer(player %08x) from %s", Arg(c, 0), wf::game::DescribeAddress(RetAddr(c)).c_str());
}

void OnTurnStarted(safetyhook::Context& c) {
    // 0x709827 is an event handler (arg = event message); it looks the player up with CurrentPlayer itself.
    uintptr_t p = wf::wum::CurrentPlayer(c.ecx);
    WF_INFO("[net] turn started: player %08x %s surrenderNext=%u", static_cast<unsigned>(p),
            !p ? "(none)" : Read<uint8_t>(p + O::PlayerIsLocal) ? "local" : "remote",
            p ? Read<uint8_t>(p + O::PlayerSurrenderNext) : 0);
}

void OnCheckViability(safetyhook::Context& c) {
    static int lastOff = 0x7fffffff, lastCount = -1;
    int nOffset = static_cast<int>(Arg(c, 0));
    int count = Read<int32_t>(c.ecx + O::SessionPlayerCount);
    if (nOffset != lastOff || count != lastCount) {
        lastOff = nOffset;
        lastCount = count;
        WF_INFO("[net] CheckViability(session %08x, nOffset=%d) sessionPlayers=%d", static_cast<unsigned>(c.ecx), nOffset,
                count);
    }
}

void OnConnCtor(safetyhook::Context& c) {
    WF_INFO("[net] XSteamConnection created %08x from %s", static_cast<unsigned>(c.ecx),
            wf::game::DescribeAddress(RetAddr(c)).c_str());
}

void OnConnDtor(safetyhook::Context& c) {
    uintptr_t t = c.ecx;
    WF_INFO("[net] XSteamConnection destroyed %08x sendSeq=%u recvSeq=%u state=%u", static_cast<unsigned>(t),
            Read<uint32_t>(t + O::ConnSendSeq), Read<uint32_t>(t + O::ConnRecvSeq), Read<uint32_t>(t + O::ConnState));
}

void OnNewSender(safetyhook::Context&) { WF_WARN("[net] P2P packet from unknown SteamID -> new XSteamConnection"); }

bool Mid(uintptr_t addr, safetyhook::MidHookFn fn, const char* what) {
    auto h = safetyhook::create_mid(addr, fn);
    if (!h) {
        WF_ERROR("[net] failed to hook %s at %08x", what, static_cast<unsigned>(addr));
        return false;
    }
    g_hooks.push_back(std::move(h));
    return true;
}

class NetSession final : public wf::Module {
public:
    const char* Name() const override { return "NetSession"; }
    const char* Description() const override { return "match lifecycle tracing + back-to-back match state repair"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 20; }

    bool Install() override {
        g_fix.resetSurrender = Bool("FixStaleSurrender", true);
        g_fix.resetThrottleMask = Bool("FixThrottleMask", true);
        g_fix.resetViabilityOffset = Bool("FixViabilityOffset", true);
        bool trace = Bool("Trace", true);
        bool traceTransport = Bool("TraceTransport", true);
        g_fix.releaseStuckPause = Bool("FixStuckPause", true);
        static bool watch = Bool("Watch", true);
        static bool forceNetLog = Bool("ForceEngineNetLog", true);

        // Instrumentation. Entry prologues verified in re-notes; SafetyHook relocates them safely.
        bool ok = Mid(A::AbortGame, &OnAbortGame, "AbortGame");
        if (trace) {
            ok &= Mid(A::SurrenderPlayer, &OnSurrender, "SurrenderPlayer");
            ok &= Mid(A::TurnStarted, &OnTurnStarted, "TurnStarted");
            ok &= Mid(A::CheckViability, &OnCheckViability, "CheckViability");
        }
        if (traceTransport) {
            ok &= Mid(A::SteamConnCtor, &OnConnCtor, "XSteamConnection ctor");
            ok &= Mid(A::SteamConnDtor, &OnConnDtor, "XSteamConnection dtor");
            ok &= Mid(A::SteamConnNewSender, &OnNewSender, "unknown-sender path");
        }

        // Behaviour change (off by default): a dead peer in a 2-player game forfeits instead of aborting
        // the whole session with "This session is no longer available".
        if (Bool("DeadPeerForfeit", false)) {
            if (wf::mem::Expect(A::DeadChannelBranch, {0x0f, 0x87, 0x0b, 0x04, 0x00, 0x00})) {
                const uint8_t jmp[] = {0xe9, 0x0c, 0x04, 0x00, 0x00, 0x90};  // jmp 0x70abf6
                wf::mem::Write(A::DeadChannelBranch, jmp, sizeof(jmp));
                WF_INFO("[net] DeadPeerForfeit enabled");
            }
        }

        wf::events::Subscribe(wf::events::Event::Frame, [] {
            PollState();
            if (watch && g_lastState) WatchProgressState();
            if (forceNetLog) {  // same as launching with /LOG ALL: the engine's own NetThrottle/NetService logging
                uintptr_t cfg = Read<uint32_t>(A::ConfigPtr);
                if (cfg && !(Read<uint8_t>(cfg + 0x9a) & 2)) wf::wum::WriteByte(cfg + 0x9a, Read<uint8_t>(cfg + 0x9a) | 2);
            }
            if ((GetAsyncKeyState(VK_F11) & 1) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000))
                DumpState("manual (Ctrl+Shift+F11)");
        });
        return ok;
    }

    void Uninstall() override { g_hooks.clear(); }
};
}  // namespace

WUMFIX_MODULE(NetSession);
