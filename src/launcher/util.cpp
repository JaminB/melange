#include "launcher/util.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <cwctype>
#include <map>
#include <mutex>

#include "tools/hash.h"

namespace melange::launcher {
std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::wstring LowerW(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

bool IContains(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (hay.size() < needle.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i)
        if (IEquals(hay.substr(i, needle.size()), needle)) return true;
    return false;
}

std::wstring FullPath(const std::wstring& p) {
    if (p.empty()) return {};
    std::wstring in = p;
    for (auto& c : in)
        if (c == L'/') c = L'\\';
    const DWORD n = GetFullPathNameW(in.c_str(), 0, nullptr, nullptr);
    std::wstring out = in;
    if (n) {
        std::wstring buf(n, L'\0');
        const DWORD m = GetFullPathNameW(in.c_str(), n, buf.data(), nullptr);
        if (m && m < n) out.assign(buf.data(), m);
    }
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

std::wstring PathKey(const std::wstring& p) { return LowerW(FullPath(p)); }

bool PathInside(const std::wstring& path, const std::wstring& dir) {
    const std::wstring a = PathKey(path), d = PathKey(dir);
    if (d.empty() || a.size() < d.size() || a.compare(0, d.size(), d) != 0) return false;
    return a.size() == d.size() || a[d.size()] == L'\\' || d.back() == L'\\';
}

std::wstring Parent(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

std::wstring FileName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadAll(const std::wstring& p, std::string* out, size_t cap) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(f, &sz) && static_cast<uint64_t>(sz.QuadPart) <= cap;
    if (ok) {
        out->resize(static_cast<size_t>(sz.QuadPart));
        DWORD rd = 0;
        ok = out->empty() || (ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &rd, nullptr) && rd == out->size());
    }
    CloseHandle(f);
    return ok;
}

unsigned long WriteAtomic(const std::wstring& p, std::string_view data) {
    const std::wstring tmp = p + L".tmp-" + Widen(RandomHex(4));
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wr = 0;
    bool ok = data.empty() || (WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &wr, nullptr) && wr == data.size());
    unsigned long err = ok ? 0 : GetLastError();
    if (ok) FlushFileBuffers(f);
    CloseHandle(f);
    if (ok && !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) err = GetLastError();
    if (err) DeleteFileW(tmp.c_str());
    return err;
}

bool MakeDirs(const std::wstring& dir) {
    if (dir.empty() || DirExists(dir)) return true;
    const std::wstring parent = Parent(dir);
    if (!parent.empty() && parent != dir && !(parent.size() == 2 && parent[1] == L':')) MakeDirs(parent);
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

uint64_t FileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) return buf.substr(0, n);
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDir() { return Parent(ExePath()); }

std::wstring AppDataDir() {
    PWSTR p = nullptr;
    std::wstring out;
    wchar_t env[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"MELANGE_DATA_DIR", env, MAX_PATH);   // tests: keep launcher.json elsewhere
    if (n && n < MAX_PATH) {
        out = FullPath(env);
        MakeDirs(out);
        return out;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &p)) && p) out = std::wstring(p) + L"\\Melange";
    if (p) CoTaskMemFree(p);
    if (out.empty()) out = ExeDir() + L"\\Melange";
    MakeDirs(out);
    return out;
}

std::string Win32Message(unsigned long code) {
    wchar_t* buf = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                   nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring w = n && buf ? std::wstring(buf, n) : std::wstring();
    if (buf) LocalFree(buf);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r' || w.back() == L' ' || w.back() == L'.')) w.pop_back();
    if (w.empty()) return "Windows error " + std::to_string(code);
    return Narrow(w) + ".";
}

std::string NowIsoUtc() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char b[32];
    snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string StampLocal() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[32];
    snprintf(b, sizeof b, "%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::string RandomHex(int bytes) {
    unsigned char b[32]{};
    if (bytes > 32) bytes = 32;
    BCryptGenRandom(nullptr, b, static_cast<ULONG>(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    std::string s;
    char h[3];
    for (int i = 0; i < bytes; ++i) {
        snprintf(h, sizeof h, "%02x", b[i]);
        s += h;
    }
    return s;
}
}  // namespace melange::launcher

namespace melange::launcher {
std::string Sha256Cached(const std::wstring& path) {
    static std::mutex mx;
    static std::map<std::wstring, std::pair<std::string, std::string>> cache;   // key -> (size.mtime, sha)
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return {};
    char stamp[64];
    snprintf(stamp, sizeof stamp, "%lx.%lx.%lx.%lx", fa.nFileSizeHigh, fa.nFileSizeLow, fa.ftLastWriteTime.dwHighDateTime,
             fa.ftLastWriteTime.dwLowDateTime);
    const std::wstring key = PathKey(path);
    {
        std::lock_guard lk(mx);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.first == stamp) return it->second.second;
    }
    const std::string sha = hashutil::Sha256HexFile(path);
    if (!sha.empty()) {
        std::lock_guard lk(mx);
        if (cache.size() > 256) cache.clear();
        cache[key] = {stamp, sha};
    }
    return sha;
}
}  // namespace melange::launcher
