// The helper's cache folder lifecycle: the instance lock, the start sweep and a delete that never follows links.
#include "bt/bt_cache.h"

#include <windows.h>

#include <cstdio>
#include <vector>

#include "bt/bt_logic.h"

namespace holo::bt::cache {
namespace {
constexpr int kMaxDepth = 64;                           // torrent paths are far shallower; deeper trees are left alone
constexpr unsigned long long kYoungFolderMs = 600000;  // 10 minutes

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    const wchar_t last = a.back();
    return (last == L'\\' || last == L'/') ? a + b : a + L'\\' + b;
}
std::wstring StripSlash(std::wstring s) {
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}
// \\?\ form so torrent paths longer than MAX_PATH can still be deleted.
std::wstring Long(const std::wstring& p) {
    if (p.rfind(L"\\\\?\\", 0) == 0 || p.size() < 3 || p[1] != L':') return p;
    std::wstring s = L"\\\\?\\" + p;
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

bool DeleteTreeAt(const std::wstring& dir, int depth) {
    if (depth > kMaxDepth) return false;
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(Long(Join(dir, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring p = Long(Join(dir, name));
            const DWORD a = fd.dwFileAttributes;
            if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
            if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
                // The link itself goes; its target is never entered.
                ok = ((a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(p.c_str()) : DeleteFileW(p.c_str())) && ok;
            } else if (a & FILE_ATTRIBUTE_DIRECTORY) {
                ok = DeleteTreeAt(Join(dir, name), depth + 1) && ok;
            } else {
                ok = DeleteFileW(p.c_str()) && ok;
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (!RemoveDirectoryW(Long(dir).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND &&
        GetLastError() != ERROR_PATH_NOT_FOUND)
        ok = false;
    return ok;
}

unsigned long long NowMs() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}
unsigned long long Ms(const FILETIME& ft) {
    return ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000;
}

// True when the folder belongs to no live helper: its lock opens for delete, or it has none and is not young. The
// lock comes back held (no sharing, delete on close) in *held, so no helper can start in the folder while it goes.
bool OwnerGone(const std::wstring& folder, const FILETIME& created, HANDLE* held) {
    *held = INVALID_HANDLE_VALUE;
    const std::wstring lock = Long(Join(folder, L"lock"));
    HANDLE h = CreateFileW(lock.c_str(), DELETE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        *held = h;
        return true;
    }
    const DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) return NowMs() - Ms(created) > kYoungFolderMs;
    return false;  // sharing violation (owner alive), access denied, anything else: leave it
}
}  // namespace

std::wstring TorrentDir(const std::wstring& cacheDir, const std::string& ih) {
    return Join(Join(StripSlash(cacheDir), L"bt"), std::wstring(ih.begin(), ih.end()));
}

bool DeleteTree(const std::wstring& dir) {
    const std::wstring d = StripSlash(dir);
    const DWORD a = GetFileAttributesW(Long(d).c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return true;
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return RemoveDirectoryW(Long(d).c_str()) || DeleteFileW(Long(d).c_str());
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(Long(d).c_str()) != 0;
    return DeleteTreeAt(d, 0);
}

bool HoldInstanceLock(const std::wstring& cacheDir, std::string* err) {
    static HANDLE s_lock = INVALID_HANDLE_VALUE;  // held until the process exits (no close, no join at exit)
    if (s_lock != INVALID_HANDLE_VALUE) return true;
    const std::wstring dir = StripSlash(cacheDir);
    if (!CreateDirectoryW(Long(dir).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        if (err) *err = "cannot create the cache folder (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    const DWORD a = GetFileAttributesW(Long(dir).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
        if (err) *err = "the cache folder is not a plain folder";
        return false;
    }
    HANDLE h = CreateFileW(Long(Join(dir, L"lock")).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = "cannot hold the cache lock (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    char pid[32];
    const int n = snprintf(pid, sizeof pid, "%lu\n", GetCurrentProcessId());
    DWORD w = 0;
    SetFilePointer(h, 0, nullptr, FILE_BEGIN);
    WriteFile(h, pid, static_cast<DWORD>(n), &w, nullptr);
    SetEndOfFile(h);
    s_lock = h;
    return true;
}

int SweepInstances(const std::wstring& cacheDir) {
    const std::wstring self = StripSlash(cacheDir);
    const size_t cut = self.find_last_of(L"\\/");
    if (cut == std::wstring::npos || cut < 3) return 0;
    const std::wstring parent = self.substr(0, cut), selfName = self.substr(cut + 1);
    std::vector<std::pair<std::wstring, FILETIME>> gone;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(Long(Join(parent, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring name = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (_wcsicmp(name.c_str(), selfName.c_str()) == 0 || !logic::InstanceFolderName(name)) continue;
        gone.emplace_back(name, fd.ftCreationTime);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    int n = 0;
    for (const auto& [name, created] : gone) {
        const std::wstring p = Join(parent, name);
        HANDLE held;
        if (!OwnerGone(p, created, &held)) continue;
        DeleteTree(p);  // everything but the held lock
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);  // deletes the lock
        if (DeleteTree(p)) ++n;
    }
    return n;
}

int SweepTorrents(const std::wstring& cacheDir) {
    const std::wstring bt = Join(StripSlash(cacheDir), L"bt");
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(Long(Join(bt, L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        names.push_back(name);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    int n = 0;
    for (const std::wstring& name : names) {
        // Only infohash folders, plus libtorrent's stray part files; anything else in bt\ is not ours to judge.
        if (logic::TorrentFolderName(name) && DeleteTree(Join(bt, name))) ++n;
    }
    return n;
}
}  // namespace holo::bt::cache
