#pragma once
// wum.game.turn() and wum.game.ui() without the game: the turn phase from the engine's timer messages, "mine" with its
// latch across the gap between turns, and the change detection behind melange.turn and melange.ui. The Sandbox module
// feeds it the snapshot, the bus messages and a few reads of game memory; the self-tests feed it by hand.
#include <cstdint>

namespace melange::gamestate {
enum class Local : uint8_t { Unknown, No, Yes };  // whether the player whose turn it is runs on this machine (online)
enum class TurnPhase : uint8_t { None, Intro, Play, Retreat, Settle };

inline const char* PhaseName(TurnPhase p) {
    switch (p) {
        case TurnPhase::Intro: return "intro";
        case TurnPhase::Play: return "play";
        case TurnPhase::Retreat: return "retreat";
        case TurnPhase::Settle: return "settle";
        case TurnPhase::None: break;
    }
    return "none";
}

// The engine messages that move the phase (all on the Post path).
enum class TurnMsg : uint8_t {
    TurnStarted,        // GameLogic.Turn.Started
    TurnEnded,          // GameLogic.Turn.Ended
    HotSeatStart,       // Timer.StartHotSeatTimer: the "Ready?" intro
    HotSeatEnd,         // Timer.EndHotSeatTimer and Timer.HotSeatTimedOut
    RetreatStart,       // Timer.StartRetreatTimer
    PostActivityStart,  // Timer.StartPostActivity: the settle after the shot
    PanelOpened,        // GameLogic.WeaponPanelOpened
    PanelClosed,        // GameLogic.WeaponPanelClosed
};

struct TurnInput {
    bool inMatch = false, online = false;
    int team = -1, worm = -1;               // CurrentTeamIndex, ActiveWormIndex; -1 between turns
    bool teamLocal = false, teamAi = false; // of the current team (offline "mine")
    Local current = Local::Unknown;         // online: NetPlayer +0x4e of CurrentPlayer
};
struct TurnView {
    int team = -1, worm = -1;
    bool ai = false, mine = false;
    TurnPhase phase = TurnPhase::None;
    bool operator==(const TurnView& o) const {
        return team == o.team && worm == o.worm && ai == o.ai && mine == o.mine && phase == o.phase;
    }
};
struct UiView {
    bool weaponPanel = false, pause = false, readyIntro = false, feCursor = false, attract = false;
    // The three that melange.ui reports; the other two are polled by Lua.
    bool SameEvent(const UiView& o) const {
        return weaponPanel == o.weaponPanel && pause == o.pause && readyIntro == o.readyIntro;
    }
};

class TurnTracker {
public:
    void Reset() { *this = TurnTracker(); }  // a new match (or none)

    void OnMessage(TurnMsg m) {
        switch (m) {
            case TurnMsg::TurnStarted:
                if (phase_ != TurnPhase::Intro) phase_ = TurnPhase::Play;  // the intro may have begun first
                break;
            case TurnMsg::TurnEnded:
                phase_ = TurnPhase::None;
                panel_ = false;
                break;
            case TurnMsg::HotSeatStart: phase_ = TurnPhase::Intro; break;
            case TurnMsg::HotSeatEnd:
                if (phase_ == TurnPhase::Intro) phase_ = TurnPhase::Play;
                break;
            case TurnMsg::RetreatStart: phase_ = TurnPhase::Retreat; break;
            case TurnMsg::PostActivityStart: phase_ = TurnPhase::Settle; break;
            case TurnMsg::PanelOpened: panel_ = true; break;
            case TurnMsg::PanelClosed: panel_ = false; break;
        }
    }

    TurnView Update(const TurnInput& in) {
        TurnView v;
        if (!in.inMatch) {
            Reset();
            return v;
        }
        if (in.team >= 0) {
            v.team = in.team;
            v.worm = in.worm >= 0 ? in.worm : -1;
            v.ai = in.teamAi;
        }
        if (in.online) {
            // Unknown between turns (CurrentPlayer is 0): keep the last known value, so nothing flickers at the boundary.
            if (in.current != Local::Unknown) latched_ = in.current == Local::Yes;
            v.mine = latched_;
        } else {
            latched_ = false;
            v.mine = in.team >= 0 && in.teamLocal && !in.teamAi;
        }
        v.phase = in.team >= 0 ? phase_ : TurnPhase::None;
        return v;
    }

    bool WeaponPanel() const { return panel_; }
    bool ReadyIntro(const TurnView& v) const { return v.phase == TurnPhase::Intro; }

private:
    TurnPhase phase_ = TurnPhase::None;
    bool panel_ = false, latched_ = false;
};
}  // namespace melange::gamestate
