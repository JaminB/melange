// The weapon handshake at runtime: the clone lobby policy, the host's start refusal, the lobby banner and the
// joiner's Leave lobby action. Nothing here kicks anyone.
#include "mods/weapon_gate.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "melange/jlog.h"
#include "melange/mods.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"
#include "melange/weapons.h"
#include "mods/handshake_internal.h"
#include "mods/lobby.h"
#include "mods/lobbybanner.h"
#include "mods/starthold.h"
#include "net/net.h"
#include "weapons/engine.h"
#include "weapons/manifest.h"

namespace melange::handshake::wpngate {
namespace lobbybanner = mods::lobbybanner;
namespace {
constexpr uint32_t kLeaveCode = 0x8021012D;  // Net.NotViable, the code the engine uses when the host is lost in the lobby
constexpr int kEvalEvery = 15;

Policy g_policy = Policy::Refuse;
bool g_leaveButton = true;
bool g_installed = false;

int g_holdHandle = 0;
int g_banner = 0;
bool g_hookFailLogged = false;
uint32_t g_loggedFrames = 0;
uint64_t g_lastRefuseLog = 0;

View g_view;
bool g_wasHeld = false;
std::string g_modalKey;
bool g_modalPending = false;
bool g_openedOverlay = false;
std::atomic<bool> g_leaveRequested{false};

std::string Diff(uint64_t member) {
    std::string d = PeerModsDiff(member);
    const std::string m = PeerMsgDiff(member);
    if (!m.empty()) d += (d.empty() ? "" : "; ") + m;
    return d;
}

CloneLobbyInput Gather() {
    CloneLobbyInput in;
    const uint64_t l = lobby::Current();
    in.inLobby = l != 0;
    if (!in.inLobby) return in;
    const uint64_t me = lobby::Me(), owner = lobby::Owner();
    in.weAreOwner = owner != 0 && owner == me;
    in.haveClones = LocalClones();
    in.ourHash16 = Hash16(mods::LocalContent());
    in.lobbyReq = lobby::Data("mlg.req");
    if (!in.weAreOwner && owner && (in.haveClones || !in.lobbyReq.empty())) {
        in.hostMods = lobby::MemberData(owner, "mlg.mods");
        in.diffToHost = Diff(owner);
    }
    if (in.weAreOwner && in.haveClones) {
        for (uint64_t m : lobby::Members()) {
            LobbyMember lm;
            lm.name = lobby::Name(m);
            std::string version;
            uint32_t n = 0;
            lm.hasMlg = ParseMlgValue(lobby::MemberData(m, "mlg"), &version, &lm.hash16, &n);
            if (lm.hasMlg && lm.hash16 != in.ourHash16 && lm.hash16 != "v") lm.diff = Diff(m);
            in.members.push_back(std::move(lm));
        }
    }
    return in;
}

View Evaluate() {
    const CloneLobbyInput in = Gather();
    const CloneVerdict v = EvaluateCloneLobby(in);
    View out;
    out.inLobby = in.inLobby;
    out.owner = in.weAreOwner;
    out.hostHeld = v.hostHeld;
    out.refusing = v.hostHeld && g_policy == Policy::Refuse && mods::starthold::Available();
    out.joinerMismatch = v.joinerMismatch;
    out.why = v.why;
    out.members = v.members;
    return out;
}

bool InLobbyScreen() {
    return game::IsKnownBuild() && wum::CurrentState() == wum::state::WaitingGameStart;
}

bool Wanted() { return g_policy == Policy::Refuse && g_view.inLobby && g_view.owner && LocalClones(); }

bool HoldReason(std::string* why, void*) {
    if (!Wanted() || !g_view.hostHeld) return false;
    if (why) *why = g_view.why;
    return true;
}

void LogRefusals() {
    const uint32_t n = mods::starthold::HeldFrames("weapons");
    if (n == g_loggedFrames) return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastRefuseLog < 2000) return;
    g_lastRefuseLog = now;
    LOG_INFO("[handshake] match start held for weapon content (%u frames so far): %s", n, g_view.why.c_str());
    g_loggedFrames = n;
}

void OpenModal() {
    g_modalPending = true;
    if (!overlay::Visible()) {
        overlay::SetVisible(true);
        g_openedOverlay = true;
    }
    overlay::OpenPanel("thumper.lobby", true);
}

bool CallAbort(uintptr_t ns) {
    __try {
        reinterpret_cast<void(__thiscall*)(uintptr_t, uint32_t)>(wum::addr::AbortGame)(ns, kLeaveCode);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void Tick() {
    if (g_leaveRequested.exchange(false)) {
        const uintptr_t ns = game::IsKnownBuild() ? wum::NetService() : 0;
        const bool joiner = lobby::Current() && lobby::Owner() != lobby::Me();
        if (ns && joiner && InLobbyScreen()) {
            LOG_INFO("[handshake] leaving the lobby at the player's request (weapon content differs)");
            jlog::Rec("handshake", jlog::Level::Info, "leave_lobby").Str("why", g_view.why).Emit();
            if (!CallAbort(ns)) LOG_ERROR("[handshake] leaving the lobby failed");
        }
    }
    if (events::FrameCount() % kEvalEvery) return;
    g_view = Evaluate();
    const bool refusing = Wanted() && g_view.hostHeld && mods::starthold::Available();
    if (Wanted() && g_view.hostHeld && !mods::starthold::Available() && !g_hookFailLogged) {
        g_hookFailLogged = true;
        LOG_ERROR("[handshake] the start refusal is unavailable: a mismatched lobby falls back to clones off");
    }
    g_view.refusing = refusing;
    if (g_view.hostHeld != g_wasHeld) {
        g_wasHeld = g_view.hostHeld;
        if (g_view.hostHeld)
            LOG_WARN("[handshake] weapon content: %s %s", g_view.refusing ? "the match start is refused:" : "clones will be off:",
                     g_view.why.c_str());
        jlog::Rec("handshake", g_view.hostHeld ? jlog::Level::Warn : jlog::Level::Info, "clone_lobby")
            .Bool("held", g_view.hostHeld).Bool("refusing", g_view.refusing).Str("why", g_view.why).Emit();
    }
    LogRefusals();
    if (!g_view.inLobby) {
        g_modalKey.clear();
        return;
    }
    if (g_view.joinerMismatch) {
        const std::string key = std::to_string(lobby::Current()) + "|" + lobby::Data("mlg.req");
        if (key != g_modalKey) {
            g_modalKey = key;
            LOG_WARN("[handshake] weapon content: %s", g_view.why.c_str());
            jlog::Rec("handshake", jlog::Level::Warn, "clone_lobby_joined").Str("why", g_view.why).Emit();
            OpenModal();
        }
    }
}

void UpdateBanner() {
    constexpr uint32_t kAmber = 0xff30a0ffu;
    if (!g_banner) return;
    const View& v = g_view;
    if (!v.inLobby || !(v.hostHeld || v.joinerMismatch)) lobbybanner::Set(g_banner, kAmber, "", {});
    else if (v.hostHeld)
        lobbybanner::Set(g_banner, kAmber,
                         v.refusing ? "Clone weapons: the match cannot start until every player has the same mods."
                                    : "Clone weapons will be off this match: not every player has the same mods.",
                         v.members);
    else lobbybanner::Set(g_banner, kAmber, "Clone weapons: your mods differ from the host's.", {v.why});
}

int HookState() {
    int st = -1;
    mods::starthold::HookEnabled(&st);
    return st;
}

bool VerbState(std::string_view, void*) {
    const View v = Evaluate();
    LOG_INFO("[handshake] wpn: policy=%s clones=%d inLobby=%d owner=%d held=%d refusing=%d joinerMismatch=%d hook=%d "
             "refusedFrames=%u req='%s' why='%s'",
             g_policy == Policy::Refuse ? "refuse" : "suspend", LocalClones(), v.inLobby, v.owner, v.hostHeld,
             g_view.refusing, v.joinerMismatch, HookState(), mods::starthold::HeldFrames("weapons"),
             lobby::Data("mlg.req").c_str(), v.why.c_str());
    for (uint64_t m : lobby::Members())
        LOG_INFO("[handshake]   %s mlg='%s' wpn='%s'", lobby::Name(m).c_str(), lobby::MemberData(m, "mlg").c_str(),
                 lobby::MemberData(m, "mlg.wpn").c_str());
    return true;
}

bool VerbLeave(std::string_view, void*) {
    if (!LeaveAvailable()) return false;
    RequestLeave();
    return true;
}
}  // namespace

void Install(Policy policy, bool leaveButton) {
    if (g_installed) return;
    g_installed = true;
    g_policy = policy;
    g_leaveButton = leaveButton;
    events::Subscribe(events::Event::Frame, [] {
        Tick();
        UpdateBanner();
    });
    g_holdHandle = mods::starthold::Add("weapons", &HoldReason, nullptr);
    g_banner = lobbybanner::Add("weapons", 10);
    testcmd::Register("handshake.wpn", &VerbState);
    testcmd::Register("handshake.leave", &VerbLeave);
    LOG_INFO("[handshake] weapon gate: %s, %d clone(s) and %d rename(s) declared", policy == Policy::Refuse ? "refuse" : "suspend",
             LocalClones() ? static_cast<int>(weapons::manifest::Frozen().size()) : 0,
             LocalClones() ? static_cast<int>(weapons::manifest::FrozenText().size()) : 0);
}

Policy CurrentPolicy() { return g_policy; }

bool LocalClones() {
    // Vanilla renames count as weapon content: peers must show the same names, so they get the same gate as clones.
    return weapons::Enabled() && ((weapons::manifest::IsFrozen() && !weapons::manifest::Frozen().empty()) ||
                                  (weapons::manifest::IsTextFrozen() && !weapons::manifest::FrozenText().empty()));
}

View Current() { return g_view; }

bool TakeModalRequest() {
    const bool r = g_modalPending;
    g_modalPending = false;
    return r;
}

bool LeaveAvailable() {
    return g_leaveButton && game::IsKnownBuild() && lobby::Current() && lobby::Owner() != lobby::Me() && InLobbyScreen();
}

void RequestLeave() { g_leaveRequested = true; }

void ModalClosed() {
    if (g_openedOverlay) overlay::SetVisible(false);
    g_openedOverlay = false;
}
}  // namespace melange::handshake::wpngate

namespace melange::mods {
bool CloneLobbyOk(std::string* why) {
    const handshake::CloneVerdict v = handshake::EvaluateCloneLobby(handshake::wpngate::Gather());
    if (why) *why = v.why;
    if (!v.ok)
        jlog::Rec("handshake", jlog::Level::Warn, "clone_gate").Str("why", v.why).Bool("hostHeld", v.hostHeld).Emit();
    return v.ok;
}
}  // namespace melange::mods
