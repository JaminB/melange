#include "render/mirage/modfs.h"

#include <windows.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/log.h"
#include "melange/mods.h"
#include "mods/thumper_internal.h"

// Reimplemented for M2: an "enabled root" used to mean "every subfolder of Mods\ not named in
// [Mirage] DisabledMods" (M1). It now means "every mod Thumper's spice.json resolution says is active
// this session" (melange::thumper::ActiveRoots()) - Roots()/Resolve()/Watch() keep their exact M1
// signatures, so no caller (Mirage's shaders/postfx, B, C, D) needs to change. `disabled` from
// Configure() is no longer used for filtering: Thumper migrates it into thumper-state.json once, at
// Thumper's own Install() (src/mods/thumper.cpp), before this file's Configure() is even called.
namespace melange::mirage::modfs {
namespace {
struct Watcher {
    int handle;
    std::wstring subdir;
    ChangeFn fn;
    void* user;
};

std::mutex g_mx;
std::wstring g_modsDir;
bool g_configured = false;
std::vector<Watcher> g_watchers;
std::map<std::wstring, uint64_t> g_pending;
int g_nextHandle = 1;
bool g_threadStarted = false;
bool g_onChangeRegistered = false;

// Copies of Thumper's active-root strings, refreshed only when melange::mods::OnChange fires - so a
// Root pointer handed out by Roots()/Resolve() stays valid exactly as long as the header promises.
struct RootStorage {
    std::vector<std::string> ids;
    std::vector<std::wstring> dirs;
};
RootStorage g_roots;

void RefreshRootsLocked() {
    std::vector<melange::thumper::SessionRoot> active = melange::thumper::ActiveRoots();
    RootStorage next;
    next.ids.reserve(active.size());
    next.dirs.reserve(active.size());
    for (melange::thumper::SessionRoot& r : active) {
        next.ids.push_back(std::move(r.id));
        next.dirs.push_back(std::move(r.dir));
    }
    g_roots = std::move(next);
}

void OnModsChanged(void*) {
    std::lock_guard lk(g_mx);
    RefreshRootsLocked();
}

void EnsureOnChangeRegistered() {
    if (g_onChangeRegistered) return;
    g_onChangeRegistered = true;
    melange::mods::OnChange(&OnModsChanged, nullptr);
    RefreshRootsLocked();  // Thumper (Order 36) already resolved once before Mirage (Order 40) gets here.
}

// "<id>\<subdir>\..." relative to the mods folder.
bool SplitRel(const std::wstring& rel, std::wstring* id, std::wstring* sub) {
    size_t a = rel.find(L'\\');
    if (a == std::wstring::npos) return false;
    size_t b = rel.find(L'\\', a + 1);
    if (b == std::wstring::npos) return false;
    *id = rel.substr(0, a);
    *sub = rel.substr(a + 1, b - a - 1);
    return true;
}

bool IsManifestChange(const std::wstring& rel) {
    size_t a = rel.find(L'\\');
    if (a == std::wstring::npos) return false;
    std::wstring rest = rel.substr(a + 1);
    return rest.find(L'\\') == std::wstring::npos && _wcsicmp(rest.c_str(), L"spice.json") == 0;
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
        DWORD got = 0;
        while (ReadDirectoryChangesW(h, buf, sizeof(buf), TRUE,
                                     FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                         FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
                                     &got, nullptr, nullptr)) {
            uint64_t now = GetTickCount64();
            bool rescan = got == 0;  // overflow: safest is a full rescan
            std::vector<std::pair<std::wstring, uint64_t>> pend;
            if (got != 0) {
                auto* r = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf);
                for (;;) {
                    std::wstring rel(r->FileName, r->FileNameLength / sizeof(wchar_t));
                    if (rel.find(L'\\') == std::wstring::npos || IsManifestChange(rel)) rescan = true;
                    else pend.emplace_back(dir + L"\\" + rel, now);
                    if (!r->NextEntryOffset) break;
                    r = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(reinterpret_cast<uint8_t*>(r) + r->NextEntryOffset);
                }
            }
            {
                std::lock_guard lk(g_mx);
                for (auto& p : pend) g_pending[p.first] = p.second;
            }
            if (rescan) melange::thumper::Rescan();  // fires mods::OnChange, which refreshes g_roots
        }
        CloseHandle(h);
        Sleep(500);
    }
}
}  // namespace

void Configure(const std::wstring& modsDir, const std::string& /*disabled*/) {
    std::lock_guard lk(g_mx);
    g_modsDir = modsDir;
    while (!g_modsDir.empty() && (g_modsDir.back() == L'\\' || g_modsDir.back() == L'/')) g_modsDir.pop_back();
    g_configured = !g_modsDir.empty();
    EnsureOnChangeRegistered();
    // The watcher drives melange::thumper::Rescan() on any Mods\ change (folder add/remove, spice.json
    // edit), so it must run even if nothing ever calls Watch() for a shaders/effects subfolder.
    if (!g_threadStarted) {
        g_threadStarted = true;
        if (HANDLE t = CreateThread(nullptr, 0, &WatchThread, nullptr, 0, nullptr)) CloseHandle(t);
    }
}

int Roots(Root* out, int max) {
    std::lock_guard lk(g_mx);
    EnsureOnChangeRegistered();
    int n = 0;
    for (size_t i = 0; i < g_roots.ids.size(); ++i) {
        if (n >= max) break;
        if (out) out[n] = {g_roots.ids[i].c_str(), g_roots.dirs[i].c_str()};
        ++n;
    }
    return out ? n : static_cast<int>(g_roots.ids.size());
}

std::wstring Resolve(const wchar_t* relPath, const char** owner) {
    if (owner) *owner = "";
    if (!relPath) return {};
    std::lock_guard lk(g_mx);
    EnsureOnChangeRegistered();
    for (size_t i = g_roots.ids.size(); i-- > 0;) {
        std::wstring p = g_roots.dirs[i] + L"\\" + relPath;
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) {
            if (owner) *owner = g_roots.ids[i].c_str();
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
        if (g_pending.empty()) return;
        uint64_t now = GetTickCount64();
        size_t base = g_modsDir.size() + 1;
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            if (now - it->second < 300) {
                ++it;
                continue;
            }
            std::wstring id, sub;
            if (it->first.size() > base && SplitRel(it->first.substr(base), &id, &sub))
                for (const Watcher& w : g_watchers)
                    if (_wcsicmp(w.subdir.c_str(), sub.c_str()) == 0) calls.push_back({w.fn, w.user, it->first});
            it = g_pending.erase(it);
        }
    }
    for (const Call& c : calls) c.fn(c.path.c_str(), c.user);
}
}  // namespace melange::mirage::modfs
