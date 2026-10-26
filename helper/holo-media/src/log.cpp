#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "util.h"

namespace holo::log {
namespace {
std::mutex g_mu;
HANDLE g_file = INVALID_HANDLE_VALUE;
uint64_t g_bytes = 0;
constexpr uint64_t kCap = 2u << 20;
const double g_t0 = QpcMs();
}  // namespace

void Open(const std::wstring& path) {
    std::lock_guard lk(g_mu);
    if (g_file != INVALID_HANDLE_VALUE) return;
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void Line(const char* fmt, ...) {
    char buf[2048];
    const int head = snprintf(buf, sizeof buf, "[%9.3f] ", (QpcMs() - g_t0) / 1000.0);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + head, sizeof buf - head - 2, fmt, ap);
    va_end(ap);
    if (n < 0) n = 0;
    size_t len = static_cast<size_t>(head) + (static_cast<size_t>(n) < sizeof buf - head - 2 ? n : sizeof buf - head - 3);
    buf[len++] = '\n';
    std::lock_guard lk(g_mu);
    if (g_file == INVALID_HANDLE_VALUE) {
        fwrite(buf, 1, len, stderr);
        return;
    }
    if (g_bytes + len > kCap) return;
    DWORD w = 0;
    WriteFile(g_file, buf, static_cast<DWORD>(len), &w, nullptr);
    g_bytes += w;
}
}  // namespace holo::log
