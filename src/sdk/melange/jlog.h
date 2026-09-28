#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
namespace melange::jlog {
enum class Level : uint8_t { Trace, Debug, Info, Warn, Error, Fatal };

// Builder for one JSONL record. Cheap when the category/level is filtered out (Enabled() is checked first).
// Thread-safe; never blocks on I/O (a writer thread owns the file). Never calls melange::log (no recursion).
class Rec {
  public:
    Rec(std::string_view category, Level lvl, std::string_view msg);
    Rec& Int(const char* k, int64_t v);
    Rec& Uint(const char* k, uint64_t v);
    Rec& Hex(const char* k, uint64_t v);   // "0x..." string
    Rec& Float(const char* k, double v);
    Rec& Str(const char* k, std::string_view v);
    Rec& Bool(const char* k, bool v);
    Rec& Vec3(const char* k, const float v[3]);
    Rec& Raw(const char* k, std::string_view json);  // caller guarantees valid JSON
    void Emit();                                     // also called by the destructor if not emitted
    ~Rec();
  private:
    struct Impl; Impl* p_;
};
bool Enabled(std::string_view category, Level lvl);

struct Session {
    std::wstring root;     // e.g. %USERPROFILE%\Documents\Melange\logs
    std::wstring dir;      // root\2026-09-27_14-03-22_pid1234
    std::string id;        // "2026-09-27_14-03-22_pid1234"
};
const Session& CurrentSession();
std::vector<std::wstring> RecentSessionDirs(size_t max);  // newest first, including the current one

// Blocks until everything queued before the call is on disk, or timeoutMs passes. Safe from any thread;
// FlushFromCrash() is the no-lock, no-allocation variant for the unhandled-exception filter.
bool Flush(uint32_t timeoutMs = 2000);
void FlushFromCrash();

// In-memory tail for the viewer (last N records as formatted JSON lines).
struct Line { uint64_t seq; Level lvl; std::string category; std::string json; };
size_t Tail(uint64_t afterSeq, std::vector<Line>& out, size_t max);

struct Stats { uint64_t records, dropped, bytes, filesRotated; };
Stats GetStats();
}
