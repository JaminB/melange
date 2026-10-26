#pragma once
// p:follow, the peer-side drift correction (design 1.1.3, Critique Y1/Y2). Pure: no clock, no I/O, no game. The media
// host feeds it the host's state `st`, the local player's extrapolated position and the peer bus's clock estimate once
// per frame, and sends whatever Command comes back. Constants are Syncplay's: a 0.10 s deadband, a 5 % speed nudge up
// to 1.5 s, beyond that an exact seek slightly ahead (the lead, learned from how long seeks take here), and 1 s of
// grace after any seek so one correction never cascades into the next. Where a hard seek actually landed is checked
// once its grace is over: the lead is corrected by that miss, and a miss of 0.35 s or more is fixed by one more seek
// (a 5 % nudge would need 7 s or more for it). The host never waits for a peer: a buffering peer catches up by itself.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace melange::media::sync {
inline constexpr double kDeadband = 0.10;       // |drift| below this: nothing (and any nudge ends)
inline constexpr double kHardSeek = 1.5;        // |drift| at or above this: seek
inline constexpr double kHardSeekNoClock = 3.0;  // the same before the bus has 2 clock samples (no nudging then)
inline constexpr double kPausedSeek = 0.25;     // a paused host: seek to its position beyond this
inline constexpr double kNudgeSlow = 0.95, kNudgeFast = 1.05;
inline constexpr double kGraceMs = 1000;        // no correction this long after a seek
inline constexpr double kLeadStart = 0.5, kLeadMin = 0.2, kLeadMax = 3.0, kLeadKeep = 0.7;  // EWMA weight of the old lead
inline constexpr double kSeekPendingMaxMs = 8000;  // a seek that never shows a fresh position is given up after this
inline constexpr double kRefineSeek = 0.35;     // a hard seek that missed by this much is redone once, with the miss

// The host's state as Holo sends it (wum.peer): epoch, item id, play or pause, position (s), rate, and the host's
// wum.peer.now() (ms) at which `p` holds.
struct Target {
    double e = 0;
    std::string i;
    bool play = false;
    double p = 0, r = 1, a = 0;
};

struct Clock {
    bool ready = false;    // the bus has at least 2 samples for the host
    double offsetMs = 0;   // host's clock - mine
};

// The local player this frame.
struct Local {
    bool loaded = false;     // file-loaded seen for the current item: the position means something
    double pos = 0;          // extrapolated position now (s)
    bool paused = false;     // mpv pause as last reported
    bool buffering = false;  // paused-for-cache
    double speed = 1;        // mpv speed as last commanded
    bool live = false;       // a live stream: follow at the live edge, no seek, no nudge (Y2)
    double sampleQpcMs = 0;  // the newest raw position sample (helper QPC ms) and its value, to learn the lead
    double sampleValue = 0;
};

enum class Action : uint8_t { None, Nudge, Seek };
inline const char* ActionName(Action a) { return a == Action::Nudge ? "nudge" : a == Action::Seek ? "seek" : "none"; }

struct Command {
    bool setPause = false, pause = false;
    bool setSpeed = false;
    double speed = 1;
    bool seek = false;
    double seekTo = 0;
};

class Follower {
public:
    // A new host state. False (ignored) when stale: a lower epoch, or the same epoch with an older timestamp.
    // `receivedMs` is my clock when it arrived (used only while the clock is not ready).
    bool Update(const Target& t, double receivedMs) {
        if (have_ && (t.e < cur_.e || (t.e == cur_.e && t.a < cur_.a))) return false;
        if (!have_ || t.i != cur_.i) needHard_ = true;  // a new item (or the first apply) starts with a hard seek
        cur_ = t;
        cur_.r = std::clamp(t.r, 0.25, 4.0);
        receivedMs_ = receivedMs;
        have_ = true;
        return true;
    }
    // The item was (re)opened locally (open, quality change, helper restart): the next step hard-seeks.
    void Reopened() {
        needHard_ = true;
        seekPending_ = false;
        graceUntil_ = 0;
        landing_ = false;
    }
    void Reset() {
        *this = Follower();
    }
    bool Active() const { return have_; }
    const Target& Current() const { return cur_; }

    // Where the host is now, by my clock.
    double TargetPos(double nowMs, const Clock& c) const {
        if (!cur_.play) return cur_.p;
        const double elapsed = c.ready ? (nowMs + c.offsetMs) - cur_.a : nowMs - receivedMs_;
        return cur_.p + std::max(0.0, elapsed) / 1000.0 * cur_.r;
    }

    Command Step(double nowMs, const Local& l, const Clock& c) {
        Command cmd;
        action_ = Action::None;
        hasDrift_ = false;
        if (!have_) return cmd;
        const bool wantPause = !cur_.play;
        if (l.paused != wantPause) {
            cmd.setPause = true;
            cmd.pause = wantPause;
        }
        double speed = cur_.r;
        auto finish = [&] {
            if (std::fabs(speed - l.speed) > 1e-9) {
                cmd.setSpeed = true;
                cmd.speed = speed;
            }
            return cmd;
        };
        if (!l.loaded) return finish();
        if (l.live) return finish();  // live edge: play/pause and rate only, no drift shown

        LearnLead(nowMs, l);
        const double target = TargetPos(nowMs, c);
        drift_ = l.pos - target;
        hasDrift_ = true;
        const double ad = std::fabs(drift_);

        if (l.buffering) {
            // The host never waits. A stall we did not cause ends in a hard seek to wherever the host is by then.
            if (!seekPending_) needHard_ = true;
            return finish();
        }
        if (nowMs < graceUntil_ || (seekPending_ && nowMs - seekIssuedMs_ < kSeekPendingMaxMs)) return finish();
        seekPending_ = false;

        if (wantPause) {
            if (ad > kPausedSeek || (needHard_ && ad > 0.05)) IssueSeek(nowMs, cur_.p, false, &cmd);
            needHard_ = false;
            landing_ = false;  // a paused seek goes to p, with no lead to correct
            return finish();
        }
        const double hard = c.ready ? kHardSeek : kHardSeekNoClock;
        const bool landed = landing_;  // the first look after a hard seek: how far off it landed
        landing_ = false;
        if (needHard_ || ad >= hard) {
            IssueSeek(nowMs, target + lead_, true, &cmd);
            needHard_ = false;
            landing_ = true;
            refined_ = false;
        } else if (landed && c.ready && ad >= kDeadband) {
            // That seek went to target + leadUsed_ and is drift_ off now: target + leadUsed_ - drift_ would have hit.
            lead_ = std::clamp(leadUsed_ - drift_, kLeadMin, kLeadMax);
            if (ad >= kRefineSeek && !refined_) {
                IssueSeek(nowMs, target + lead_, true, &cmd);
                landing_ = true;
                refined_ = true;  // once: a second miss is left to the nudge, so seeks never chain
            } else {
                speed = cur_.r * (drift_ > 0 ? kNudgeSlow : kNudgeFast);
                action_ = Action::Nudge;
            }
        } else if (c.ready && ad >= kDeadband) {
            speed = cur_.r * (drift_ > 0 ? kNudgeSlow : kNudgeFast);
            action_ = Action::Nudge;
        }
        return finish();
    }

    Action LastAction() const { return action_; }
    bool HasDrift() const { return hasDrift_; }
    double Drift() const { return drift_; }
    double Lead() const { return lead_; }

private:
    // `learn`: a seek while playing, timed to learn the lead (a paused seek shows no fresh samples).
    void IssueSeek(double nowMs, double to, bool learn, Command* cmd) {
        cmd->seek = true;
        cmd->seekTo = std::max(0.0, to);
        action_ = Action::Seek;
        graceUntil_ = nowMs + kGraceMs;
        seekPending_ = learn;
        seekIssuedMs_ = nowMs;
        seekTo_ = cmd->seekTo;
        leadUsed_ = lead_;
    }
    // The first fresh position sample near the seek target, taken after the seek and while playing, ends the seek:
    // its delay is how long a seek takes on this machine and item, which the next seek leads by.
    void LearnLead(double nowMs, const Local& l) {
        if (!seekPending_) return;
        if (nowMs - seekIssuedMs_ > kSeekPendingMaxMs) {
            seekPending_ = false;
            landing_ = false;  // never confirmed: where it landed is unknown, so nothing is corrected by it
            return;
        }
        if (l.buffering || l.paused || l.sampleQpcMs <= seekIssuedMs_ || std::fabs(l.sampleValue - seekTo_) > 1.0) return;
        const double took = (l.sampleQpcMs - seekIssuedMs_) / 1000.0;
        lead_ = std::clamp(kLeadKeep * lead_ + (1 - kLeadKeep) * took, kLeadMin, kLeadMax);
        seekPending_ = false;
    }

    Target cur_;
    bool have_ = false, needHard_ = true;
    double receivedMs_ = 0;
    double graceUntil_ = 0;
    bool seekPending_ = false;
    double seekIssuedMs_ = 0, seekTo_ = 0;
    double lead_ = kLeadStart;
    double leadUsed_ = kLeadStart;  // the lead the last seek went with
    bool landing_ = false;          // a playing hard seek was issued: check where it landed after its grace
    bool refined_ = false;          // the last hard seek was already a redo
    Action action_ = Action::None;
    bool hasDrift_ = false;
    double drift_ = 0;
};

// The local position now from the newest sample (Critique Y1): the helper stamps each time-pos/audio-pts with its QPC
// time, which every process on the machine shares, so the queue's 0-130 ms of delay cancels out.
inline double Extrapolate(double sample, double sampleQpcMs, double nowMs, double speed, bool advancing, double duration) {
    double pos = sample;
    if (advancing && sampleQpcMs > 0) pos += std::clamp(nowMs - sampleQpcMs, 0.0, 5000.0) / 1000.0 * speed;
    if (duration > 0) pos = std::min(pos, duration);
    return std::max(0.0, pos);
}
}  // namespace melange::media::sync
