#include "levels/test.h"

#include <algorithm>
#include <cstdio>

namespace melange::levels::test {
bool Override::Arm(const std::string& key, int timeoutS, uint64_t nowMs, Tod tod) {
    if (key.empty() || phase_ == Phase::Starting || phase_ == Phase::Playing) return false;
    const int t = std::clamp(timeoutS, 1, kMaxTimeoutS);
    phase_ = Phase::Armed;
    key_ = key;
    frontend_.clear();
    deadline_ = nowMs + static_cast<uint64_t>(t) * 1000;
    started_ = 0;
    tod_ = tod;
    startSeen_ = false;
    return true;
}

void Override::Disarm() {
    phase_ = Phase::Idle;
    key_.clear();
    frontend_.clear();
    tod_ = Tod::Default;
    startSeen_ = false;
}

void Override::NoteStartGame(const std::string& text) {
    if (phase_ == Phase::Armed && text.rfind("QuickStart", 0) == 0) startSeen_ = true;
}

std::string Override::Take(const std::string& frontendKey, uint64_t nowMs, Event* ev, const SetUp& s) {
    if (ev) *ev = Event::None;
    switch (phase_) {
        case Phase::Armed:
            if (nowMs > deadline_) {
                Disarm();
                if (ev) *ev = Event::Expired;
                return "";
            }
            if (s.attract) {
                Disarm();
                if (ev) *ev = Event::AttractRefused;
                return "";
            }
            if (!startSeen_ || s.loading) return "";
            phase_ = Phase::Starting;
            frontend_ = frontendKey;
            started_ = nowMs;
            if (ev) *ev = Event::Started;
            return key_;
        case Phase::Starting:
        case Phase::Playing:
            return frontendKey == frontend_ || frontendKey == key_ ? key_ : "";
        default:
            return "";
    }
}

Override::Event Override::Update(bool inMatch, uint64_t nowMs) {
    switch (phase_) {
        case Phase::Armed:
            if (nowMs > deadline_) {
                Disarm();
                return Event::Expired;
            }
            return Event::None;
        case Phase::Starting:
            if (inMatch) {
                phase_ = Phase::Playing;
                return Event::Playing;
            }
            if (nowMs - started_ > kStartWindowMs) {
                Disarm();
                return Event::Abandoned;
            }
            return Event::None;
        case Phase::Playing:
            if (!inMatch) {
                Disarm();
                return Event::Ended;
            }
            return Event::None;
        default:
            return Event::None;
    }
}

const char* StateName(TestState s) {
    switch (s) {
        case TestState::Idle: return "idle";
        case TestState::Registering: return "registering";
        case TestState::Registered: return "registered";
        case TestState::Armed: return "armed";
        case TestState::Starting: return "starting";
        case TestState::Playing: return "playing";
        case TestState::Ended: return "ended";
        case TestState::Failed: return "failed";
    }
    return "?";
}

const char* TodName(Tod t) {
    switch (t) {
        case Tod::Day: return "DAY";
        case Tod::Evening: return "EVENING";
        case Tod::Night: return "NIGHT";
        default: return "";
    }
}

bool ParseTod(const std::string& s, Tod* out) {
    for (Tod t : {Tod::Default, Tod::Day, Tod::Evening, Tod::Night})
        if (s == TodName(t)) {
            if (out) *out = t;
            return true;
        }
    return false;
}

const char* SourceName(Source s) {
    switch (s) {
        case Source::Pack: return "pack";
        case Source::Test: return "test";
        default: return "vanilla";
    }
}

namespace {
std::string Quote(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (u < 0x20 || u >= 0x7f) {
            char b[8];
            snprintf(b, sizeof b, "\\u%04x", u);
            o += b;
        } else {
            o += c;
        }
    }
    return o + "\"";
}
}  // namespace

std::string StateJson(TestState s, const std::string& key, const std::string& detail) {
    return std::string("{\"state\":") + Quote(StateName(s)) + ",\"key\":" + Quote(key) + ",\"detail\":" + Quote(detail) +
           "}";
}

std::string LevelJson(const std::string& key, const std::string& stem, Source source, bool online, const float* water) {
    std::string w = "null";
    if (water) {
        char b[32];
        snprintf(b, sizeof b, "%.4f", static_cast<double>(*water));
        w = b;
    }
    return "{\"level\":" + Quote(key) + ",\"stem\":" + Quote(stem) + ",\"source\":" + Quote(SourceName(source)) +
           ",\"online\":" + (online ? "true" : "false") + ",\"water\":" + w + "}";
}
}  // namespace melange::levels::test
