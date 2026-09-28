#include "core/log.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace melange::log {
namespace {
HANDLE g_file = INVALID_HANDLE_VALUE;
SRWLOCK g_lock = SRWLOCK_INIT;
ULONGLONG g_start = 0;
std::atomic<Tap> g_tap{nullptr};

void Append(const char* s, size_t n) {
    if (g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_file, s, static_cast<DWORD>(n), &written, nullptr);
    FlushFileBuffers(g_file);
}
}  // namespace

void Init(const std::wstring& path) {
    // Keep the previous session's log: after a crash/hang the user restarts the game and we must not lose evidence.
    std::wstring prev = path;
    prev.insert(prev.rfind(L'.'), L".prev");
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    g_start = GetTickCount64();
}

void Write(const char* level, const char* fmt, ...) {
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (Tap tap = g_tap.load(std::memory_order_acquire)) tap(level, msg);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2300];
    int n = snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u [+%8.3f] [%5lu] %s %s\r\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, (GetTickCount64() - g_start) / 1000.0, GetCurrentThreadId(), level,
                     msg);
    if (n < 0) return;
    if (n >= static_cast<int>(sizeof(line))) n = sizeof(line) - 1;
    AcquireSRWLockExclusive(&g_lock);
    Append(line, n);
    ReleaseSRWLockExclusive(&g_lock);
}

void WriteRaw(const char* text) {
    AcquireSRWLockExclusive(&g_lock);
    Append(text, strlen(text));
    ReleaseSRWLockExclusive(&g_lock);
}

void HexDump(const char* title, const void* data, size_t len, size_t max) {
    const auto* p = static_cast<const unsigned char*>(data);
    size_t n = len < max ? len : max;
    char buf[64 * 3 + 16];
    size_t o = 0;
    for (size_t i = 0; i < n && o + 4 < sizeof(buf); ++i) o += snprintf(buf + o, sizeof(buf) - o, "%02x ", p[i]);
    buf[o] = 0;
    Write("TRACE", "%s (%zu bytes): %s%s", title, len, buf, len > max ? "..." : "");
}

void SetTap(Tap fn) { g_tap.store(fn, std::memory_order_release); }
}  // namespace melange::log
