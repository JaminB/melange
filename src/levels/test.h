#pragma once
#include <cstdint>
#include <string>

#include "melange/levels.h"

// The Erg Test workspace: the one-shot level override (pure, test_arm.cpp) and Test registration (test.cpp).
namespace melange::levels::test {
// Armed until used, disarmed, or timed out. The host runs the level set-up twice per start with the same key, so a
// used override keeps answering for that frontend key until the match it started ends.
class Override {
public:
    enum class Phase : uint8_t { Idle, Armed, Starting, Playing };
    enum class Event : uint8_t { None, Expired, Started, Playing, Ended, Abandoned };

    bool Arm(const std::string& key, int timeoutS, uint64_t nowMs);
    void Disarm();
    Phase phase() const { return phase_; }
    const std::string& key() const { return key_; }
    bool armed() const { return phase_ == Phase::Armed; }
    // At a level set-up: the key to load instead of frontendKey, or "".
    std::string Take(const std::string& frontendKey, uint64_t nowMs, Event* ev);
    // Once per frame.
    Event Update(bool inMatch, uint64_t nowMs);

    static constexpr uint64_t kStartWindowMs = 60000;   // a started override that never reaches a match
    static constexpr int kMaxTimeoutS = 3600;

private:
    Phase phase_ = Phase::Idle;
    std::string key_, frontend_;
    uint64_t deadline_ = 0, started_ = 0;
};

const char* StateName(TestState s);   // "idle", "registering", ...
const char* SourceName(Source s);     // "vanilla", "pack", "test"

// The "erg" channel payloads: {"state","key","detail"} and {"level","stem","source","online","water"}.
std::string StateJson(TestState s, const std::string& key, const std::string& detail);
std::string LevelJson(const std::string& key, const std::string& stem, Source source, bool online, const float* water);

// Game side (test.cpp).
bool Register(const char* stem, const char* title, char* err, size_t errLen);   // any thread; queued
bool Arm(const char* key, int timeoutS);
void Disarm(const char* why);
bool Armed(char* key, size_t keyLen);
const char* Take(const char* frontendKey);
void OnFrame(bool atFrontend);
bool Pending(const std::string& key);
}  // namespace melange::levels::test
