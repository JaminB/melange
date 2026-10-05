// The log export's game-independent core (tools/log_export_core.h): file selection, redaction, the zip.
#include "tools/log_export_core.h"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <exception>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")  // FOLDERID_*

#include "core/log.h"
#include "miniz.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"
#include "tools/redact.h"
#include "tools/sysinfo.h"
#include "version.h"
#include "wormsign/library.h"

namespace melange::exporter::core {
namespace {

constexpr uint64_t kSecond = 10'000'000ull;  // FILETIME ticks
constexpr uint64_t kMinute = 60 * kSecond;
constexpr uint64_t kCap = 64ull * 1024 * 1024;           // per file
constexpr uint64_t kFullDumpCap = 256ull * 1024 * 1024;
constexpr uint64_t kReplayBudget = 192ull * 1024 * 1024; // all of one game's replays together (the zip is built in memory)

std::string Narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool HasSuffixCI(const std::wstring& s, const wchar_t* suffix) {
    size_t n = wcslen(suffix);
    if (s.size() < n) return false;
    return _wcsicmp(s.c_str() + (s.size() - n), suffix) == 0;
}

bool IsTextExtension(const std::wstring& name) {
    for (const wchar_t* ext : {L".log", L".jsonl", L".ini", L".json", L".txt"})
        if (HasSuffixCI(name, ext)) return true;
    return false;
}

std::wstring BaseNameW(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

bool DirExists(const std::wstring& p) {
    const DWORD a = p.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDirectoryRecursive(const std::wstring& dir) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    size_t pos = dir.find_first_of(L"\\/", 3);  // skip past a drive letter ("C:\")
    while (pos != std::wstring::npos) {
        CreateDirectoryW(dir.substr(0, pos).c_str(), nullptr);
        pos = dir.find_first_of(L"\\/", pos + 1);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

bool DirWritable(const std::wstring& dir) {
    if (!DirExists(dir)) return false;
    // Hidden and deleted on close: the probe never shows up as an icon on the Desktop.
    const std::wstring probe = dir + L"\\.melange_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

uint64_t Ticks(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

uint64_t NowUtc() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return Ticks(ft);
}

bool LocalToUtcTicks(const SYSTEMTIME& local, uint64_t* out) {
    SYSTEMTIME utc{};
    FILETIME ft{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return false;
    *out = Ticks(ft);
    return true;
}

std::string IsoUtc(uint64_t ticks) {
    FILETIME ft{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    SYSTEMTIME st{};
    if (!ticks || !FileTimeToSystemTime(&ft, &st)) return {};
    char ts[32];
    snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    return ts;
}

// Fixed-width decimal field at `pos`; false if any character isn't a digit.
bool Digits(std::wstring_view s, size_t pos, size_t n, unsigned* out) {
    if (pos + n > s.size()) return false;
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!iswdigit(s[pos + i])) return false;
        v = v * 10 + static_cast<unsigned>(s[pos + i] - L'0');
    }
    *out = v;
    return true;
}

// 1..10 digits from `pos` up to the first non-digit; `*end` is that position.
bool Pid(std::wstring_view s, size_t pos, uint32_t* pid, size_t* end) {
    uint64_t v = 0;
    size_t i = pos;
    while (i < s.size() && iswdigit(s[i]) && i - pos < 10) v = v * 10 + static_cast<uint64_t>(s[i++] - L'0');
    if (i == pos || v > 0xFFFFFFFFull || (i < s.size() && iswdigit(s[i]))) return false;
    *pid = static_cast<uint32_t>(v);
    *end = i;
    return true;
}

bool MakeTime(unsigned y, unsigned mo, unsigned d, unsigned h, unsigned mi, unsigned s, uint64_t* utc) {
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 59) return false;
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(mo);
    st.wDay = static_cast<WORD>(d);
    st.wHour = static_cast<WORD>(h);
    st.wMinute = static_cast<WORD>(mi);
    st.wSecond = static_cast<WORD>(s);
    return LocalToUtcTicks(st, utc);
}

struct FileInfo {
    std::wstring path;
    uint64_t mtime = 0;
};

// Plain files in `dir` matching `pattern`, newest first.
std::vector<FileInfo> ListFiles(const std::wstring& dir, const wchar_t* pattern) {
    std::vector<FileInfo> found;
    if (dir.empty()) return found;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        found.push_back({dir + L"\\" + fd.cFileName, Ticks(fd.ftLastWriteTime)});
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(), [](const FileInfo& a, const FileInfo& b) { return a.mtime > b.mtime; });
    return found;
}

std::vector<std::wstring> Newest(const std::wstring& dir, const wchar_t* pattern, size_t max) {
    std::vector<std::wstring> out;
    for (const auto& f : ListFiles(dir, pattern)) {
        if (out.size() >= max) break;
        out.push_back(f.path);
    }
    return out;
}

// Reads the whole file, or only its last capBytes if larger (the newest part matters most).
// Full sharing so files the game or the log writer still has open can be read.
bool ReadCapped(const std::wstring& path, uint64_t capBytes, bool fromHead, std::string& out, bool* truncated) {
    *truncated = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size)) {
        CloseHandle(f);
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(size.QuadPart);
    uint64_t toRead = total;
    if (total > capBytes) {
        *truncated = true;
        toRead = capBytes;
        if (!fromHead) {
            LARGE_INTEGER seekTo{};
            seekTo.QuadPart = static_cast<LONGLONG>(total - capBytes);
            SetFilePointerEx(f, seekTo, nullptr, FILE_BEGIN);
        }
    }
    out.resize(static_cast<size_t>(toRead));
    size_t got = 0;
    while (got < out.size()) {
        DWORD chunk = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - got, 1u << 22));
        if (!ReadFile(f, out.data() + got, want, &chunk, nullptr) || chunk == 0) break;
        got += chunk;
    }
    out.resize(got);
    CloseHandle(f);
    return true;
}

uint64_t FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

// Zip and manifest

struct ManifestEntry {
    std::string archivePath;
    uint64_t size = 0;    // bytes in the zip, after redaction/truncation
    std::string sha256;   // of those bytes
    std::string source;   // original path, redacted
    bool truncated = false;
};

class ZipBuilder {
public:
    ZipBuilder() { mz_zip_writer_init_heap(&zip_, 0, 1 << 20); }
    ~ZipBuilder() { mz_zip_writer_end(&zip_); }
    ZipBuilder(const ZipBuilder&) = delete;

    bool Add(const std::string& archivePath, const void* data, size_t n) {
        return mz_zip_writer_add_mem(&zip_, archivePath.c_str(), data, n, 6 /* deflate level */) != 0;
    }
    // The builder must not be used after this.
    bool Finalize(std::string* outBytes) {
        void* buf = nullptr;
        size_t sz = 0;
        if (!mz_zip_writer_finalize_heap_archive(&zip_, &buf, &sz)) return false;
        outBytes->assign(static_cast<const char*>(buf), sz);
        mz_free(buf);
        return true;
    }

private:
    mz_zip_archive zip_{};
};

// The computer name appears in engine log names (XOM0-<NAME>.log) and contents, so it is always replaced.
const std::string& ComputerName() {
    static const std::string name = [] {
        wchar_t buf[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        return GetComputerNameW(buf, &n) ? Narrow(buf) : std::string();
    }();
    return name;
}

// Profile folder name. After an account rename it differs from the account name, so both are redacted.
const std::string& ProfileFolderName() {
    static const std::string name = Narrow(BaseNameW(KnownFolder(Folder::Profile)));
    return name;
}

std::string CurrentUserName() {
    wchar_t buf[256];
    DWORD n = 256;
    if (GetUserNameW(buf, &n)) return Narrow(buf);
    return {};
}

// One export in progress: the zip, its manifest and the redaction every entry goes through.
class Collector {
public:
    explicit Collector(bool redactUserPaths)
        : redactUserPaths_(redactUserPaths), salt_(melange::hashutil::RandomSalt()), userName_(CurrentUserName()) {}

    std::vector<ManifestEntry> manifest;
    std::vector<std::string> absent;
    ZipBuilder zip;

    std::string ExcludeComputerName(std::string_view text) const {
        return melange::redact::ReplaceName(text, ComputerName(), "%COMPUTERNAME%");
    }
    std::string RedactUserNames(std::string_view text) const {
        std::string out = melange::redact::RedactUserName(text, userName_);
        const std::string& profile = ProfileFolderName();
        if (!profile.empty()) out = melange::redact::RedactUserName(out, profile);
        return out;
    }

    void Add(const std::string& archivePathIn, std::string data, const std::wstring& sourcePath, bool truncated,
             bool isText) {
        if (isText) {
            // Generated entries hold no ids, only version numbers the IPv4 pattern would hash.
            if (sourcePath != L"(generated)") data = melange::redact::HashIdsAndIps(data, salt_);
            if (redactUserPaths_) data = RedactUserNames(data);
            data = ExcludeComputerName(data);
        }
        const std::string archivePath = ExcludeComputerName(archivePathIn);
        // Never list an entry in the manifest that failed to go into the zip.
        if (!zip.Add(archivePath, data.data(), data.size())) {
            LOG_WARN("[LogExport] could not add zip entry '%s' (%zu bytes); omitted from the export", archivePath.c_str(),
                     data.size());
            return;
        }
        ManifestEntry e;
        e.archivePath = archivePath;
        e.size = data.size();
        e.sha256 = melange::hashutil::Sha256Hex(data.data(), data.size());
        e.source = Narrow(sourcePath);
        if (redactUserPaths_) e.source = RedactUserNames(e.source);
        e.source = ExcludeComputerName(e.source);
        e.truncated = truncated;
        manifest.push_back(std::move(e));
    }

    bool AddFile(const std::string& archivePath, const std::wstring& sourcePath, uint64_t capBytes = kCap) {
        std::string data;
        bool truncated = false;
        if (!ReadCapped(sourcePath, capBytes, false, data, &truncated)) return false;
        bool isText = IsTextExtension(sourcePath);
        if (truncated && isText) data = "...[truncated, showing only the end of a larger file]...\n" + data;
        const size_t before = manifest.size();
        Add(archivePath, std::move(data), sourcePath, truncated, isText);
        return manifest.size() > before;
    }

    void AddGenerated(const std::string& archivePath, const std::string& data, bool isText) {
        Add(archivePath, data, L"(generated)", false, isText);
    }

    // A .wsr recording can carry a peer's raw SteamID (its DVRG chunk) and the case is exactly the one this export
    // exists for, so it gets the same redaction as everything else here rather than going in verbatim: rewritten
    // through the library's redactor (SteamID-shaped numbers and IPs salted-hashed, the Windows user name replaced)
    // into a temp file, then capped from its start so the HEAD chunk a reader needs survives the cap.
    bool AddRedactedWsr(const std::string& archivePath, const std::wstring& sourcePath) {
        wchar_t tmpDir[MAX_PATH], tmpPath[MAX_PATH];
        if (!GetTempPathW(MAX_PATH, tmpDir) || !GetTempFileNameW(tmpDir, L"wsx", 0, tmpPath)) return false;
        std::string err;
        if (!melange::wormsign::library::ExportRedacted(sourcePath, tmpPath, &err, salt_)) {
            DeleteFileW(tmpPath);
            return false;
        }
        std::string data;
        bool truncated = false;
        const bool ok = ReadCapped(tmpPath, kCap, true, data, &truncated);
        DeleteFileW(tmpPath);
        if (!ok) return false;
        Add(archivePath, std::move(data), sourcePath, truncated, /*isText=*/false);
        return true;
    }

private:
    bool redactUserPaths_;
    std::string salt_;
    std::string userName_;
};

std::string BuildReadme(Scope scope) {
    std::string s;
    s += "Melange logs export\n";
    s += "Melange " MELANGE_VERSION "\n\n";
    if (scope == Scope::LastGame)
        s += "One game: the session, recordings and desync bundles of the game named in manifest.json.\n\n";
    s += "This zip contains:\n";
    s += "  README.txt        this file\n";
    s += "  manifest.json     every file below, its size and SHA-256 checksum, and which game was exported\n";
    s += "  system.json       OS, CPU, GPU/GL and exe identification\n";
    s += "  gpu/compat.*      GPU compatibility report (only when exported from inside the game)\n";
    s += "  logs/sessions/*   structured JSONL event logs\n";
    s += "  logs/Melange.log, logs/Melange.prev.log   the plain text log\n";
    s += "  logs/engine/*     the engine's own XOM/Net log files, if found\n";
    s += "  logs/launcher/*   Melange.exe's own log, if found\n";
    s += "  dumps/*.dmp       crash/hang minidumps, if any were found\n";
    s += "  config/*.ini      Melange.ini and any other .ini next to the game exe\n";
    s += "  mods/*.json       installed mods (spice.json ids and versions, enabled state), modules and .asi plugins\n";
    s += "  replays/*         desync bundles and match recordings, if any\n\n";
    s += "Privacy note - this zip can contain:\n";
    s += "  - your Windows user name in file paths (replaced with %USERNAME% by default)\n";
    s += "  - Steam ids, persona names and lobby ids (ids are replaced with a short hash unique to\n";
    s += "    this export; persona names in chat/trace text are not touched)\n";
    s += "  - IP addresses and ports (replaced the same way as Steam ids)\n";
    s += "  - chat text, if chat logging was on\n";
    s += "  - crash minidumps, which contain parts of the game's memory\n";
    s += "Your computer name is replaced with %COMPUTERNAME% in text files and file names.\n\n";
    s += "Nothing here is uploaded anywhere. This zip is only written to your own computer.\n";
    return s;
}

struct Window {
    bool known = false;
    uint64_t start = 0, end = 0;
    bool Contains(uint64_t t, uint64_t before, uint64_t after) const {
        return known && t + before >= start && t <= end + after;
    }
    uint64_t Distance(uint64_t t) const {
        if (t < start) return start - t;
        return t > end ? t - end : 0;
    }
};

// The data dir (<folder of melange.asi>\Melange) whose Melange.log was written last.
std::wstring PickDataDir(const std::vector<std::wstring>& dirs) {
    std::wstring best;
    uint64_t bestTime = 0;
    for (const auto& d : dirs) {
        auto f = ListFiles(d, L"Melange.log");
        if (!f.empty() && (best.empty() || f[0].mtime > bestTime)) {
            best = d;
            bestTime = f[0].mtime;
        } else if (best.empty() && DirExists(d)) {
            best = d;  // no log yet, but its dumps\ may still matter
        }
    }
    return best;
}

std::string SpiceListing(const std::wstring& gameDir) {
    jsonmini::Arr arr;
    const std::wstring mods = gameDir + L"\\Mods";
    WIN32_FIND_DATAW fd{};
    HANDLE h = gameDir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((mods + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return arr.End();
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        json::Value v;
        json::Error e;
        jsonmini::Obj o;
        o.Str("dir", Narrow(fd.cFileName));
        if (!json::ParseFile(mods + L"\\" + fd.cFileName + L"\\spice.json", &v, &e) || !v.IsObject()) {
            o.Bool("spiceJson", false);
        } else {
            for (const char* k : {"id", "name", "version", "kind"})
                if (const json::Value* s = v.Get(k); s && s->IsString()) o.Str(k, s->string);
        }
        arr.Raw(o.End());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return arr.End();
}

bool DoExport(const std::wstring& zipPath, const Request& rq, Result* res) {
    const Options& opt = rq.opt;
    const Sources& src = rq.src;
    if (rq.prov.beforeCollect) rq.prov.beforeCollect();

    Collector c(opt.redactUserPaths);
    std::vector<std::string> sessionIds;
    const std::wstring dataDir = PickDataDir(src.dataDirs);

    // Which game: the chosen session folder and its time window.
    SessionDir chosen;
    bool haveSession = false, live = false;
    Window win;
    uint32_t pid = 0;
    if (rq.scope == Scope::LastGame) {
        if (!src.currentSessionDir.empty() && DirExists(src.currentSessionDir) &&
            ParseSessionName(BaseNameW(src.currentSessionDir), &chosen.pid, &chosen.startUtc)) {
            chosen.path = src.currentSessionDir;
            chosen.id = Narrow(BaseNameW(src.currentSessionDir));
            haveSession = live = true;
        } else if (src.currentPid == 0) {
            const auto all = FindSessions(src.sessionRoots);
            if (!all.empty()) {
                chosen = all.front();
                haveSession = true;
            }
        }
        pid = haveSession ? chosen.pid : src.currentPid;
        if (haveSession) {
            win.known = true;
            win.start = chosen.startUtc;
            win.end = chosen.startUtc;
            if (live) {
                win.end = NowUtc();
            } else {
                for (const auto& f : ListFiles(chosen.path, L"*")) win.end = std::max(win.end, f.mtime);
            }
        } else if (src.currentPid != 0 && src.currentPid == GetCurrentProcessId()) {
            // In the game with session logging off: the process's own lifetime is the window.
            FILETIME created{}, exited{}, kernel{}, user{};
            if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
                win.known = live = true;
                win.start = Ticks(created);
                win.end = NowUtc();
            }
        }
        if (!haveSession) c.absent.push_back("logs/sessions (no session folder for this game)");
        res->sessionId = chosen.id;
        res->pid = pid;
    }

    // logs/sessions/<id>/events*.jsonl
    if (rq.scope == Scope::LastGame) {
        if (haveSession) {
            sessionIds.push_back(chosen.id);
            for (const auto& f : Newest(chosen.path, L"events*.jsonl", 64))
                c.AddFile("logs/sessions/" + chosen.id + "/" + Narrow(BaseNameW(f)), f);
        }
    } else {
        if (!src.currentSessionDir.empty()) sessionIds.push_back(Narrow(BaseNameW(src.currentSessionDir)));
        const auto all = FindSessions(src.sessionRoots);
        const size_t n = static_cast<size_t>(std::max(1, opt.sessions));
        if (all.empty()) c.absent.push_back("logs/sessions (no session folders found)");
        for (size_t i = 0; i < all.size() && i < n; ++i) {
            const auto& s = all[i];
            if (std::find(sessionIds.begin(), sessionIds.end(), s.id) == sessionIds.end()) sessionIds.push_back(s.id);
            for (const auto& f : Newest(s.path, L"events*.jsonl", 64))
                c.AddFile("logs/sessions/" + s.id + "/" + Narrow(BaseNameW(f)), f);
        }
    }

    // logs/Melange.log, logs/Melange.prev.log
    {
        bool any = false;
        for (const wchar_t* name : {L"Melange.log", L"Melange.prev.log"})
            if (!dataDir.empty()) any |= c.AddFile("logs/" + Narrow(name), dataDir + L"\\" + name);
        if (!any) c.absent.push_back("logs/Melange.log (not found)");
    }

    // logs/engine/XOM*-*.log, Net_*.log from the game folder: for one game the ones written closest to its window
    // (up to 3 inside it, else the single nearest), otherwise the newest 3.
    {
        bool any = false;
        for (const wchar_t* pattern : {L"XOM*-*.log", L"Net_*.log"}) {
            std::vector<std::wstring> pick;
            if (win.known) {
                auto files = ListFiles(src.gameDir, pattern);
                std::stable_sort(files.begin(), files.end(), [&](const FileInfo& a, const FileInfo& b) {
                    return win.Distance(a.mtime) < win.Distance(b.mtime);
                });
                for (const auto& f : files)
                    if (pick.size() < 3 && win.Contains(f.mtime, kMinute, 15 * kMinute)) pick.push_back(f.path);
                if (pick.empty() && !files.empty()) pick.push_back(files.front().path);
            } else {
                pick = Newest(src.gameDir, pattern, 3);
            }
            for (const auto& f : pick) any |= c.AddFile("logs/engine/" + Narrow(BaseNameW(f)), f);
        }
        if (!any) c.absent.push_back("logs/engine (no engine log files found)");
    }

    // logs/launcher/: Melange.exe's log and the one before it.
    {
        bool any = false;
        for (const wchar_t* name : {L"launcher.log", L"launcher.1.log"})
            if (!src.launcherLogDir.empty())
                any |= c.AddFile("logs/launcher/" + Narrow(name), src.launcherLogDir + L"\\" + name);
        if (!any) c.absent.push_back("logs/launcher (no launcher.log found)");
    }

    // dumps/*.dmp, newest 3 (for one game: from its window). Full-memory dumps ("-full.dmp") can hold private
    // data, so they are opt-in.
    if (opt.includeDumps) {
        std::vector<std::wstring> dumps;
        for (const auto& f : ListFiles(dataDir.empty() ? std::wstring() : dataDir + L"\\dumps", L"*.dmp")) {
            if (HasSuffixCI(f.path, L"-full.dmp") && !opt.includeFullDumps) continue;
            if (rq.scope == Scope::LastGame && !win.Contains(f.mtime, kMinute, 15 * kMinute)) continue;
            dumps.push_back(f.path);
            if (dumps.size() >= 3) break;
        }
        if (dumps.empty()) c.absent.push_back(rq.scope == Scope::LastGame ? "dumps (none from this game)" : "dumps (none found)");
        for (const auto& f : dumps)
            c.AddFile("dumps/" + Narrow(BaseNameW(f)), f, HasSuffixCI(f, L"-full.dmp") ? kFullDumpCap : kCap);
    } else {
        c.absent.push_back("dumps (IncludeDumps=0)");
    }

    // replays/: desync bundles (already redacted when they were written) and match recordings (redacted here,
    // since .wsr is not a text extension and AddFile only redacts those). For one game: every one whose name
    // carries its pid, newest first within a size budget; otherwise the newest of each.
    {
        const size_t before = c.manifest.size();
        auto addWsr = [&](const std::wstring& f) {
            if (!c.AddRedactedWsr("replays/" + Narrow(BaseNameW(f)), f))
                LOG_WARN("[LogExport] could not redact %ls for the export; omitted", f.c_str());
        };
        if (rq.scope == Scope::LastGame) {
            uint64_t budget = kReplayBudget;
            size_t skipped = 0;
            if (pid != 0) {
                for (const wchar_t* pattern : {L"desync-*.zip", L"wsr-*.wsr"}) {
                    for (const auto& f : ListFiles(src.replaysDir, pattern)) {
                        uint32_t fpid = 0;
                        uint64_t when = 0;
                        if (!ParseReplayName(BaseNameW(f.path), &fpid, &when) || fpid != pid) continue;
                        // A pid can come round again on another day: only this game's window counts.
                        if (win.known && when + 2 * kMinute < win.start) continue;
                        const uint64_t size = std::min(FileBytes(f.path), kCap);
                        if (size > budget) {
                            ++skipped;
                            continue;
                        }
                        budget -= size;
                        if (HasSuffixCI(f.path, L".wsr"))
                            addWsr(f.path);
                        else
                            c.AddFile("replays/" + Narrow(BaseNameW(f.path)), f.path);
                    }
                }
            }
            if (skipped) c.absent.push_back("replays (" + std::to_string(skipped) + " older file(s) over the size budget)");
            if (c.manifest.size() == before) c.absent.push_back("replays (no desync bundle or recording from this game)");
        } else {
            for (const auto& f : Newest(src.replaysDir, L"desync-*.zip", 1))
                c.AddFile("replays/" + Narrow(BaseNameW(f)), f);
            for (const auto& f : Newest(src.replaysDir, L"*.wsr", 1)) addWsr(f);
            if (c.manifest.size() == before) c.absent.push_back("replays (no desync bundle or recording found)");
        }
    }

    // config/*.ini from the game folder and the ASI loader's plugins\ and scripts\ folders.
    {
        bool any = false;
        for (const wchar_t* sub : {L"", L"plugins", L"scripts"}) {
            if (src.gameDir.empty()) break;
            std::wstring dir = src.gameDir + (*sub ? L"\\" + std::wstring(sub) : std::wstring());
            std::string arcDir = *sub ? "config/" + Narrow(sub) + "/" : "config/";
            for (const auto& f : Newest(dir, L"*.ini", 64)) any |= c.AddFile(arcDir + Narrow(BaseNameW(f)), f);
        }
        if (!any) c.absent.push_back("config (no .ini files found)");
    }

    // mods/: what is installed and switched on. modules.json only comes from inside the game.
    if (rq.prov.modulesJson) c.AddGenerated("mods/modules.json", rq.prov.modulesJson(), false);
    c.AddGenerated("mods/plugins.json",
                   rq.prov.pluginsJson ? rq.prov.pluginsJson() : melange::sysinfo::PluginsJsonIn(src.gameDir), false);
    if (!src.gameDir.empty()) {
        c.AddGenerated("mods/spice.json", SpiceListing(src.gameDir), true);
        if (!c.AddFile("mods/thumper-state.json", src.gameDir + L"\\Mods\\thumper-state.json"))
            c.absent.push_back("mods/thumper-state.json (not found)");
        if (!c.AddFile("mods/store-installed.json", src.gameDir + L"\\Mods\\.store\\installed.json"))
            c.absent.push_back("mods/store-installed.json (no Store installs)");
    }

    c.AddGenerated("system.json",
                   rq.prov.systemJson ? rq.prov.systemJson() : melange::sysinfo::OfflineJson(src.gameDir), true);
    if (rq.prov.gpuCompatJson) c.AddGenerated("gpu/compat.json", rq.prov.gpuCompatJson(), true);
    if (rq.prov.gpuCompatText) c.AddGenerated("gpu/compat.txt", rq.prov.gpuCompatText(), true);
    if (!rq.prov.gpuCompatJson) c.absent.push_back("gpu (only exported from inside the game)");

    // Last: it needs every other entry's hash.
    {
        jsonmini::Obj optsJson;
        optsJson.Int("sessions", opt.sessions)
            .Bool("includeDumps", opt.includeDumps)
            .Bool("includeFullDumps", opt.includeFullDumps)
            .Bool("redactUserPaths", opt.redactUserPaths);
        jsonmini::Arr entriesJson;
        for (const auto& e : c.manifest) {
            jsonmini::Obj o;
            o.Str("path", e.archivePath).UInt("size", e.size).Str("sha256", e.sha256).Str("source", e.source)
                .Bool("truncated", e.truncated);
            entriesJson.Raw(o.End());
        }
        jsonmini::Arr sessionsJson;
        for (const auto& s : sessionIds) sessionsJson.Str(s);
        jsonmini::Arr absentJson;
        for (const auto& s : c.absent) absentJson.Str(s);

        jsonmini::Obj root;
        root.Str("melangeVersion", MELANGE_VERSION)
            .Str("generatedAtUtc", IsoUtc(NowUtc()))
            .Str("producer", rq.producer)
            .Str("scope", rq.scope == Scope::LastGame ? "lastGame" : "recentSessions")
            .Raw("options", optsJson.End());
        if (rq.scope == Scope::LastGame) {
            jsonmini::Obj game;
            if (haveSession) game.Str("sessionId", chosen.id);
            else game.Raw("sessionId", "null");
            game.UInt("pid", pid).Bool("live", live);
            if (win.known) game.Str("startUtc", IsoUtc(win.start)).Str("endUtc", IsoUtc(win.end));
            game.Str("replaysMatchedBy", "pid");
            root.Raw("game", game.End());
        }
        root.Raw("entries", entriesJson.End())
            .Raw("sessionIds", sessionsJson.End())
            .Raw("absent", absentJson.End());
        // Session ids are local timestamps and pids, but the manifest still goes through the computer-name pass.
        const std::string manifestJson = c.ExcludeComputerName(root.End());
        c.zip.Add("manifest.json", manifestJson.data(), manifestJson.size());
    }
    const std::string readme = BuildReadme(rq.scope);
    c.zip.Add("README.txt", readme.data(), readme.size());

    std::string zipBytes;
    if (!c.zip.Finalize(&zipBytes)) {
        res->error = "miniz failed to finalize the archive";
        return false;
    }

    if (size_t slash = zipPath.find_last_of(L"\\/"); slash != std::wstring::npos)
        EnsureDirectoryRecursive(zipPath.substr(0, slash));
    HANDLE out = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        res->error = "could not create " + Narrow(zipPath);
        return false;
    }
    bool ok = true;
    size_t written = 0;
    while (written < zipBytes.size()) {
        DWORD chunk = 0;
        DWORD want = static_cast<DWORD>(std::min<size_t>(zipBytes.size() - written, 1u << 22));
        if (!WriteFile(out, zipBytes.data() + written, want, &chunk, nullptr) || chunk == 0) {
            ok = false;
            break;
        }
        written += chunk;
    }
    CloseHandle(out);
    if (!ok) {
        res->error = "write failed part-way through " + Narrow(zipPath);
        DeleteFileW(zipPath.c_str());
    }
    res->path = zipPath;
    res->bytes = ok ? zipBytes.size() : 0;
    res->entries = c.manifest.size();
    LOG_INFO("[LogExport] %s export %s: %s (%zu entries, session %s)",
             rq.scope == Scope::LastGame ? "last-game" : "recent-sessions", ok ? "OK" : "FAILED",
             Narrow(zipPath).c_str(), c.manifest.size(), chosen.id.empty() ? "-" : chosen.id.c_str());
    return ok;
}
}  // namespace

bool ParseSessionName(std::wstring_view name, uint32_t* pid, uint64_t* startUtc) {
    // 0         1         2
    // 0123456789012345678901
    // YYYY-MM-DD_HH-MM-SS_pid<digits>
    unsigned y, mo, d, h, mi, s;
    if (name.size() < 24 || !Digits(name, 0, 4, &y) || name[4] != L'-' || !Digits(name, 5, 2, &mo) ||
        name[7] != L'-' || !Digits(name, 8, 2, &d) || name[10] != L'_' || !Digits(name, 11, 2, &h) ||
        name[13] != L'-' || !Digits(name, 14, 2, &mi) || name[16] != L'-' || !Digits(name, 17, 2, &s) ||
        name[19] != L'_' || name.compare(20, 3, L"pid") != 0)
        return false;
    size_t end = 0;
    if (!Pid(name, 23, pid, &end) || end != name.size()) return false;
    return MakeTime(y, mo, d, h, mi, s, startUtc);
}

bool ParseReplayName(std::wstring_view name, uint32_t* pid, uint64_t* whenUtc) {
    size_t p = 0;
    if (name.compare(0, 4, L"wsr-") == 0)
        p = 4;
    else if (name.compare(0, 7, L"desync-") == 0)
        p = 7;
    else
        return false;
    // YYYYMMDD-HHMMSS-p<pid>[-.]
    unsigned y, mo, d, h, mi, s;
    if (!Digits(name, p, 4, &y) || !Digits(name, p + 4, 2, &mo) || !Digits(name, p + 6, 2, &d) ||
        p + 8 >= name.size() || name[p + 8] != L'-' || !Digits(name, p + 9, 2, &h) || !Digits(name, p + 11, 2, &mi) ||
        !Digits(name, p + 13, 2, &s) || name.compare(p + 15, 2, L"-p") != 0)
        return false;
    size_t end = 0;
    if (!Pid(name, p + 17, pid, &end) || end >= name.size() || (name[end] != L'-' && name[end] != L'.')) return false;
    return MakeTime(y, mo, d, h, mi, s, whenUtc);
}

std::vector<SessionDir> FindSessions(const std::vector<std::wstring>& roots) {
    std::vector<SessionDir> out;
    std::vector<std::wstring> seen;  // lower-cased full paths
    for (const auto& rootIn : roots) {
        if (rootIn.empty()) continue;
        std::wstring root = rootIn;
        while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            SessionDir s;
            if (!ParseSessionName(fd.cFileName, &s.pid, &s.startUtc)) continue;
            s.path = root + L"\\" + fd.cFileName;
            wchar_t full[MAX_PATH * 2];
            std::wstring key = GetFullPathNameW(s.path.c_str(), MAX_PATH * 2, full, nullptr) ? full : s.path;
            for (auto& ch : key) ch = static_cast<wchar_t>(towlower(ch));
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
            s.id = Narrow(fd.cFileName);
            out.push_back(std::move(s));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    // The date-first names sort chronologically; the start time breaks ties across roots the same way.
    std::sort(out.begin(), out.end(), [](const SessionDir& a, const SessionDir& b) {
        return a.startUtc != b.startUtc ? a.startUtc > b.startUtc : a.id > b.id;
    });
    return out;
}

std::wstring KnownFolder(Folder f) {
    const KNOWNFOLDERID* id = &FOLDERID_Documents;
    switch (f) {
        case Folder::Documents: id = &FOLDERID_Documents; break;
        case Folder::Desktop: id = &FOLDERID_Desktop; break;
        case Folder::LocalAppData: id = &FOLDERID_LocalAppData; break;
        case Folder::Profile: id = &FOLDERID_Profile; break;
    }
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &path)) && path) out = path;
    if (path) CoTaskMemFree(path);
    return out;
}

std::wstring DefaultZipName() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"Melange-logs-%04u%02u%02u-%02u%02u%02u.zip", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);
    return buf;
}

std::wstring OneClickPath(bool* onDesktop) {
    std::wstring dir = KnownFolder(Folder::Desktop);
    *onDesktop = DirWritable(dir);
    if (!*onDesktop) {
        const std::wstring docs = KnownFolder(Folder::Documents);
        dir = (docs.empty() ? std::wstring(L".") : docs) + L"\\Melange\\exports";
        EnsureDirectoryRecursive(dir);
    }
    // Two exports in the same second get -2, -3, ... rather than overwriting each other.
    const std::wstring name = DefaultZipName();
    std::wstring path = dir + L"\\" + name;
    for (int i = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && i < 100; ++i)
        path = dir + L"\\" + name.substr(0, name.size() - 4) + L"-" + std::to_wstring(i) + L".zip";
    return path;
}

bool RevealInExplorer(const std::wstring& path) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str())) {
        ok = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
        ILFree(pidl);
    }
    if (SUCCEEDED(co)) CoUninitialize();
    if (!ok) {
        const std::wstring args = L"/select,\"" + path + L"\"";
        ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                                                     SW_SHOWNORMAL)) > 32;
    }
    return ok;
}

// An export can need hundreds of MB in a 32-bit process; an exception escaping a worker thread would terminate
// it, so catch everything here.
bool Export(const std::wstring& zipPath, const Request& rq, Result* out) {
    Result local;
    Result* res = out ? out : &local;
    *res = Result{};
    try {
        return DoExport(zipPath, rq, res);
    } catch (const std::exception& e) {
        LOG_ERROR("[LogExport] export threw an exception: %s", e.what());
        res->error = std::string("export failed: ") + e.what();
    } catch (...) {
        LOG_ERROR("[LogExport] export threw an unknown exception");
        res->error = "export failed: unknown exception";
    }
    return false;
}
}  // namespace melange::exporter::core
