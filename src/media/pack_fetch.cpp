// The media pack's downloads (pack_internal.h): one resumable, hashed, throttled GET into a .part file, for the pack
// zip and the yt-dlp lane. The store::fetch FileSink pattern (install::FetchVerified), plus a resume after a dropped
// connection: a 95 MB download should not start over.
#include <windows.h>

#include <optional>

#include "core/log.h"
#include "media/pack_internal.h"
#include "store/fetch.h"
#include "tools/hash.h"

namespace melange::media::pack::core {
namespace {
constexpr ULONGLONG kProgressMs = 100;

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

struct PartSink : store::fetch::Sink {
    HANDLE f = INVALID_HANDLE_VALUE;
    std::optional<hashutil::Sha256> sha;
    uint64_t n = 0, size = 0;
    const Hooks* h = nullptr;
    bool ioerr = false, restarted = false;
    ULONGLONG lastProgress = 0, windowStart = 0;
    uint64_t windowBytes = 0;

    bool Start(uint64_t offset, uint64_t) override {
        if (offset == n) return true;
        if (offset != 0) return false;
        // The server ignored the range: drop the kept bytes and hash from the start.
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(f, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(f)) {
            ioerr = true;
            return false;
        }
        sha.emplace();
        n = 0;
        restarted = true;
        return true;
    }
    bool Write(const void* p, size_t len) override {
        DWORD w = 0;
        if (!WriteFile(f, p, static_cast<DWORD>(len), &w, nullptr) || w != len) {
            ioerr = true;
            return false;
        }
        n += len;
        if (!sha->Update(p, len)) return false;
        const ULONGLONG now = GetTickCount64();
        if (h->progress && now - lastProgress >= kProgressMs) {
            lastProgress = now;
            h->progress(n, size);
        }
        // The match throttle ([Media] MatchDownloadKBps), re-read as it goes: a match can start mid-download.
        const uint32_t kbps = h->throttleKBps ? h->throttleKBps() : 0;
        if (!kbps || now - windowStart > 2000) {
            windowStart = now;
            windowBytes = 0;
        }
        windowBytes += len;
        if (const uint32_t ms = logic::ThrottleSleepMs(windowBytes, now - windowStart, kbps)) Sleep(ms);
        return true;
    }
};

// Hashes the first `n` bytes of `f` (a kept partial download) through one counted 1 MiB buffer.
bool HashPrefix(HANDLE f, uint64_t n, hashutil::Sha256* sha, const Hooks& h) {
    char* buf = static_cast<char*>(CountedAlloc(kIoBufferBytes));
    if (!buf) return false;
    uint64_t done = 0;
    bool ok = true;
    while (ok && done < n) {
        if (h.cancel && h.cancel->load()) ok = false;
        DWORD want = static_cast<DWORD>((n - done) < kIoBufferBytes ? (n - done) : kIoBufferBytes), got = 0;
        if (ok && (!ReadFile(f, buf, want, &got, nullptr) || got != want || !sha->Update(buf, got))) ok = false;
        done += got;
    }
    CountedFree(buf);
    return ok;
}
}  // namespace

bool FetchResumable(const std::string& url, const std::wstring& part, uint64_t size, const std::string& sha256,
                    const Hooks& h, std::string* error, std::string* detail) {
    auto fail = [&](const char* e, std::string d) {
        *error = e;
        *detail = std::move(d);
        return false;
    };
    uint64_t have = FileSize(part);
    if (have > size) {
        DeleteFileW(part.c_str());
        have = 0;
    }
    PartSink sink;
    sink.h = &h;
    sink.size = size;
    sink.f = CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sink.f == INVALID_HANDLE_VALUE) return fail("disk", "cannot create " + Narrow(part));
    sink.sha.emplace();
    if (have && h.phase) h.phase("verifying");  // hashing the kept part
    if (have && !HashPrefix(sink.f, have, &*sink.sha, h)) {
        if (h.cancel && h.cancel->load()) {
            CloseHandle(sink.f);
            return fail("cancelled", "cancelled");
        }
        // An unreadable kept part: start over.
        LARGE_INTEGER zero{};
        SetFilePointerEx(sink.f, zero, nullptr, FILE_BEGIN);
        SetEndOfFile(sink.f);
        sink.sha.emplace();
        have = 0;
    }
    sink.n = have;
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(have);
    SetFilePointerEx(sink.f, end, nullptr, FILE_BEGIN);
    SetEndOfFile(sink.f);

    bool ok = true;
    std::string why;
    if (have < size) {
        if (have && h.phase) h.phase("downloading");
        if (have) LOG_INFO("[media] resuming %s at %llu of %llu bytes", url.c_str(), have, size);
        store::fetch::Options o;
        o.cap = size;
        o.stallMs = 30000;
        o.cancel = h.cancel;
        o.registerActive = false;  // the Store's CancelActive must not stop the media download
        o.userAgent = h.userAgent;
        o.resumeFrom = have;
        ok = store::fetch::Get(url, sink, o, &why);
    }
    if (h.progress) h.progress(sink.n, size);
    CloseHandle(sink.f);
    if (!ok) {
        if (sink.ioerr) return fail("disk", "cannot write " + Narrow(part) + " (disk full?)");
        if (why == "cancelled") return fail("cancelled", "cancelled");
        // A refused range means the kept part is useless; anything else keeps it for the next try.
        if (why.find("HTTP 416") != std::string::npos || why.find("different range") != std::string::npos ||
            why.find("larger than") != std::string::npos)
            DeleteFileW(part.c_str());
        return fail("network", why);
    }
    if (h.phase) h.phase("verifying");
    const std::string actual = sink.sha->FinishHex();
    if (sink.n != size || actual != sha256) {
        DeleteFileW(part.c_str());
        return fail("hash", sink.n != size ? "size " + std::to_string(sink.n) + ", expected " + std::to_string(size)
                                           : "sha256 " + actual + ", expected " + sha256);
    }
    return true;
}

// ---------------------------------------------------------------- helpers
std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool MakeDirs(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2 && !MakeDirs(dir.substr(0, slash))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".new";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size() && FlushFileBuffers(f);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

int64_t UnixNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<int64_t>(t / 10000000ull) - 11644473600ll;
}
}  // namespace melange::media::pack::core
