#pragma once
// Where crash and hang minidumps go, without the game: the Diagnostics module writes them, the log export (in the game
// and in Melange.exe) collects them, and the self-test checks the order. Header-only so both sides share it.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace melange::debug {
// The folders a minidump is tried in, in order: <data dir>\dumps (next to melange.asi), then
// <Documents>\Melange\dumps for when the game folder is not writable (a Program Files install). Empty inputs are
// skipped and a folder named twice (the data dir already under Documents) is listed once.
inline std::vector<std::wstring> DumpDirs(const std::wstring& dataDir, const std::wstring& documentsDir) {
    std::vector<std::wstring> out;
    auto add = [&out](std::wstring base, const wchar_t* sub) {
        while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) base.pop_back();
        if (base.empty()) return;
        const std::wstring d = base + sub;
        for (const auto& o : out)
            if (_wcsicmp(o.c_str(), d.c_str()) == 0) return;
        out.push_back(d);
    };
    add(dataDir, L"\\dumps");
    add(documentsDir, L"\\Melange\\dumps");
    return out;
}

// <YYYYMMDD_HHMMSS>_<tag>[-full].dmp. The log exporter relies on the "-full" suffix to tell full-memory dumps apart.
inline std::wstring DumpFileName(const SYSTEMTIME& st, const char* tag, bool full) {
    wchar_t name[144];
    swprintf(name, 144, L"%04u%02u%02u_%02u%02u%02u_%S%s.dmp", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, tag ? tag : "dump", full ? L"-full" : L"");
    return name;
}
}  // namespace melange::debug
