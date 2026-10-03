#include "launcher/setup/running.h"

#include <windows.h>
#include <tlhelp32.h>

#include "launcher/util.h"
#include "oasis/standalone/game_lock.h"

namespace melange::launcher::setup {
bool MelangeLoaded(const std::wstring& dir) {
    if (dir.empty()) return false;
    return oasis::standalone::GameRunning(FullPath(dir));
}

bool GameRunning(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (MelangeLoaded(dir)) return true;
    const std::wstring want = PathKey(dir + L"\\WormsMayhem.exe");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"WormsMayhem.exe") != 0) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) {
            found = true;   // cannot tell where it runs from: assume this folder rather than write under a live game
            continue;
        }
        wchar_t buf[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(p, 0, buf, &n) && PathKey(std::wstring(buf, n)) == want) found = true;
        CloseHandle(p);
    }
    CloseHandle(snap);
    return found;
}
}  // namespace melange::launcher::setup
