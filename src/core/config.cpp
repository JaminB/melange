#include "core/config.h"

#include "core/config_schema.h"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace melange::config {
namespace {
std::wstring g_path;

std::wstring W(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// ---------------------------------------------------------------- the read cache
// Every GetPrivateProfileString opens, parses and closes Melange.ini. Mods read their settings from Lua on the main
// thread (Kindjal, Bloodsand, Sunstone and others re-read theirs every half second, dozens of keys a second), and on a
// loaded machine (a virus scan, another game copying files) the close of the ini can block in NtClose for seconds:
// two test copies hung 8 s at the same moment in config::GetString (HANG snapshots 2026-11-18, melange.asi+0x3b880 ->
// kernel32 BaseDllReadWriteIniFile -> ntdll NtClose). So values are read from the disk once and kept here. Writes
// through this module update the cache at once; any other change of the file (Oasis' ini editor, the launcher, a hand
// edit) is seen by a watcher thread, which compares the file's write time and size and drops the cache. Invalidate()
// is for writers in this process that need the new value on the very next read.
struct Stamp {
    FILETIME write{};
    DWORD sizeLo = 0, sizeHi = 0;
    bool ok = false;
    bool operator==(const Stamp& o) const {
        return ok == o.ok && sizeLo == o.sizeLo && sizeHi == o.sizeHi && write.dwLowDateTime == o.write.dwLowDateTime &&
               write.dwHighDateTime == o.write.dwHighDateTime;
    }
};

std::mutex g_mx;
std::unordered_map<std::wstring, std::optional<std::wstring>> g_cache;     // "section\nkey" (lower case) -> raw value
std::atomic<unsigned> g_gen{0};                                              // bumped on every invalidation
std::atomic<bool> g_cacheOn{false};
std::once_flag g_watchOnce;

Stamp StampOf(const std::wstring& path) {
    Stamp s;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) {
        s.write = a.ftLastWriteTime;
        s.sizeLo = a.nFileSizeLow;
        s.sizeHi = a.nFileSizeHigh;
        s.ok = true;
    }
    return s;
}

void DropAll() {
    std::lock_guard<std::mutex> lk(g_mx);
    g_cache.clear();
    ++g_gen;
}

void Watch(std::wstring path, Stamp last) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring dir = slash == std::wstring::npos ? L"." : path.substr(0, slash);
    HANDLE h = FindFirstChangeNotificationW(dir.c_str(), FALSE,
                                            FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME |
                                                FILE_NOTIFY_CHANGE_SIZE);
    for (;;) {
        // a change notice, or once a second anyway (a notice can be missed while the handle is being re-armed)
        DWORD r = WAIT_TIMEOUT;
        if (h != INVALID_HANDLE_VALUE) r = WaitForSingleObject(h, 1000);
        else Sleep(1000);
        const Stamp now = StampOf(path);
        if (!(now == last)) {
            last = now;
            DropAll();
        }
        if (r == WAIT_OBJECT_0 && !FindNextChangeNotification(h)) {
            FindCloseChangeNotification(h);
            h = INVALID_HANDLE_VALUE;
        }
    }
}

void StartCache() {
    if (g_path.empty()) return;
    std::call_once(g_watchOnce, [] {
        const Stamp first = StampOf(g_path);
        try {
            std::thread(Watch, g_path, first).detach();
            g_cacheOn = true;
        } catch (...) {
            // no thread: every read goes to the disk, as before
        }
    });
}

std::wstring CacheKey(const std::wstring& section, const std::wstring& key) {
    std::wstring k;
    k.reserve(section.size() + key.size() + 1);
    for (wchar_t c : section) k += static_cast<wchar_t>(towlower(c));
    k += L'\n';
    for (wchar_t c : key) k += static_cast<wchar_t>(towlower(c));
    return k;
}

// The raw value of a key, or nullopt when the ini has no such key.
std::optional<std::wstring> ReadRaw(const std::wstring& section, const std::wstring& key) {
    StartCache();
    const bool cached = g_cacheOn.load();
    std::wstring ck;
    unsigned gen = 0;
    if (cached) {
        ck = CacheKey(section, key);
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_cache.find(ck);
        if (it != g_cache.end()) return it->second;
        gen = g_gen.load();
    }
    wchar_t buf[1024];
    const DWORD n = GetPrivateProfileStringW(section.c_str(), key.c_str(), L"\x1", buf, 1024, g_path.c_str());
    std::optional<std::wstring> v;
    if (!(n == 1 && buf[0] == L'\x1')) v = std::wstring(buf, n);
    if (cached) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_gen.load() == gen) g_cache[ck] = v;      // not if the file changed while it was being read
    }
    return v;
}

void Forget(const std::wstring& section, const std::wstring& key) {
    if (!g_cacheOn.load()) return;
    std::lock_guard<std::mutex> lk(g_mx);
    g_cache.erase(CacheKey(section, key));
    ++g_gen;
}
}  // namespace

void Init(const std::wstring& iniPath) {
    g_path = iniPath;
    DropAll();
}
const std::wstring& Path() { return g_path; }

void Invalidate() { DropAll(); }

std::string GetString(const char* section, const char* key, const char* def) {
    const std::optional<std::wstring> raw = ReadRaw(W(section), W(key));
    const std::wstring wv = raw ? *raw : W(def);
    int n = WideCharToMultiByte(CP_UTF8, 0, wv.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, wv.c_str(), -1, s.data(), n, nullptr, nullptr);
    // strip inline comments ("value ; comment")
    if (auto p = s.find(';'); p != std::string::npos) s.resize(p);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

int GetInt(const char* section, const char* key, int def) {
    auto s = GetString(section, key, "");
    if (s.empty()) return def;
    return static_cast<int>(strtol(s.c_str(), nullptr, 0));
}

bool GetBool(const char* section, const char* key, bool def) { return GetInt(section, key, def ? 1 : 0) != 0; }

float GetFloat(const char* section, const char* key, float def) {
    auto s = GetString(section, key, "");
    return s.empty() ? def : static_cast<float>(atof(s.c_str()));
}

void EnsureKey(const char* section, const char* key, const char* def) {
    schema::Record(section, key, def);
    const std::wstring ws = W(section), wk = W(key);
    if (!ReadRaw(ws, wk)) {
        WritePrivateProfileStringW(ws.c_str(), wk.c_str(), W(def).c_str(), g_path.c_str());
        Forget(ws, wk);
    }
}

void SetString(const char* section, const char* key, const char* value) {
    const std::wstring ws = W(section), wk = W(key);
    WritePrivateProfileStringW(ws.c_str(), wk.c_str(), W(value ? value : "").c_str(), g_path.c_str());
    Forget(ws, wk);
}
}  // namespace melange::config
