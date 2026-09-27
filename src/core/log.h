#pragma once
#include <string>

// Thread-safe line logger. Every line is flushed immediately so the log survives crashes and hangs.
namespace wf::log {
void Init(const std::wstring& path);
void Write(const char* level, const char* fmt, ...);
void WriteRaw(const char* text);  // no prefix, no newline added
void HexDump(const char* title, const void* data, size_t len, size_t max = 64);
}  // namespace wf::log

#define WF_INFO(...) ::wf::log::Write("INFO ", __VA_ARGS__)
#define WF_WARN(...) ::wf::log::Write("WARN ", __VA_ARGS__)
#define WF_ERROR(...) ::wf::log::Write("ERROR", __VA_ARGS__)
#define WF_TRACE(...) ::wf::log::Write("TRACE", __VA_ARGS__)
