#include "render/mirage/modfs.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/log.h"

namespace melange::mirage::modfs {
namespace {
struct Entry {
    std::string id;
    std::wstring dir;
};
struct Watcher {
    int handle;
    std::wstring subdir;
    ChangeFn fn;
    void* user;
};

constexpr size_t kMaxGenerations = 8;

std::mutex g_mx;
std::wstring g_modsDir;
std::vector<std::string> g_disabled;
// Old generations stay alive so pointers handed out by Roots() never dangle; every caller (Scan()/Watch() users)
// copies id/dir out immediately rather than holding them across a rescan, so trimming to a handful is safe.
std::vector<std::unique_ptr<std::vector<Entry>>> g_generations;
std::vector<Entry> g_emptyRoots;
std::vector<Entry>* g_roots = &g_emptyRoots;
bool g_scanned = false;
bool g_configured = false;  // Configure() was called with a non-empty mods dir
std::atomic<bool> g_dirty{false};
std::vector<Watcher> g_watchers;
std::map<std::wstring, uint64_t> g_pending;  // path -> tick of the last change
int g_nextHandle = 1;
bool g_threadStarted = false;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsDisabled(const std::string& id) {
    std::string l = Lower(id);
    return std::find(g_disabled.begin(), g_disabled.end(), l) != g_disabled.end();
}

void ScanLocked() {
    auto list = std::make_unique<std::vector<Entry>>();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_modsDir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            std::string id = game::Narrow(fd.cFileName);
            if (IsDisabled(id)) continue;
            list->push_back({id, g_modsDir + L"\\" + fd.cFileName});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(list->begin(), list->end(), [](const Entry& a, const Entry& b) { return _stricmp(a.id.c_str(), b.id.c_str()) < 0; });
    g_roots = list.get();
    g_generations.push_back(std::move(list));
    if (g_generations.size() > kMaxGenerations) g_generations.erase(g_generations.begin(), g_generations.end() - kMaxGenerations);
    g_scanned = true;
}

void EnsureScanned() {
    // Without a real mods dir, FindFirstFileW(L"\\*") would list the current drive's root as if every top-level
    // folder (Windows, Program Files, ...) were a mod.
    if (!g_configured) return;
    if (!g_scanned || g_dirty.exchange(false)) ScanLocked();
}

// "<id>\<subdir>\..." relative to the mods folder
bool SplitRel(const std::wstring& rel, std::wstring* id, std::wstring* sub) {
    size_t a = rel.find(L'\\');
    if (a == std::wstring::npos) return false;
    size_t b = rel.find(L'\\', a + 1);
    if (b == std::wstring::npos) return false;
    *id = rel.substr(0, a);
    *sub = rel.substr(a + 1, b - a - 1);
    return true;
}

DWORD WINAPI WatchThread(void*) {
    alignas(DWORD) static uint8_t buf[64 * 1024];
    for (;;) {
        std::wstring dir;
        {
            std::lock_guard lk(g_mx);
            if (g_configured) dir = g_modsDir;
        }
        if (dir.empty()) {
            Sleep(2000);
            continue;
        }
        HANDLE h = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }
        g_dirty = true;
        DWORD got = 0;
        while (ReadDirectoryChangesW(h, buf, sizeof(buf), TRUE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                     &got, nullptr, nullptr)) {
            uint64_t now = GetTickCount64();
            std::lock_guard lk(g_mx);
            if (got == 0) {  // overflow: rescan, nothing precise to report
                g_dirty = true;
                continue;
            }
            for (auto* r = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf);;
                 r = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<uint8_t*>(r) + r->NextEntryOffset)) {
                std::wstring rel(r->FileName, r->FileNameLength / sizeof(wchar_t));
                if (rel.find(L'\\') == std::wstring::npos) g_dirty = true;
                else g_pending[dir + L"\\" + rel] = now;
                if (!r->NextEntryOffset) break;
            }
        }
        CloseHandle(h);
        Sleep(500);
    }
}
}  // namespace

void Configure(const std::wstring& modsDir, const std::string& disabled) {
    std::lock_guard lk(g_mx);
    g_modsDir = modsDir;
    while (!g_modsDir.empty() && (g_modsDir.back() == L'\\' || g_modsDir.back() == L'/')) g_modsDir.pop_back();
    g_configured = !g_modsDir.empty();
    g_disabled.clear();
    size_t p = 0;
    while (p <= disabled.size()) {
        size_t q = disabled.find(',', p);
        if (q == std::string::npos) q = disabled.size();
        std::string s = disabled.substr(p, q - p);
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
        if (!s.empty()) g_disabled.push_back(Lower(s));
        p = q + 1;
    }
    g_scanned = false;
}

int Roots(Root* out, int max) {
    std::lock_guard lk(g_mx);
    EnsureScanned();
    int n = 0;
    for (const Entry& e : *g_roots) {
        if (n >= max) break;
        if (out) out[n] = {e.id.c_str(), e.dir.c_str()};
        ++n;
    }
    return out ? n : static_cast<int>(g_roots->size());
}

std::wstring Resolve(const wchar_t* relPath, const char** owner) {
    if (owner) *owner = "";
    if (!relPath) return {};
    std::lock_guard lk(g_mx);
    EnsureScanned();
    for (auto it = g_roots->rbegin(); it != g_roots->rend(); ++it) {
        std::wstring p = it->dir + L"\\" + relPath;
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            if (owner) *owner = it->id.c_str();
            return p;
        }
    }
    return {};
}

int Watch(const wchar_t* subdir, ChangeFn fn, void* user) {
    if (!subdir || !fn) return 0;
    std::lock_guard lk(g_mx);
    int h = g_nextHandle++;
    g_watchers.push_back({h, subdir, fn, user});
    if (!g_threadStarted) {
        g_threadStarted = true;
        if (HANDLE t = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr)) CloseHandle(t);
        LOG_INFO("[mirage] watching %s for mod changes", game::Narrow(g_modsDir).c_str());
    }
    return h;
}

void OnFrame() {
    struct Call {
        ChangeFn fn;
        void* user;
        std::wstring path;
    };
    std::vector<Call> calls;
    {
        std::lock_guard lk(g_mx);
        if (g_scanned && g_dirty.exchange(false)) ScanLocked();
        if (g_pending.empty()) return;
        uint64_t now = GetTickCount64();
        size_t base = g_modsDir.size() + 1;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (now - it->second < 300) {
                ++it;
                continue;
            }
            std::wstring id, sub;
            if (it->first.size() > base && SplitRel(it->first.substr(base), &id, &sub) && !IsDisabled(game::Narrow(id)))
                for (const Watcher& w : g_watchers)
                    if (_wcsicmp(w.subdir.c_str(), sub.c_str()) == 0) calls.push_back({w.fn, w.user, it->first});
            it = g_pending.erase(it);
        }
    }
    for (const Call& c : calls) c.fn(c.path.c_str(), c.user);
}
}  // namespace melange::mirage::modfs
