#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Is this folder a supported Worms Ultimate Mayhem? Size and PE timestamp first, SHA-256 only when they match.
namespace melange::launcher::setup {
struct Profile {
    uint64_t size;
    uint32_t timestamp;
    std::string sha256, name;
};
const std::vector<Profile>& DefaultProfiles();

enum class Verdict { Ok, WrongBuild, NoExe, NotFound, Unreadable };
const char* VerdictName(Verdict v);

struct ExeFacts {
    bool present = false;
    uint64_t size = 0;
    uint32_t timestamp = 0;
    std::string sha256, build;
};
struct GameCheck {
    std::wstring path;
    Verdict verdict = Verdict::NotFound;
    std::string store = "unknown";   // steam | gog | unknown (set by the caller)
    bool running = false, writable = false;
    ExeFacts exe;
    std::string error;
};

// The exe verdict only; running/writable/store are left to the caller. Cached per (path, size, write time).
GameCheck CheckExe(const std::wstring& dir, const std::vector<Profile>& profiles = DefaultProfiles());
// Whether the current user may create files in `dir` (an access check, nothing is written).
bool CanWrite(const std::wstring& dir);
std::string GameCheckJson(const GameCheck& c);

extern std::atomic<int> g_hashCount;   // self-test: how many files were hashed
void ClearExeCache();
}  // namespace melange::launcher::setup
