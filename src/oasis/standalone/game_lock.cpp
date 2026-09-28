#include "oasis/standalone/game_lock.h"

#include <windows.h>

#include <cwctype>

namespace melange::oasis::standalone {
bool GameRunning(const std::wstring& gameDir) {
    std::wstring dir = gameDir;
    for (auto& c : dir) c = static_cast<wchar_t>(towlower(c));
    uint32_t h = 2166136261u;
    for (wchar_t c : dir) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    wchar_t name[64];
    swprintf(name, 64, L"Local\\Melange-%08x", h);
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, name);
    if (!m) return false;
    CloseHandle(m);
    return true;
}
}  // namespace melange::oasis::standalone
