// NetSession: traces the online match lifecycle and repairs state the game forgets to reset
// between back-to-back matches in the same lobby.
#include <windows.h>

#include <safetyhook.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "net/net.h"
#include "melange/bus.h"
#include "melange/jlog.h"
#include "mods/simfix.h"

namespace {
namespace A = melange::wum::addr;
namespace S = melange::wum::state;
namespace O = melange::wum::off;
using melange::wum::Read;

std::string DescribePlayers(uintptr_t ns) {
    std::string out;
    int n = melange::wum::PlayerCount(ns);
    for (int i = 0; i < n && i < 16; ++i) {
        uintptr_t p = melange::wum::PlayerAt(ns, i);
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
    uintptr_t ns = melange::wum::NetService();
    if (!ns) {
        LOG_INFO("[net] %s: NetService not created", why);
        return;
    }
    uintptr_t session = Read<uint32_t>(ns + O::Session);
    uintptr_t throttle = Read<uint32_t>(ns + O::Throttle);
    LOG_INFO("[net] %s: state=%s viabilityArmed=%u begin=%u ingame=%u ended=%u curSurrendered=%u throttleMask=%02x "
            "session=%08x(bits=%04x players=%d nOffset=%d) players=%d%s",
            why, melange::wum::StateName(Read<uint32_t>(ns + O::State)), Read<uint8_t>(ns + O::ViabilityArmed),
            Read<uint8_t>(ns + O::BeginGameDone), Read<uint8_t>(ns + O::InGameFlag), Read<uint8_t>(ns + O::GameEnded),
            Read<uint8_t>(ns + O::CurrentSurrendered), Read<uint8_t>(throttle + O::ThrottleMask),
            static_cast<unsigned>(session), Read<uint32_t>(session + O::SessionStateBits),
            Read<int32_t>(session + O::SessionPlayerCount), Read<int32_t>(session + O::SessionNOffset),
            melange::wum::PlayerCount(ns), DescribePlayers(ns).c_str());
}

struct Fixes {
    bool resetSurrender = true;
    bool resetThrottleMask = true;
    bool resetViabilityOffset = true;
    bool releaseStuckPause = true;
    bool skipCameraHold = true;
    bool alignDrumFuse = true;
    bool panelSelectUnderMenu = false;
} g_fix;
bool g_cameraHoldPatched = false;
void SetCameraHoldPatch(bool on);
bool g_panelSelectPatched = false;
void SetPanelSelectPatch(bool on);
void ResetMatchTrace();
void DecideSimFixes();
void ClearSimFixes();

int g_matchNumber = 0;
// FixDrumFuse changes the sim (when a drum explodes), so it runs only when every machine in the match runs it: decided
// once per match from the lobby's agreed list (mods/simfix.h) when the match leaves the lobby, before the sim starts,
// and kept until the match ends. False offline and whenever the lobby has not agreed.
bool g_drumFuseAgreed = false;
bool g_simFixesDecided = false;

// A NetThrottle pause taken late in a match survives into the lobby (WaitingConnections turns autopause
// off without unpausing), so the next match's task manager never runs. Mirrors AbortGame: release the
// throttle, then drain the app pause refcount.
int ReleaseStuckPause(uintptr_t ns, const char* when) {
    int changes = 0;
    uintptr_t thr = Read<uint32_t>(ns + O::Throttle);
    if (thr && Read<uint8_t>(thr + O::ThrottlePaused)) {
        LOG_WARN("[fix] %s: NetThrottle still paused -> SetPaused(0)", when);
        reinterpret_cast<void(__thiscall*)(uintptr_t, bool)>(A::ThrottleSetPaused)(thr, false);
        ++changes;
    }
    uintptr_t tm = Read<uint32_t>(A::TaskManagerPtr);
    for (int i = 0; i < 16 && tm && Read<uint8_t>(tm + 0x3c); ++i) {
        LOG_WARN("[fix] %s: task manager still paused (app refcount %d) -> Unpause()", when,
                Read<int32_t>(Read<uint32_t>(A::AppPtr) + 0x70));
        reinterpret_cast<void(__cdecl*)()>(A::Unpause)();
        ++changes;
    }
    return changes;
}

void ResetStaleMatchState(const char* when, bool inLobby) {
    uintptr_t ns = melange::wum::NetService();
    if (!ns) return;
    int changes = 0;

    if (g_fix.resetSurrender) {
        // A leftover "surrender at next turn" flag makes only some machines surrender -> desync.
        int n = melange::wum::PlayerCount(ns);
        for (int i = 0; i < n && i < 16; ++i) {
            uintptr_t p = melange::wum::PlayerAt(ns, i);
            if (!p) continue;
            for (uintptr_t f : {O::PlayerSurrenderNext, O::PlayerSurrender2}) {
                if (Read<uint8_t>(p + f)) {
                    LOG_WARN("[fix] %s: player[%d] stale surrender flag +0x%02x=1 -> 0", when, i, static_cast<unsigned>(f));
                    melange::wum::WriteByte(p + f, 0);
                    ++changes;
                }
            }
        }
        if (Read<uint8_t>(ns + O::CurrentSurrendered)) {
            LOG_WARN("[fix] %s: stale currentSurrendered=1 -> 0", when);
            melange::wum::WriteByte(ns + O::CurrentSurrendered, 0);
            ++changes;
        }
    }

    if (g_fix.resetThrottleMask) {
        // Host migration zeroes the received-types mask and nothing restores it, so the throttle pauses every tick.
        uintptr_t t = Read<uint32_t>(ns + O::Throttle);
        uint8_t mask = Read<uint8_t>(t + O::ThrottleMask, 0x3f);
        if (t && mask != 0x3f) {
            LOG_WARN("[fix] %s: NetThrottle mask %02x -> 3f", when, mask);
            melange::wum::WriteByte(t + O::ThrottleMask, 0x3f);
            ++changes;
        }
    }

    if (g_fix.resetViabilityOffset) {
        // nOffset stays -1 after a host loss, making CheckViability fail forever.
        uintptr_t s = Read<uint32_t>(ns + O::Session);
        int32_t off = Read<int32_t>(s + O::SessionNOffset);
        if (s && off != 0) {
            LOG_WARN("[fix] %s: session nOffset %d -> 0", when, off);
            melange::wum::WriteInt(s + O::SessionNOffset, 0);
            ++changes;
        }
    }
    if (g_fix.releaseStuckPause && inLobby) changes += ReleaseStuckPause(ns, when);
    LOG_INFO("[net] %s: stale-state reset done (%d fields repaired)", when, changes);
}

uintptr_t g_lastState = 0;

uint32_t SimClock() { return Read<uint32_t>(Read<uint32_t>(A::TaskManagerPtr) + 0x38); }
bool InNetMatch() { return g_lastState == S::InGame; }

// The current player is this machine's: the test behind the engine's own replication gate (0x708fdc, used by
// 0x539c00), false between turns.
bool ActivePlayerHere() {
    const uintptr_t cur = melange::wum::CurrentPlayer(melange::wum::NetService());
    return cur && Read<uint8_t>(cur + O::PlayerIsLocal);
}

void OnStateChange(uintptr_t from, uintptr_t to) {
    LOG_INFO("[net] state %s -> %s", melange::wum::StateName(from), melange::wum::StateName(to));
    melange::jlog::Rec("net", melange::jlog::Level::Info, "state").Int("from", static_cast<int64_t>(from)).Int("to", static_cast<int64_t>(to)).Emit();
    using melange::events::Event;
    if (to == S::WaitingGameStart) {
        if (from == S::WaitingUnload) {
            DumpState("back in lobby after match");
            ResetStaleMatchState("return-to-lobby", true);
        }
        ClearSimFixes();
        melange::events::Fire(Event::LobbyEnter);
    } else if (to == S::WaitingConnections || (to == S::WaitingLoad && from != S::WaitingConnections)) {
        // WaitingConnections can finish within one frame (seen on the joiner), so the poll may go straight to WaitingLoad.
        ++g_matchNumber;
        DumpState("match starting");
        ResetStaleMatchState("match-start", false);
        DecideSimFixes();
    } else if (to == S::InGame) {
        LOG_INFO("[net] ===== match %d in progress =====", g_matchNumber);
        if (!g_simFixesDecided) DecideSimFixes();  // a route that skipped the loading states (not seen; a safety net)
        SetCameraHoldPatch(true);
        SetPanelSelectPatch(true);
        ResetMatchTrace();
        melange::events::Fire(Event::MatchStart);
    } else if (to == S::ProcessWinOrDraw || (to == S::WaitingUnload && from != S::ProcessWinOrDraw)) {
        // ProcessWinOrDraw usually runs within one frame, so the poll often sees InGame -> WaitingUnload.
        DumpState("match over");
        melange::events::Fire(Event::MatchEnd);
    } else if (to == 0 && from != 0) {
        DumpState("net session closed");
        ClearSimFixes();
        melange::events::Fire(Event::LobbyLeave);
    }
    if (to != S::InGame) {
        SetCameraHoldPatch(false);
        SetPanelSelectPatch(false);
    }
}

// One log line per change of the values that gate match progress, so a freeze shows which one got stuck.
void WatchProgressState() {
    static std::string last;
    uintptr_t ns = melange::wum::NetService();
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
    int n = melange::wum::PlayerCount(ns);
    for (int i = 0; i < n && i < 8; ++i) {
        uintptr_t p = melange::wum::PlayerAt(ns, i);
        snprintf(b, sizeof(b), " | p%d loaded=%u surr=%u", i, Read<uint8_t>(p + O::PlayerLoaded),
                 Read<uint8_t>(p + O::PlayerSurrenderNext));
        cur += b;
    }
    if (cur != last) {
        last = cur;
        LOG_INFO("[watch] %s", cur.c_str());
    }
}

void PollState() {
    uintptr_t s = melange::wum::CurrentState();
    if (s != g_lastState) {
        uintptr_t prev = g_lastState;
        g_lastState = s;
        OnStateChange(prev, s);
    }
}

std::vector<SafetyHookMid> g_hooks;

// Host migration: the turn owner's NetService::Update resends the whole turn (ReplayMessageStoreService 0x5408a0,
// "Resending all input messages from current turn") after the other machines discarded theirs, but leaves the same
// messages in the pending lists, and the next per-tick flush (0x5403a0) sends them again. The receiver's only dedupe
// is a strict `time < latest` per type (NetStored*Array::Handle, 0x68abfe and siblings), so inputs sent while the
// clock was frozen arrive twice with equal times and are applied twice: a desync. The resend sends the full-turn
// lists and then empties them, so check before it runs which pending lists are exactly the tail of their full-turn
// list, and after it drop those pending entries (if unchanged) through the game's own vector::erase.
struct StoreList {
    uint32_t pending;  // vector in the service; the full-turn list is at pending + kFullTurnDelta
    uintptr_t erase;   // __thiscall vector::erase(ret*, first, last), iterators {proxy, ptr}, ret 0x14
    uint32_t elem;
    uint32_t cmpEnd;   // SameEntry compares bytes [8, cmpEnd): elem, or 12 where +0xc is a per-list owned pointer
    const char* name;
};
constexpr StoreList kStoreLists[] = {
    {0x1e8, 0x53bda0, 12, 12, "msg"},     {0x200, 0x53be30, 16, 16, "int"},   {0x218, 0x53df00, 16, 12, "string"},
    {0x230, 0x53bec0, 20, 20, "two-int"}, {0x248, 0x53bf40, 16, 16, "float"}, {0x260, 0x53bfd0, 20, 20, "two-float"},
};
constexpr uint32_t kFullTurnDelta = 0x90;
constexpr uint32_t kVecProxy = 0x0, kVecFirst = 0xc, kVecLast = 0x10;
constexpr uintptr_t kResendAll = 0x5408a0;
// Lobby-entered handler (FUN_006269f0) copies the host's landscape_code into WXD.Level.Current via
// SetString(key, FUN_006266f0(table, code)). The lookup returns NULL for a code missing from this game's level list
// and SetString then strcmp()s it (crash c0000005 at 0x638a6c). Hook the PUSH EAX after the lookup call.
constexpr uintptr_t kLobbyLevelPush = 0x626c24;
constexpr uintptr_t kLobbyLevelDone = 0x626c32;  // past the SetString call and its ADD ESP,0x10
// Girder desync. Closing the weapon panel posts Input.ClosePanelPressed, a local message (the input translation
// service posts it; it is never a sim input). GirderKitLogicEntity::HandleMessage 0x55bac0 answers it with
// SetOverride(0,"UtilityGirder") and CameraManagerService::SetCamera("GirderCam") 0x51e4e0, on the closing machine
// only. SetCamera deactivates and re-activates the camera even when it is already current; OccludingCam::Activate
// 0x52da00 then re-derives the orbit yaw from the previous look vector (asin) and re-runs the occlusion search
// 0x530c00 over its 45-degree candidates, which can land on another pose once the girder has moved. The other
// machine keeps the old pose, so the logical cameras differ and the next turn-end validation fails (reasons 7,8;
// with a placed girder also 9,11,13). When the logical camera is another one (the kit outlives GameLogic.EndTurn's
// switch to Default, or a Path/Fall camera runs during its life), the switch itself diverges (reason 6). And the
// local-only message also reaches this handler on a spectator that right-clicks while the girder is selected. So
// during an online match the call is always skipped: before the close the logical cameras agree on every machine
// (otherwise validation has already failed), and not switching keeps this one identical to the peers that never saw
// the message. Offline the game re-aims as before. SetOverride(0,"UtilityGirder") still runs (it restores the girder
// control group; the key releases it forces go out as replicated sends, which the D1 trace below watches).
// The sim-driven activation (GirderKitLogicEntity 0x55ada0, run on every machine) is untouched.
constexpr uintptr_t kGirderCloseCase = 0x55bdd6;      // push 0x95d94c (the message id) ... call SetCamera
constexpr uintptr_t kGirderSetCamera = 0x55be02;      // call 0x51e4e0; ecx = CameraManagerService, [esp] = "GirderCam"
constexpr uintptr_t kGirderSetCameraDone = 0x55be07;  // push ebp; push esi; call the base HandleMessage
constexpr uintptr_t kGirderCamVtable = 0x855324;
constexpr uint32_t kCamIndexNone = 0xff, kCamMaxCount = 64;
// Turn-end clock desync. The turn ends on every machine from its own Lua: Timer.PostActivityTimedOut posts
// Net.DisableAllInput, whose handler (0x50e500) calls NetService::ProcessDisableNetInputMessage 0x709a3e. The active
// player builds and sends a GameStateValidationMsg stamped with its sim clock; a spectator pops the one it queued at
// receipt (ScheduleValidation 0x708fec) and requires the stamp to equal its own clock, and the per-frame time-sync
// check 0x709134 aborts (site D) as soon as the local clock passes a queued stamp. So every machine must reach its
// post-activity timeout at the same sim time. That timer waits for the ActiveObject registry, and FlyCam::Update
// 0x528240 registers "Hold the FlyCamera for a moment after the explosion" (about 1 s) when its tracked projectile is
// gone: a per-machine camera event (the engine tags the function "Danger of non-determinism"). Seen 2026-10-06 on
// the one spectator that switched to the blimp view (E, SpectateCam.BlimpViewPressed) 0.7 s after a Super Sheep
// explosion, and reproduced on LocalNet the same way: the hold delays NoActivity by its length, the machine ends its
// turn later than the others, and a spectator then overshoots the host's stamp by one tick (site D, Net.OutOfSynch)
// while an active player would stamp later than the spectators' own turn end (Net.Error.MissingTurnEndVal on every
// spectator). The fix skips the registration during an online match: the camera still pauses, only the turn no
// longer waits for it (verified: the same key press then ends the turn at the host's stamp).
constexpr uintptr_t kFlyCamHoldLea = 0x528444;      // lea eax,[esi+0x90]; esi = FlyCam (+0x8c hold ms)
constexpr uintptr_t kFlyCamHoldCall = 0x528457;     // call 0x4d3af0 (cdecl, its 4 args popped by the add esp,0x10 after)
constexpr uintptr_t kFlyCamHoldRelease = 0x5282a4;  // test eax,eax; eax = the hold's handle (NULL once released)
constexpr uintptr_t kScheduleValidation = 0x708fec; // __thiscall NetService::ScheduleValidation(msg)
constexpr uintptr_t kDisableNetInput = 0x709a3e;    // __thiscall NetService::ProcessDisableNetInputMessage()
constexpr uintptr_t kTimeSyncOvershoot = 0x709208;  // in 0x709134, "NETWORK TIMES NOT IN SYNC": eax = our clock, esi = msg, edi = NetService
constexpr uintptr_t kCameraManagerPtr = 0x95c370;
// Weapon selected under a frontend layer. Picking a weapon in the panel sends a replicated Weapon.DelayedPanelChanged
// (0x5427f0 at 0x600d0c, stamped ahead), and every machine's WXWeaponPanelLogicEntity::HandleMessage 0x603b70 then
// sets the worm's weapon. But when the frontend layer counter is non-zero (a popup or the pause menu is open), the
// handler only applies it when Game.Scope is 0, a NetService exists and the current player is remote; otherwise it
// skips it (0x603c40: cmp [0x97a614],ebx / jz 0x603c71 / cmp al,bl / jnz 0x603da9 / NetService / jz 0x603da9 /
// CurrentPlayerLocal / jnz 0x603da9). A machine with no layer open always applies it, so an active player who
// presses Esc between the pick and its stamped tick keeps the old weapon while the others switch (worm/weapon
// validation reasons). FixPanelSelectUnderMenu turns the jz into a jmp while an online match
// runs: 0x603c71 is IsMessage(edi, Weapon.DelayedPanelChanged), and every other id still goes on to 0x603da9, the
// skip's own target. Off by default until the desync is reproduced online.
constexpr uintptr_t kPanelSelectGate = 0x603c40;  // cmp [0x97a614],ebx; edi = message id (movzx), ebx = 0
constexpr uintptr_t kPanelSelectJz = 0x603c46;    // jz 0x603c71 (74 29); eb 29 while patched
constexpr uintptr_t kFrontendLayers = 0x97a614;   // frontend layer counter (DAT_0097a614)
// Trace: a replicated send made on a machine that is not the current player's. Every ReplayMessageStore sender
// inserts into this machine's dispatch list first and only then hands the record to its network queue (0x5416e0 and
// siblings), which drops it unless 0x539c00 passes, online meaning the current player is local. So a send from a
// spectator is applied on that spectator only, unless every machine makes the same send (the sim-driven ones do, and
// the receivers dedupe them by time), which is why this only logs. Hooked at the `mov ecx,[0x95cc20]` before each
// queue call, past the sender prologues that Wormsign's capture hooks and checks: esp = the MessageInfo just built
// (+4 id, +8 stamp), and the sender's own return address is at retOff.
struct SendSite {
    uintptr_t at;
    uint32_t retOff;
    const char* type;
};
constexpr SendSite kSendSites[] = {
    {0x5427dc, 0x0c, "msg"},   {0x542894, 0x10, "int"},       {0x54295c, 0x14, "two-int"},
    {0x542a17, 0x10, "float"}, {0x542adf, 0x14, "two-float"}, {0x5431d9, 0x1c, "string"},
};
// Trace: CameraManagerService::SetCamera 0x51e4e0 checks DeterminismChecker at entry (asserts when checking is on and
// the deterministic-phase flag 0x922ab4 is 0, then sets it). The flag is no filter: SetCamera and most camera code
// leave it at 1, so a local-only call usually finds it set too. Each distinct caller is logged once per match with the
// flag and whose turn it is; a caller that shows up on one machine's log and not the other's is a local-only switch.
constexpr uintptr_t kSetCamera = 0x51e4e0;
constexpr uintptr_t kDeterministicPhase = 0x922ab4;
// Per-match caps on the diagnostics above, so a chatty site cannot flood the log.
constexpr size_t kTraceKeysPerMatch = 32;
constexpr int kTraceLinesPerMatch = 16;
SafetyHookInline g_resendHook;

// Entry layout: +0 vtable, +4 u16 id (+6 is uninitialised padding), +8 time, +0xc payload (the string type keeps an
// XString* there, which each list may own separately). Compare everything except the padding and that pointer.
bool SameEntry(uintptr_t a, uintptr_t b, const StoreList& l) {
    uint8_t x[32], y[32];
    const uint32_t n = l.elem;
    if (n > sizeof x || !melange::mem::SafeRead(a, x, n) || !melange::mem::SafeRead(b, y, n)) return false;
    return !memcmp(x, y, 6) && !memcmp(x + 8, y + 8, l.cmpEnd - 8);
}

std::string Hex(uintptr_t a, uint32_t n) {
    uint8_t b[32] = {};
    melange::mem::SafeRead(a, b, n < sizeof b ? n : sizeof b);
    std::string s;
    char h[4];
    for (uint32_t i = 0; i < n && i < sizeof b; ++i) snprintf(h, sizeof h, "%02x", b[i]), s += h;
    return s;
}

struct Range {
    uint32_t first = 0, last = 0;
};
Range Vec(uintptr_t v) { return {Read<uint32_t>(v + kVecFirst), Read<uint32_t>(v + kVecLast)}; }

// Before the resend: the pending entries that the resend is about to carry, i.e. a pending list that is exactly the
// tail of its full-turn list, entry for entry (SameEntry). Their bytes go to `snap`; returns how many, or -1 (logged).
int ResentPending(uintptr_t svc, const StoreList& l, std::vector<uint8_t>* snap) {
    const uintptr_t pend = svc + l.pending;
    const Range p = Vec(pend), f = Vec(pend + kFullTurnDelta);
    if (p.last < p.first || f.last < f.first || (p.last - p.first) % l.elem || (f.last - f.first) % l.elem) {
        LOG_WARN("[fix] migration resend: %s lists malformed (pending %08x..%08x, full turn %08x..%08x)", l.name,
                 p.first, p.last, f.first, f.last);
        return -1;
    }
    const uint32_t np = (p.last - p.first) / l.elem, nf = (f.last - f.first) / l.elem;
    if (!np) return 0;
    if (np > nf) {
        LOG_WARN("[fix] migration resend: %s has %u pending but only %u in the full turn", l.name, np, nf);
        return -1;
    }
    for (uint32_t i = 0; i < np; ++i) {
        const uintptr_t a = p.first + i * l.elem, b = f.first + (nf - np + i) * l.elem;
        if (!SameEntry(a, b, l)) {
            LOG_WARN("[fix] migration resend: %s pending[%u] %s != full turn[%u] %s (%u pending, %u full)", l.name, i,
                     Hex(a, l.elem).c_str(), nf - np + i, Hex(b, l.elem).c_str(), np, nf);
            return -1;
        }
    }
    snap->resize(np * l.elem);
    if (!melange::mem::SafeRead(p.first, snap->data(), snap->size())) return -1;
    return static_cast<int>(np);
}

// After the resend: drop the pending entries it carried, if the list still holds exactly those. Returns how many, or -1.
int DropPending(uintptr_t svc, const StoreList& l, const std::vector<uint8_t>& snap) {
    const uintptr_t pend = svc + l.pending;
    const Range p = Vec(pend);
    std::vector<uint8_t> now(snap.size());
    if (p.last - p.first != snap.size() || !melange::mem::SafeRead(p.first, now.data(), now.size()) || now != snap) {
        LOG_WARN("[fix] migration resend: %s pending list changed during the resend; left alone", l.name);
        return -1;
    }
    const uint32_t proxy = Read<uint32_t>(pend + kVecProxy);
    if (!proxy) return -1;  // the checked iterators would trap
    uint32_t ret[2] = {};
    reinterpret_cast<void*(__thiscall*)(uintptr_t, uint32_t*, uint32_t, uint32_t, uint32_t, uint32_t)>(l.erase)(
        pend, ret, proxy, p.first, proxy, p.last);
    return static_cast<int>(snap.size() / l.elem);
}

void __fastcall OnResendAll(uintptr_t svc, void* /*edx*/) {
    constexpr size_t kLists = sizeof kStoreLists / sizeof kStoreLists[0];
    std::vector<uint8_t> snap[kLists];
    int plan[kLists];
    for (size_t i = 0; i < kLists; ++i) plan[i] = ResentPending(svc, kStoreLists[i], &snap[i]);
    g_resendHook.thiscall<void>(svc);
    std::string done;
    for (size_t i = 0; i < kLists; ++i) {
        const int n = plan[i] > 0 ? DropPending(svc, kStoreLists[i], snap[i]) : plan[i];
        if (n == 0) continue;
        char b[48];
        snprintf(b, sizeof b, " %s=%d", kStoreLists[i].name, n);
        done += b;
    }
    if (done.empty())
        LOG_INFO("[fix] migration resend: no pending inputs to drop");
    else
        LOG_WARN("[fix] migration resend: dropped pending inputs already resent (-1 = left alone):%s", done.c_str());
}

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

// eax = looked-up level (NULL = unknown). [esp] = &table, [esp+4] = code: the lookup's two cdecl args, still pushed.
void OnLobbyLevel(safetyhook::Context& c) {
    if (c.eax) return;
    LOG_WARN("[fix] lobby level code %u not in this game's level list; keeping current level",
            Read<uint32_t>(c.esp + 4));
    // Resume at kLobbyLevelDone with both args popped (the skipped ADD ESP,0x10 popped these plus SetString's two).
    // safetyhook's x86 stub ignores ctx.esp on restore: it does `pop esp` (trampoline_esp) then `ret`. So park the
    // resume address in the code slot being discarded and return through it, leaving esp = original esp + 8.
    *reinterpret_cast<uintptr_t*>(c.esp + 4) = kLobbyLevelDone;
    c.trampoline_esp = c.esp + 4;
}

std::string CStr(uintptr_t p, size_t max = 64) {
    std::string s;
    char ch = 0;
    for (size_t i = 0; p && i < max && melange::mem::SafeRead(p + i, &ch, 1) && ch; ++i) s += ch;
    return s;
}

// The manager's logical camera (m_uLogicalCamera +0x28c into the Camera* vector +0x2a0..+0x2a4), or 0.
uintptr_t LogicalCamera(uintptr_t mgr, uint32_t* index, uint32_t* count) {
    *index = mgr ? Read<uint32_t>(mgr + 0x28c) : kCamIndexNone;
    const uint32_t first = mgr ? Read<uint32_t>(mgr + 0x2a0) : 0, last = mgr ? Read<uint32_t>(mgr + 0x2a4) : 0;
    *count = last >= first ? (last - first) / 4 : 0;
    return *index < *count && *count <= kCamMaxCount ? Read<uint32_t>(first + *index * 4) : 0;
}
// A camera's name: +0x40 holds the XString's char* (what 0x4b58f0 hands to the "Path" compare).
std::string CameraName(uintptr_t cam) { return cam ? CStr(Read<uint32_t>(cam + 0x40)) : "none"; }

// Per-match trace budgets, reset when a match enters InGame (ResetMatchTrace).
struct MatchTrace {
    int girderClose = 0;
    int panelSelect = 0;
    int layerChanges = 0;
    std::vector<uint64_t> sends, setCameras;  // keys already logged
    bool sendsCapped = false, setCamerasCapped = false;
    uint32_t lastLayers = 0;
} g_matchTrace;
bool g_traceLayers = false;

std::unordered_map<uintptr_t, uint32_t> g_drumFirstUpdate;  // oil drum -> time of its first Update this match
void ResetMatchTrace() {
    g_matchTrace = MatchTrace{};
    g_drumFirstUpdate.clear();
}

// True the first time `key` is seen this match, until `seen` holds kTraceKeysPerMatch keys (then says so once).
bool FirstSeen(std::vector<uint64_t>& seen, bool& capped, uint64_t key, const char* what) {
    for (uint64_t k : seen)
        if (k == key) return false;
    if (seen.size() >= kTraceKeysPerMatch) {
        if (!capped) LOG_INFO("[net] %s: %u distinct sites logged this match; not logging more", what,
                              static_cast<unsigned>(kTraceKeysPerMatch));
        capped = true;
        return false;
    }
    seen.push_back(key);
    return true;
}

// At the CALL SetCamera("GirderCam") of the panel-close case: ecx = CameraManagerService (just loaded from
// 0x95c370), [esp] = the pushed name (the thiscall's one stack arg, popped by its RET 4), edi = the message id.
void OnGirderPanelClose(safetyhook::Context& c) {
    if (!InNetMatch()) return;  // offline the game re-aims as it always did
    uint32_t index, count;
    const uintptr_t cam = LogicalCamera(c.ecx, &index, &count);
    const bool girderCam = cam && Read<uint32_t>(cam) == kGirderCamVtable;
    if (g_matchTrace.girderClose < 4) {
        ++g_matchTrace.girderClose;
        const char* msg = melange::bus::NameOf(static_cast<melange::bus::MsgId>(c.edi));
        const bool active = ActivePlayerHere();
        const std::string name = CameraName(cam);
        LOG_INFO("[fix] girder panel close (%s) on the %s at t=%u: logical camera %u/%u is %s (%s) -> skipping the "
                 "switch to GirderCam", msg ? msg : "?", active ? "active player" : "spectator", SimClock(), index,
                 count, girderCam ? "GirderCam" : "another camera", name.c_str());
        melange::jlog::Rec("net", melange::jlog::Level::Info, "girder panel close").Uint("t", SimClock())
            .Bool("active", active).Bool("girderCam", girderCam).Str("camera", name).Emit();
    }
    // Skip the call: resume right after it with the argument popped, as the callee's RET 4 would have left it
    // (same trick as OnLobbyLevel: the stub restores esp from trampoline_esp and returns through that slot).
    *reinterpret_cast<uintptr_t*>(c.esp) = kGirderSetCameraDone;
    c.trampoline_esp = c.esp;
}

// At 0x603c40, before the frontend-layer test: edi = the message id, al = Game.Scope (0x4d3ed0, just called).
// Reports the case the patch exists for (the active player handling Weapon.DelayedPanelChanged under a frontend
// layer), patched or not.
void OnPanelSelectGate(safetyhook::Context& c) {
    static melange::bus::MsgId id = melange::bus::kInvalidId;
    if (!InNetMatch()) return;
    if (id == melange::bus::kInvalidId) id = melange::bus::IdOf("Weapon.DelayedPanelChanged");
    if (id == melange::bus::kInvalidId || static_cast<uint16_t>(c.edi) != id) return;
    const uint32_t layers = Read<uint32_t>(kFrontendLayers);
    if (!layers || !ActivePlayerHere() || g_matchTrace.panelSelect >= kTraceLinesPerMatch) return;
    ++g_matchTrace.panelSelect;
    const bool patched = g_panelSelectPatched;
    if (patched)
        LOG_INFO("[fix] Weapon.DelayedPanelChanged at t=%u with %u frontend layer(s) open on the active player: "
                 "applied (FixPanelSelectUnderMenu)", SimClock(), layers);
    else
        LOG_WARN("[net] Weapon.DelayedPanelChanged at t=%u with %u frontend layer(s) open on the active player: "
                 "skipped by the game, so this machine keeps its old weapon while the others switch "
                 "(FixPanelSelectUnderMenu=0)", SimClock(), layers);
    melange::jlog::Rec("net", patched ? melange::jlog::Level::Info : melange::jlog::Level::Warn, "panel select under menu")
        .Uint("t", SimClock()).Uint("layers", layers).Uint("scope", c.eax & 0xff).Bool("patched", patched).Emit();
}

// D1 at one of kSendSites: esp = the MessageInfo about to be queued for the network.
void SpectatorSend(const SendSite& s, const safetyhook::Context& c) {
    if (!InNetMatch() || ActivePlayerHere()) return;
    const uint16_t id = static_cast<uint16_t>(Read<uint32_t>(c.esp + 4));  // +6 is padding
    const uint32_t stamp = Read<uint32_t>(c.esp + 8), ret = Read<uint32_t>(c.esp + s.retOff);
    if (!FirstSeen(g_matchTrace.sends, g_matchTrace.sendsCapped, (static_cast<uint64_t>(id) << 32) | ret,
                   "replicated sends off-turn"))
        return;
    const char* msg = melange::bus::NameOf(static_cast<melange::bus::MsgId>(id));
    LOG_INFO("[net] replicated %s send of %s (stamp %u, our t=%u) from %s while the current player is not local: "
             "applied here only unless every machine sends it", s.type, msg ? msg : "?", stamp, SimClock(),
             melange::game::DescribeAddress(ret).c_str());
    melange::jlog::Rec("net", melange::jlog::Level::Info, "off-turn send").Str("type", s.type).Str("msg", msg ? msg : "?")
        .Uint("stamp", stamp).Uint("t", SimClock()).Hex("caller", ret).Emit();
}
template <int N>
void OnSpectatorSend(safetyhook::Context& c) {
    SpectatorSend(kSendSites[N], c);
}

// mov ecx,[0x95cc20] / lea r,[esp] / push r / call the network queue (0x5416e0, 0x541850, 0x541980, 0x541b20,
// 0x541c50, 0x543000).
bool SendSitesOriginal() {
    namespace M = melange::mem;
    return M::Expect(kSendSites[0].at, {0x8b, 0x0d, 0x20, 0xcc, 0x95, 0x00, 0x8d, 0x14, 0x24, 0x52, 0xe8, 0xf5, 0xee, 0xff, 0xff}) &&
           M::Expect(kSendSites[1].at, {0x8b, 0x0d, 0x20, 0xcc, 0x95, 0x00, 0x8d, 0x04, 0x24, 0x50, 0xe8, 0xad, 0xef, 0xff, 0xff}) &&
           M::Expect(kSendSites[2].at, {0x8b, 0x0d, 0x20, 0xcc, 0x95, 0x00, 0x8d, 0x04, 0x24, 0x50, 0xe8, 0x15, 0xf0, 0xff, 0xff}) &&
           M::Expect(kSendSites[3].at, {0x8b, 0x0d, 0x20, 0xcc, 0x95, 0x00, 0x8d, 0x14, 0x24, 0x52, 0xe8, 0xfa, 0xf0, 0xff, 0xff}) &&
           M::Expect(kSendSites[4].at, {0x8b, 0x0d, 0x20, 0xcc, 0x95, 0x00, 0x8d, 0x14, 0x24, 0x52, 0xe8, 0x62, 0xf1, 0xff, 0xff}) &&
           M::Expect(kSendSites[5].at, {0x8b, 0x0d, 0x20, 0xcc, 0x95, 0x00, 0x8d, 0x14, 0x24, 0x52, 0xe8, 0x18, 0xfe, 0xff, 0xff});
}

// D2 at SetCamera entry: ecx = CameraManagerService, [esp+4] = the requested camera name.
void OnSetCamera(safetyhook::Context& c) {
    if (!InNetMatch()) return;
    const uint32_t ret = RetAddr(c);
    if (!FirstSeen(g_matchTrace.setCameras, g_matchTrace.setCamerasCapped, ret, "SetCamera callers")) return;
    const bool deterministic = Read<uint8_t>(kDeterministicPhase, 1) != 0;
    const bool active = ActivePlayerHere();
    uint32_t index, count;
    const std::string want = CStr(Arg(c, 0)), cur = CameraName(LogicalCamera(c.ecx, &index, &count));
    LOG_INFO("[net] SetCamera(\"%s\") at t=%u (logical camera %s, deterministic phase %u, %s) from %s", want.c_str(),
             SimClock(), cur.c_str(), deterministic ? 1u : 0u, active ? "active player" : "spectator",
             melange::game::DescribeAddress(ret).c_str());
    melange::jlog::Rec("net", deterministic ? melange::jlog::Level::Info : melange::jlog::Level::Warn, "SetCamera caller")
        .Str("name", want).Str("current", cur).Uint("t", SimClock()).Hex("caller", ret).Bool("deterministic", deterministic)
        .Bool("active", active).Emit();
}

// Under Trace: each change of the frontend layer counter during a match (does the pause menu or the chat console
// raise it online at all? FixPanelSelectUnderMenu only matters if it does).
void WatchFrontendLayers() {
    if (!g_traceLayers || !InNetMatch()) return;
    const uint32_t v = Read<uint32_t>(kFrontendLayers);
    if (v == g_matchTrace.lastLayers) return;
    const uint32_t was = g_matchTrace.lastLayers;
    g_matchTrace.lastLayers = v;
    if (g_matchTrace.layerChanges >= kTraceLinesPerMatch) return;
    ++g_matchTrace.layerChanges;
    const bool active = ActivePlayerHere();
    LOG_INFO("[net] frontend layers %u -> %u at t=%u (%s)", was, v, SimClock(), active ? "active player" : "spectator");
    melange::jlog::Rec("net", melange::jlog::Level::Info, "frontend layers").Uint("from", was).Uint("to", v)
        .Uint("t", SimClock()).Bool("active", active).Emit();
}

// The queued validation messages: how many and the head's stamp.
std::string ValidationFifo(uintptr_t ns, uint32_t* headT) {
    const uint32_t n = Read<uint32_t>(ns + O::ValidationFifoCount);
    *headT = 0;
    if (!n) return "queue empty";
    const uintptr_t head = Read<uint32_t>(Read<uint32_t>(ns + O::ValidationFifo));
    *headT = Read<uint32_t>(head + O::ValidationTime);
    char b[64];
    snprintf(b, sizeof b, "queued %u, head t=%u sov %u", n, *headT, Read<uint8_t>(head + O::ValidationSov));
    return b;
}

// ecx = NetService, [esp+4] = the received GameStateValidationMsg (before the duplicate check and the push).
void OnScheduleValidation(safetyhook::Context& c) {
    const uintptr_t msg = Arg(c, 0);
    const uint32_t t = SimClock(), mt = Read<uint32_t>(msg + O::ValidationTime), sov = Read<uint8_t>(msg + O::ValidationSov);
    uint32_t headT;
    const std::string q = ValidationFifo(c.ecx, &headT);
    LOG_INFO("[net] validation received for t=%u (sov %u) at our t=%u%s (%s)", mt, sov, t, t > mt ? " LATE" : "", q.c_str());
    melange::jlog::Rec("net", t > mt ? melange::jlog::Level::Warn : melange::jlog::Level::Info, "validation received")
        .Uint("t", t).Uint("msgT", mt).Uint("sov", sov).Uint("queued", Read<uint32_t>(c.ecx + O::ValidationFifoCount)).Emit();
}

// ecx = NetService: this machine's own turn end (its Lua posted Net.DisableAllInput).
void OnDisableNetInput(safetyhook::Context& c) {
    const uint32_t t = SimClock();
    uint32_t headT;
    const std::string q = ValidationFifo(c.ecx, &headT);
    // The active player builds and sends the validation here; everyone else pops the one it received.
    const uintptr_t cur = melange::wum::CurrentPlayer(c.ecx);
    const bool active = cur && Read<uint8_t>(cur + O::PlayerIsLocal);
    const bool bad = !active && (headT ? headT != t : !Read<uint8_t>(c.ecx + O::CurrentSurrendered));
    LOG_INFO("[net] turn end at t=%u (%s; %s)%s", t, active ? "active player" : "spectator", q.c_str(),
             !bad ? "" : headT ? " MISMATCH" : " NO VALIDATION YET");
    melange::jlog::Rec("net", bad ? melange::jlog::Level::Warn : melange::jlog::Level::Info, "turn end")
        .Uint("t", t).Bool("active", active).Uint("headT", headT).Uint("queued", Read<uint32_t>(c.ecx + O::ValidationFifoCount)).Emit();
}

// The time-sync check found our clock past the queued stamp: the engine logs it to a null stream and aborts.
void OnTimeSyncOvershoot(safetyhook::Context& c) {
    const uint32_t mt = Read<uint32_t>(c.esi + O::ValidationTime), sov = Read<uint8_t>(c.esi + O::ValidationSov);
    LOG_ERROR("[net] time-sync check: our t=%u is past the queued validation for t=%u (sov %u, queued %u) -> abort",
              c.eax, mt, sov, Read<uint32_t>(c.edi + O::ValidationFifoCount));
    melange::jlog::Rec("net", melange::jlog::Level::Error, "time sync overshoot")
        .Uint("t", c.eax).Uint("msgT", mt).Uint("sov", sov).Uint("queued", Read<uint32_t>(c.edi + O::ValidationFifoCount)).Emit();
}

// esi = the FlyCam about to register its post-explosion hold. Says whether it is even the manager's logical camera.
void OnFlyCamHold(safetyhook::Context& c) {
    uint32_t index, count;
    const uintptr_t logical = LogicalCamera(Read<uint32_t>(kCameraManagerPtr), &index, &count);
    const bool skipped = g_cameraHoldPatched;
    LOG_INFO("[net] FlyCam post-explosion hold at t=%u for %u ms (%s logical camera)%s", SimClock(),
             Read<uint32_t>(c.esi + 0x8c), logical == c.esi ? "the" : "not the",
             skipped ? "; not registered as activity (FixCameraHold)" : "");
    melange::jlog::Rec("net", melange::jlog::Level::Info, "camera hold").Uint("t", SimClock())
        .Uint("ms", Read<uint32_t>(c.esi + 0x8c)).Bool("logical", logical == c.esi).Bool("net", InNetMatch())
        .Bool("skipped", skipped).Emit();
}

// Runs every frame once the hold time is up; the handle is non-NULL only on the frame that releases it.
void OnFlyCamHoldRelease(safetyhook::Context& c) {
    if (c.eax) LOG_INFO("[net] FlyCam hold released at t=%u", SimClock());
}

// Oil drum fuse (OilDrumLogicEntity.cpp). An Explosion within LandDamageRadius + 9 of the drum (handler 0x5d1e80) or
// a Damage.Impulse with damage > 0 (0x5d1fa0) marks it "about to explode" and stamps it (+0x84) with the sim clock.
// Its task, Update 0x5d2200 (vtable slot 6, __stdcall(this, now)), explodes it on the first run with now > stamp + 50
// and always returns 0x14, so the scheduler (0x68d6f1) runs it again 20 ms after the task's own time.
//
// The bug: the sim clock (TaskManager+0x38) is only grid-aligned (a multiple of the 20 ms tick) while the scheduler is
// dispatching a task -- it writes the task's own time there. Between frames, when no task is pending, TaskManager::Update
// (0x68d4d4) writes the raw frame time (now - timebase) there instead, which is not on the grid and differs from one
// machine to the next by each machine's frame timing. A Damage.Impulse is delivered off the physics step, not from a
// scheduled task, so a drum knocked by a nearby blast reads that off-grid, per-machine clock as its stamp. Its fuse
// (first update past stamp + 50) then crosses a tick boundary at a different tick on each machine, so the drum explodes
// a tick apart; every RNG draw of that explosion and its poison cloud lands in a different tick, and the turn-end check
// fails with "Random's dont match" (2026-10-24, the second drum of a Carrion Drop, 57 LCG draws apart).
//
// Two per-machine inputs feed the fuse, and the fix removes both during an online match: (1) FixDrumFuse snaps the stamp
// down to the tick grid as it is written, so the fuse always measures from a grid time; (2) it returns the mine's value
// (20 - now % 20, see 0x5cbd30) from the drum's Update when the task is off the grid, so the task itself runs on the
// grid from its next tick on, as a mine's does. With both, the drum explodes exactly 3 ticks (60 ms) after its grid
// stamp on every machine. Offline the engine is left alone.
constexpr uintptr_t kDrumUpdateRet = 0x5d231d;      // mov eax,0x14 / mov fs:[0],ecx
constexpr uintptr_t kDrumUpdateRetHook = 0x5d2322;  // after the mov eax: [esp+0x14] = drum, [esp+0x18] = now
constexpr uintptr_t kDrumFlagExplosion = 0x5d1f2a;  // mov [esi+0x84],edx: esi = drum, edx = sim clock (Explosion)
constexpr uintptr_t kDrumFlagImpulse = 0x5d2007;    // mov [esi+0x84],eax: esi = drum, eax = sim clock (Damage.Impulse)
constexpr uintptr_t kDrumExplodeCheck = 0x5d227b;   // cmp byte [esi+0x68],0 once now > stamp + 50; explodes when 0

uint32_t TickOf(uint32_t t) { return (t + 19) / 20; }

// The agreement (see g_drumFuseAgreed): read once per match, as the match leaves the lobby. A lobby member without the
// fix, on an older Melange or without Melange, or a lobby whose owner has not written the agreed list, turns it off on
// every machine for that match, and a lobby data change after this point changes nothing until the next match.
void DecideSimFixes() {
    namespace simfix = melange::handshake::simfix;
    g_simFixesDecided = true;
    g_drumFuseAgreed = g_fix.alignDrumFuse && simfix::Agreed(simfix::kDrumFuse);
    simfix::SetActive(g_drumFuseAgreed ? std::vector<std::string>{simfix::kDrumFuse} : std::vector<std::string>{});
    std::string why;
    if (g_drumFuseAgreed)
        LOG_INFO("[fix] FixDrumFuse: on for match %d (every player in the lobby runs it)", g_matchNumber);
    else if (!g_fix.alignDrumFuse)
        LOG_INFO("[fix] FixDrumFuse: off for match %d (off in Melange.ini)", g_matchNumber);
    else
        LOG_WARN("[fix] FixDrumFuse: off for match %d on every machine, since not every player runs it: %s", g_matchNumber,
                 (why = simfix::Why(simfix::kDrumFuse)).c_str());
    melange::jlog::Rec("net", g_drumFuseAgreed || !g_fix.alignDrumFuse ? melange::jlog::Level::Info : melange::jlog::Level::Warn,
                       "sim fixes decided").Int("match", g_matchNumber).Bool("drumFuse", g_drumFuseAgreed).Str("why", why).Emit();
}
void ClearSimFixes() {
    g_drumFuseAgreed = g_simFixesDecided = false;
    melange::handshake::simfix::SetActive({});
}
bool DrumFuseFixOn() { return g_drumFuseAgreed && InNetMatch(); }

// Snaps the stamp register down to the tick grid (during a net match that agreed to the fix) just before the engine
// stores it into +0x84, and logs it. `reg` is edx on the Explosion path and eax on the Damage.Impulse path; neither is
// reused after the store.
void OnDrumFlag(uintptr_t drum, uint32_t& reg, const char* how) {
    const uint32_t raw = reg;
    const bool fix = DrumFuseFixOn() && raw % 20 != 0;
    if (fix) reg = (raw / 20) * 20;
    LOG_INFO("[net] oil drum %08x about to explode: %s at t=%u (tick %u)%s -> explodes at t=%u (tick %u)",
             static_cast<unsigned>(drum), how, raw, TickOf(raw),
             fix ? "; stamp snapped to the 20 ms grid (FixDrumFuse)" : "", reg + 60, TickOf(reg + 60));
    melange::jlog::Rec("net", melange::jlog::Level::Info, "oil drum flagged").Hex("drum", drum).Str("how", how)
        .Uint("rawT", raw).Uint("stamp", reg).Uint("explodeT", reg + 60).Bool("aligned", fix).Emit();
}
void OnDrumFlagExplosion(safetyhook::Context& c) { OnDrumFlag(c.esi, c.edx, "Explosion"); }
void OnDrumFlagImpulse(safetyhook::Context& c) { OnDrumFlag(c.esi, c.eax, "Damage.Impulse"); }

void OnDrumExplodeCheck(safetyhook::Context& c) {
    const uintptr_t drum = c.esi;
    if (Read<uint8_t>(drum + 0x68)) return;  // exploded already: this run or a later one deletes it
    const uint32_t now = Read<uint32_t>(c.esp + 0x1c), stamp = Read<uint32_t>(drum + 0x84);
    LOG_INFO("[net] oil drum %08x explodes at t=%u (tick %u), %u ms after its stamp t=%u (tick %u); sim clock %u",
             static_cast<unsigned>(drum), now, TickOf(now), now - stamp, stamp, TickOf(stamp), SimClock());
    melange::jlog::Rec("net", melange::jlog::Level::Info, "oil drum explodes").Hex("drum", drum).Uint("t", now)
        .Uint("tick", TickOf(now)).Uint("stamp", stamp).Uint("clock", SimClock()).Emit();
}

// Every drum Update, about to return 0x14 (eax). Logs each drum's first run of the match (its task phase) and, with
// FixDrumFuse agreed for this online match, returns 20 - now % 20 instead when the task is off the grid.
void OnDrumUpdateRet(safetyhook::Context& c) {
    const uintptr_t drum = Read<uint32_t>(c.esp + 0x14);
    const uint32_t now = Read<uint32_t>(c.esp + 0x18);
    const bool net = InNetMatch();
    const bool fix = DrumFuseFixOn() && now % 20 != 0;
    if (fix) c.eax = 20 - now % 20;
    auto [it, fresh] = g_drumFirstUpdate.try_emplace(drum, now);
    if (fresh) {
        LOG_INFO("[net] oil drum %08x: first update at t=%u (tick %u), task phase %u%s", static_cast<unsigned>(drum), now,
                 TickOf(now), now % 20, fix ? "; re-aligned to the 20 ms grid (FixDrumFuse)" : "");
        melange::jlog::Rec("net", melange::jlog::Level::Info, "oil drum task").Hex("drum", drum).Uint("t", now)
            .Uint("phase", now % 20).Bool("net", net).Bool("aligned", fix).Emit();
    } else if (fix) {
        LOG_INFO("[fix] FixDrumFuse: oil drum %08x task at t=%u (phase %u) re-aligned to the 20 ms grid",
                 static_cast<unsigned>(drum), now, now % 20);
    }
    if (Read<uint8_t>(drum + 0x80) && !Read<uint8_t>(drum + 0x68))
        LOG_INFO("[net] oil drum %08x update at t=%u (tick %u), stamp t=%u, not yet past stamp + 50",
                 static_cast<unsigned>(drum), now, TickOf(now), Read<uint32_t>(drum + 0x84));
}

// FixCameraHold: while an online match runs, the register call is five NOPs (its pushes are still popped by the
// add esp,0x10 after it, and the release path only checks the handle at +0x90 for NULL). Restored outside matches so
// local games keep the engine's camera pause as activity.
void SetCameraHoldPatch(bool on) {
    static const uint8_t call[5] = {0xe8, 0x94, 0xb6, 0xfa, 0xff};  // call 0x4d3af0
    static const uint8_t nops[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
    if (!g_fix.skipCameraHold || on == g_cameraHoldPatched) return;
    const bool asExpected = on ? melange::mem::Expect(kFlyCamHoldCall, {0xe8, 0x94, 0xb6, 0xfa, 0xff})
                               : melange::mem::Expect(kFlyCamHoldCall, {0x90, 0x90, 0x90, 0x90, 0x90});
    if (!asExpected) {
        LOG_WARN("[fix] FixCameraHold: unexpected code at %08x; not %s", static_cast<unsigned>(kFlyCamHoldCall),
                 on ? "installed" : "restored");
        return;
    }
    melange::mem::Write(kFlyCamHoldCall, on ? nops : call, 5);
    g_cameraHoldPatched = on;
    LOG_INFO("[fix] FixCameraHold: FlyCam hold %s", on ? "no longer registers as activity (online match)" : "restored");
}

// FixPanelSelectUnderMenu: while an online match runs, the frontend-layer jz at 0x603c46 is a jmp (see
// kPanelSelectGate). Only the jz is checked here: the trace hook may own the instruction before it.
void SetPanelSelectPatch(bool on) {
    static const uint8_t jz = 0x74, jmp = 0xeb;
    if (!g_fix.panelSelectUnderMenu || on == g_panelSelectPatched) return;
    const bool asExpected = on ? melange::mem::Expect(kPanelSelectJz, {0x74, 0x29, 0x3a, 0xc3})
                               : melange::mem::Expect(kPanelSelectJz, {0xeb, 0x29, 0x3a, 0xc3});
    if (!asExpected) {
        LOG_WARN("[fix] FixPanelSelectUnderMenu: unexpected code at %08x; not %s", static_cast<unsigned>(kPanelSelectJz),
                 on ? "installed" : "restored");
        return;
    }
    melange::mem::Write(kPanelSelectJz, on ? &jmp : &jz, 1);
    g_panelSelectPatched = on;
    LOG_INFO("[fix] FixPanelSelectUnderMenu: %s",
             on ? "a weapon picked under a frontend layer is applied (online match)" : "restored");
}

void OnAbortGame(safetyhook::Context& c) {
    uint32_t hr = Arg(c, 0), ret = RetAddr(c);
    LOG_ERROR("[net] ===== AbortGame(%08x %s) from %s  [site %s] match %d", hr, AbortCode(hr),
             melange::game::DescribeAddress(ret).c_str(), AbortSite(ret), g_matchNumber);
    DumpState("at abort");
}

void OnSurrender(safetyhook::Context& c) {
    LOG_INFO("[net] SurrenderPlayer(player %08x) from %s", Arg(c, 0), melange::game::DescribeAddress(RetAddr(c)).c_str());
}

void OnTurnStarted(safetyhook::Context& c) {
    // An event handler: the arg is the event message, so look the player up ourselves.
    uintptr_t p = melange::wum::CurrentPlayer(c.ecx);
    LOG_INFO("[net] turn started: player %08x %s surrenderNext=%u", static_cast<unsigned>(p),
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
        LOG_INFO("[net] CheckViability(session %08x, nOffset=%d) sessionPlayers=%d", static_cast<unsigned>(c.ecx), nOffset,
                count);
    }
}

void OnConnCtor(safetyhook::Context& c) {
    LOG_INFO("[net] XSteamConnection created %08x from %s", static_cast<unsigned>(c.ecx),
            melange::game::DescribeAddress(RetAddr(c)).c_str());
}

void OnConnDtor(safetyhook::Context& c) {
    uintptr_t t = c.ecx;
    LOG_INFO("[net] XSteamConnection destroyed %08x sendSeq=%u recvSeq=%u state=%u", static_cast<unsigned>(t),
            Read<uint32_t>(t + O::ConnSendSeq), Read<uint32_t>(t + O::ConnRecvSeq), Read<uint32_t>(t + O::ConnState));
}

void OnNewSender(safetyhook::Context&) { LOG_WARN("[net] P2P packet from unknown SteamID -> new XSteamConnection"); }

bool Mid(uintptr_t addr, safetyhook::MidHookFn fn, const char* what) {
    auto h = safetyhook::create_mid(addr, fn);
    if (!h) {
        LOG_ERROR("[net] failed to hook %s at %08x", what, static_cast<unsigned>(addr));
        return false;
    }
    g_hooks.push_back(std::move(h));
    return true;
}

class NetSession final : public melange::Module {
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

        bool ok = Mid(A::AbortGame, &OnAbortGame, "AbortGame");
        if (trace) {
            ok &= Mid(A::SurrenderPlayer, &OnSurrender, "SurrenderPlayer");
            ok &= Mid(A::TurnStarted, &OnTurnStarted, "TurnStarted");
            ok &= Mid(A::CheckViability, &OnCheckViability, "CheckViability");
            // The turn-end validation protocol and the camera hold that can shift a machine's turn end (see
            // kFlyCamHoldLea): push ebx/esi/edi/push "ScheduleValidation" | push ebp/mov ebp,esp/sub esp,0xc/push
            // ebx/esi | call 0x70ce5e | lea eax,[esi+0x90] | test eax,eax/jz +8/mov edx,[eax]/push eax
            if (melange::mem::Expect(kScheduleValidation, {0x53, 0x56, 0x57, 0x68, 0x94, 0xc1, 0x89, 0x00}) &&
                melange::mem::Expect(kDisableNetInput, {0x55, 0x8b, 0xec, 0x83, 0xec, 0x0c, 0x53, 0x56}) &&
                melange::mem::Expect(kTimeSyncOvershoot, {0xe8, 0x51, 0x3c, 0x00, 0x00}) &&
                melange::mem::Expect(kFlyCamHoldLea, {0x8d, 0x86, 0x90, 0x00, 0x00, 0x00, 0x50, 0x6a, 0x61}) &&
                melange::mem::Expect(kFlyCamHoldRelease, {0x85, 0xc0, 0x74, 0x08, 0x8b, 0x10, 0x50})) {
                ok &= Mid(kScheduleValidation, &OnScheduleValidation, "ScheduleValidation");
                ok &= Mid(kDisableNetInput, &OnDisableNetInput, "ProcessDisableNetInputMessage");
                ok &= Mid(kTimeSyncOvershoot, &OnTimeSyncOvershoot, "time-sync overshoot");
                ok &= Mid(kFlyCamHoldLea, &OnFlyCamHold, "FlyCam hold");
                ok &= Mid(kFlyCamHoldRelease, &OnFlyCamHoldRelease, "FlyCam hold release");
            } else {
                LOG_WARN("[net] turn-end trace: unexpected code at one of %08x %08x %08x %08x %08x; not installed",
                         static_cast<unsigned>(kScheduleValidation), static_cast<unsigned>(kDisableNetInput),
                         static_cast<unsigned>(kTimeSyncOvershoot), static_cast<unsigned>(kFlyCamHoldLea),
                         static_cast<unsigned>(kFlyCamHoldRelease));
            }
        }
        g_fix.skipCameraHold = Bool("FixCameraHold", true);
        if (g_fix.skipCameraHold && !melange::mem::Expect(kFlyCamHoldCall, {0xe8, 0x94, 0xb6, 0xfa, 0xff, 0x8b, 0x0d, 0x30, 0xd0, 0x96, 0x00})) {
            LOG_WARN("[fix] FixCameraHold: unexpected code at %08x; not installed", static_cast<unsigned>(kFlyCamHoldCall));
            g_fix.skipCameraHold = false;
        }
        g_fix.alignDrumFuse = Bool("FixDrumFuse", true);
        // mov eax,0x14 / mov fs:[0],ecx | mov [esi+0x84],edx / call ecx | mov [esi+0x84],eax / call edx |
        // cmp byte [esi+0x68],0 / jnz +0xc / mov ecx,esi / call 0x5d1370
        if (melange::mem::Expect(kDrumUpdateRet, {0xb8, 0x14, 0x00, 0x00, 0x00, 0x64, 0x89, 0x0d, 0x00, 0x00, 0x00, 0x00}) &&
            melange::mem::Expect(kDrumFlagExplosion, {0x89, 0x96, 0x84, 0x00, 0x00, 0x00, 0xff, 0xd1}) &&
            melange::mem::Expect(kDrumFlagImpulse, {0x89, 0x86, 0x84, 0x00, 0x00, 0x00, 0xff, 0xd2}) &&
            melange::mem::Expect(kDrumExplodeCheck, {0x80, 0x7e, 0x68, 0x00, 0x75, 0x0c, 0x8b, 0xce, 0xe8, 0xe8, 0xf0, 0xff, 0xff})) {
            // The flag hooks snap the stamp to the grid (the fix) as well as trace it, so install them whenever the fix
            // is on or we are tracing; the update-return hook does the task re-align and logs each drum's first run.
            if (trace || g_fix.alignDrumFuse) {
                ok &= Mid(kDrumUpdateRetHook, &OnDrumUpdateRet, "oil drum update");
                ok &= Mid(kDrumFlagExplosion, &OnDrumFlagExplosion, "oil drum flagged by Explosion");
                ok &= Mid(kDrumFlagImpulse, &OnDrumFlagImpulse, "oil drum flagged by Damage.Impulse");
            }
            if (trace) ok &= Mid(kDrumExplodeCheck, &OnDrumExplodeCheck, "oil drum explosion");
        } else {
            LOG_WARN("[fix] FixDrumFuse: unexpected code at one of %08x %08x %08x %08x; not installed",
                     static_cast<unsigned>(kDrumUpdateRet), static_cast<unsigned>(kDrumFlagExplosion),
                     static_cast<unsigned>(kDrumFlagImpulse), static_cast<unsigned>(kDrumExplodeCheck));
            g_fix.alignDrumFuse = false;
        }
        // Tell the lobby we run it; it runs in a match only once every member says the same (DecideSimFixes).
        if (g_fix.alignDrumFuse) melange::handshake::simfix::Advertise(melange::handshake::simfix::kDrumFuse);
        if (traceTransport) {
            ok &= Mid(A::SteamConnCtor, &OnConnCtor, "XSteamConnection ctor");
            ok &= Mid(A::SteamConnDtor, &OnConnDtor, "XSteamConnection dtor");
            ok &= Mid(A::SteamConnNewSender, &OnNewSender, "unknown-sender path");
        }

        // A dead peer in a 2-player game forfeits instead of aborting with "session no longer available".
        if (Bool("DeadPeerForfeit", false)) {
            if (melange::mem::Expect(A::DeadChannelBranch, {0x0f, 0x87, 0x0b, 0x04, 0x00, 0x00})) {
                const uint8_t jmp[] = {0xe9, 0x0c, 0x04, 0x00, 0x00, 0x90};  // jmp 0x70abf6
                melange::mem::Write(A::DeadChannelBranch, jmp, sizeof(jmp));
                LOG_INFO("[net] DeadPeerForfeit enabled");
            }
        }

        if (Bool("FixResendDuplicates", true)) {
            // push -1 / mov eax,fs:[0] / push 0x7d2af0
            if (melange::mem::Expect(kResendAll, {0x6a, 0xff, 0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x68, 0xf0, 0x2a, 0x7d, 0x00})) {
                g_resendHook = safetyhook::create_inline(kResendAll, &OnResendAll);
                if (!g_resendHook) LOG_ERROR("[net] failed to hook migration resend at %08x", static_cast<unsigned>(kResendAll));
                ok &= static_cast<bool>(g_resendHook);
            }
        }

        if (Bool("FixLobbyLevelNull", true)) {
            // push eax / lea edx,[esp+0x2c] / push edx / call SetString
            if (melange::mem::Expect(kLobbyLevelPush, {0x50, 0x8d, 0x54, 0x24, 0x2c, 0x52, 0xe8}))
                ok &= Mid(kLobbyLevelPush, &OnLobbyLevel, "lobby level lookup");
        }

        if (Bool("FixGirderPanelCamera", true)) {
            // push 0x95d94c / push edi / call IsMessage / add esp,8 / test eax,eax / jz +0x3a / push "UtilityGirder" /
            // push 0 / call SetOverride / mov ecx,[CameraManagerService] / add esp,8 / push "GirderCam" /
            // call SetCamera / push ebp / push esi
            if (melange::mem::Expect(kGirderCloseCase, {0x68, 0x4c, 0xd9, 0x95, 0x00, 0x57, 0xe8, 0xed, 0xfe, 0x12, 0x00,
                                                        0x83, 0xc4, 0x08, 0x85, 0xc0, 0x74, 0x3a, 0x68, 0x38, 0x7e, 0x83,
                                                        0x00, 0x6a, 0x00, 0xe8, 0x4c, 0xc1, 0xfa, 0xff, 0x8b, 0x0d, 0x70,
                                                        0xc3, 0x95, 0x00, 0x83, 0xc4, 0x08, 0x68, 0x80, 0x48, 0x85, 0x00,
                                                        0xe8, 0xd9, 0x26, 0xfc, 0xff, 0x55, 0x56}))
                ok &= Mid(kGirderSetCamera, &OnGirderPanelClose, "girder panel-close SetCamera");
            else
                LOG_WARN("[fix] FixGirderPanelCamera: unexpected code at %08x; not installed",
                         static_cast<unsigned>(kGirderCloseCase));
        }

        // cmp [0x97a614],ebx / jz 0x603c71 / cmp al,bl / jnz 0x603da9. Checked whole before the trace hook takes the
        // cmp; the patch itself only touches the jz.
        g_fix.panelSelectUnderMenu = Bool("FixPanelSelectUnderMenu", false);
        const bool panelGate = melange::mem::Expect(kPanelSelectGate, {0x39, 0x1d, 0x14, 0xa6, 0x97, 0x00, 0x74, 0x29, 0x3a,
                                                                       0xc3, 0x0f, 0x85, 0x59, 0x01, 0x00, 0x00});
        if (g_fix.panelSelectUnderMenu && !panelGate) {
            LOG_WARN("[fix] FixPanelSelectUnderMenu: unexpected code at %08x; not installed",
                     static_cast<unsigned>(kPanelSelectGate));
            g_fix.panelSelectUnderMenu = false;
        }
        if (panelGate && (trace || g_fix.panelSelectUnderMenu))
            ok &= Mid(kPanelSelectGate, &OnPanelSelectGate, "weapon-panel select gate");

        g_traceLayers = trace;
        if (trace) {
            // Off-turn replicated sends and local-only camera switches (see kSendSites, kSetCamera).
            if (SendSitesOriginal()) {
                ok &= Mid(kSendSites[0].at, &OnSpectatorSend<0>, "msg send queue");
                ok &= Mid(kSendSites[1].at, &OnSpectatorSend<1>, "int send queue");
                ok &= Mid(kSendSites[2].at, &OnSpectatorSend<2>, "two-int send queue");
                ok &= Mid(kSendSites[3].at, &OnSpectatorSend<3>, "float send queue");
                ok &= Mid(kSendSites[4].at, &OnSpectatorSend<4>, "two-float send queue");
                ok &= Mid(kSendSites[5].at, &OnSpectatorSend<5>, "string send queue");
            } else {
                LOG_WARN("[net] off-turn send trace: unexpected code at a ReplayMessageStore sender; not installed");
            }
            // SEH prologue: mov eax,fs:[0] / push -1 / push 0x7d0960
            if (melange::mem::Expect(kSetCamera, {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff, 0x68, 0x60, 0x09, 0x7d, 0x00}))
                ok &= Mid(kSetCamera, &OnSetCamera, "SetCamera");
            else
                LOG_WARN("[net] SetCamera trace: unexpected code at %08x; not installed", static_cast<unsigned>(kSetCamera));
        }

        melange::events::Subscribe(melange::events::Event::Frame, [] {
            PollState();
            if (watch && g_lastState) WatchProgressState();
            WatchFrontendLayers();
            if (forceNetLog) {  // same as launching with /LOG ALL: the engine's own NetThrottle/NetService logging
                uintptr_t cfg = Read<uint32_t>(A::ConfigPtr);
                if (cfg && !(Read<uint8_t>(cfg + 0x9a) & 2)) melange::wum::WriteByte(cfg + 0x9a, Read<uint8_t>(cfg + 0x9a) | 2);
            }
            if ((GetAsyncKeyState(VK_F11) & 1) && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000))
                DumpState("manual (Ctrl+Shift+F11)");
        });
        return ok;
    }

    void Uninstall() override {
        SetCameraHoldPatch(false);
        SetPanelSelectPatch(false);
        g_traceLayers = false;
        g_hooks.clear();
        g_resendHook = {};
    }
};
}  // namespace

MELANGE_MODULE(NetSession);
