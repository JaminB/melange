#pragma once
#include <string>

// Thread-safe line logger. Every line is flushed immediately so the log survives crashes and hangs.
namespace melange::log {
void Init(const std::wstring& path);
void Write(const char* level, const char* fmt, ...);
void WriteRaw(const char* text);  // no prefix, no newline added
void HexDump(const char* title, const void* data, size_t len, size_t max = 64);

// Component C's WF_ tap (docs/m0-design.md SS3 "C", adapter 1): called from Write(), after formatting but
// before the line is prefixed with a timestamp, with the same `level` string the caller passed (e.g. "INFO ",
// "WARN ", "ENG  "). Lets jlog mirror every WF_INFO/WARN/ERROR/TRACE line (and EngineLog's "ENG  " lines) into
// the structured log without log.cpp linking against jlog. At most one tap; set once, at startup. The tap must
// not call back into melange::log (recursion) and must not throw.
using Tap = void (*)(const char* level, const char* msg);
void SetTap(Tap fn);
}  // namespace melange::log

#define WF_INFO(...) ::melange::log::Write("INFO ", __VA_ARGS__)
#define WF_WARN(...) ::melange::log::Write("WARN ", __VA_ARGS__)
#define WF_ERROR(...) ::melange::log::Write("ERROR", __VA_ARGS__)
#define WF_TRACE(...) ::melange::log::Write("TRACE", __VA_ARGS__)
