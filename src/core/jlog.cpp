// Stub for component C (structured logging). Compiles and does nothing; C replaces this file.
// Public API: src/sdk/wumfix/jlog.h. See docs/m0-design.md §3 "C: structured logging, adapters and viewer".
//
// NOTE (added by the component D build, see its report): the stub as shipped by E declared the header
// but defined none of it, which is fine as long as nothing calls in - but D's exporter genuinely needs
// Flush(), FlushFromCrash(), CurrentSession() and RecentSessionDirs() to link and to do something honest
// before C lands (its own acceptance tests exercise the export against a real running writer). The
// bodies below are deliberately minimal and do not fake C's job:
//   - Enabled()/Tail()/GetStats() report "nothing is logged yet" rather than pretending to have records;
//   - Flush()/FlushFromCrash() are no-ops, because there is no queue yet to flush;
//   - CurrentSession() computes the session id/paths from the naming scheme in docs/m0-design.md §2.5
//     but does NOT create the directory, so it has no side effect beyond returning a name;
//   - RecentSessionDirs() genuinely scans disk under the configured root, so it will pick up whatever a
//     real C, from an earlier run, actually wrote, and returns nothing when none exists (matching D's
//     "no sessions found" acceptance path, §3 "D" acceptance item 7).
// C's real implementation replaces this entire file; when it does, these bodies disappear with it.
#include "wumfix/jlog.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <mutex>
#include <utility>

#pragma comment(lib, "shell32.lib")

namespace wf::jlog {
namespace {
std::wstring DocumentsRoot() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)) && path) {
        out = path;
        CoTaskMemFree(path);
    }
    return out.empty() ? L"." : out + L"\\WUMFix\\logs";
}

// "YYYY-MM-DD_HH-MM-SS_pid<N>", matching the scheme in §2.5's Session doc comment.
std::wstring MakeSessionId(SYSTEMTIME st, DWORD pid) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%04u-%02u-%02u_%02u-%02u-%02u_pid%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, pid);
    return buf;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
}  // namespace

bool Enabled(std::string_view, Level) { return false; }  // nothing produces records until C lands

const Session& CurrentSession() {
    static const Session s = [] {
        Session out;
        out.root = DocumentsRoot();
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::wstring id = MakeSessionId(st, GetCurrentProcessId());
        out.dir = out.root + L"\\" + id;
        out.id = Narrow(id);
        return out;
    }();
    return s;
}

std::vector<std::wstring> RecentSessionDirs(size_t max) {
    std::vector<std::pair<FILETIME, std::wstring>> found;
    std::wstring root = CurrentSession().root;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            found.emplace_back(fd.ftLastWriteTime, root + L"\\" + fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) {
        return CompareFileTime(&a.first, &b.first) > 0;  // newest first
    });
    std::vector<std::wstring> out;
    for (size_t i = 0; i < found.size() && i < max; ++i) out.push_back(found[i].second);
    return out;
}

bool Flush(uint32_t) { return true; }  // nothing queued yet
void FlushFromCrash() {}

size_t Tail(uint64_t, std::vector<Line>&, size_t) { return 0; }

Stats GetStats() { return {}; }
}  // namespace wf::jlog
