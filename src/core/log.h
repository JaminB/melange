#pragma once
#include <string>

// Thread-safe line logger. Every line is written through to the system cache at once (no buffer in this process),
// so the log survives crashes, hangs and kills; it does not wait for the disk.
namespace melange::log {
void Init(const std::wstring& path);
void Write(const char* level, const char* fmt, ...);
void WriteRaw(const char* text);  // no prefix, no newline added
void HexDump(const char* title, const void* data, size_t len, size_t max = 64);

// Log tap: receives every formatted line (before the timestamp) with its level string, so jlog can mirror it.
// One tap, set once at startup. It must not call back into melange::log and must not throw.
using Tap = void (*)(const char* level, const char* msg);
void SetTap(Tap fn);
}  // namespace melange::log

#define LOG_INFO(...) ::melange::log::Write("INFO ", __VA_ARGS__)
#define LOG_WARN(...) ::melange::log::Write("WARN ", __VA_ARGS__)
#define LOG_ERROR(...) ::melange::log::Write("ERROR", __VA_ARGS__)
#define LOG_TRACE(...) ::melange::log::Write("TRACE", __VA_ARGS__)
