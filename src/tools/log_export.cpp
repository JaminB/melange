// LogExport: the "Save logs as..." zip export. Public API: melange/export.h.
#include "melange/export.h"

#include <windows.h>

#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uuid.lib")  // CLSID_FileSaveDialog / IID_IFileSaveDialog definitions

#include "core/config.h"
#include "core/debug.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "miniz.h"
#include "render/mirage/compat.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/redact.h"
#include "tools/sysinfo.h"
#include "version.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/testcmd.h"

namespace melange::exporter {
namespace {

std::string Narrow(const std::wstring& w) { return melange::game::Narrow(w); }

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
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

void EnsureDirectoryRecursive(const std::wstring& dir) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    size_t pos = dir.find_first_of(L"\\/", 3);  // skip past a drive letter ("C:\")
    while (pos != std::wstring::npos) {
        CreateDirectoryW(dir.substr(0, pos).c_str(), nullptr);
        pos = dir.find_first_of(L"\\/", pos + 1);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

std::wstring DocumentsDir() {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)) && path) {
        out = path;
        CoTaskMemFree(path);
    }
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

// Reads the whole file, or only its last capBytes if larger (the newest part matters most).
// Full sharing so files the game or the log writer still has open can be read.
bool ReadCappedTail(const std::wstring& path, uint64_t capBytes, std::string& out, bool* truncated) {
    *truncated = false;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size)) {
        CloseHandle(f);
        return false;
    }
    uint64_t total = static_cast<uint64_t>(size.QuadPart);
    uint64_t toRead = total;
    LARGE_INTEGER seekTo{};
    if (total > capBytes) {
        *truncated = true;
        toRead = capBytes;
        seekTo.QuadPart = static_cast<LONGLONG>(total - capBytes);
        SetFilePointerEx(f, seekTo, nullptr, FILE_BEGIN);
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

std::vector<std::wstring> NewestMatching(const std::wstring& dir, const wchar_t* pattern, size_t max) {
    std::vector<std::pair<FILETIME, std::wstring>> found;
    if (dir.empty()) return {};
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        found.emplace_back(fd.ftLastWriteTime, dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return CompareFileTime(&a.first, &b.first) > 0; });
    std::vector<std::wstring> out;
    for (size_t i = 0; i < found.size() && i < max; ++i) out.push_back(found[i].second);
    return out;
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
std::string ExcludeComputerName(std::string_view text) {
    return melange::redact::ReplaceName(text, ComputerName(), "%COMPUTERNAME%");
}

// Profile folder name. After an account rename it differs from the account name, so both are redacted.
const std::string& ProfileFolderName() {
    static const std::string name = [] {
        PWSTR path = nullptr;
        std::string out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path)) && path) {
            std::wstring w = path;
            size_t p = w.find_last_of(L"\\/");
            out = Narrow(p == std::wstring::npos ? w : w.substr(p + 1));
            CoTaskMemFree(path);
        }
        return out;
    }();
    return name;
}

std::string RedactUserNames(std::string_view text, std::string_view userName) {
    std::string out = melange::redact::RedactUserName(text, userName);
    const std::string& profile = ProfileFolderName();
    if (!profile.empty()) out = melange::redact::RedactUserName(out, profile);
    return out;
}

void AddEntry(ZipBuilder& zip, std::vector<ManifestEntry>& manifest, const std::string& archivePathIn, std::string data,
              const std::wstring& sourcePath, bool truncated, bool isText, bool redactUserPaths,
              std::string_view userName, std::string_view salt) {
    if (isText) {
        // Generated entries hold no ids, only version numbers the IPv4 pattern would hash.
        if (sourcePath != L"(generated)") data = melange::redact::HashIdsAndIps(data, salt);
        if (redactUserPaths) data = RedactUserNames(data, userName);
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
    if (redactUserPaths) e.source = RedactUserNames(e.source, userName);
    e.source = ExcludeComputerName(e.source);
    e.truncated = truncated;
    manifest.push_back(std::move(e));
}

void AddFileEntry(ZipBuilder& zip, std::vector<ManifestEntry>& manifest, const std::string& archivePath,
                   const std::wstring& sourcePath, uint64_t capBytes, bool redactUserPaths, std::string_view userName,
                   std::string_view salt) {
    std::string data;
    bool truncated = false;
    if (!ReadCappedTail(sourcePath, capBytes, data, &truncated)) return;
    bool isText = IsTextExtension(sourcePath);
    if (truncated && isText) {
        data = "...[truncated, showing only the end of a larger file]...\n" + data;
    }
    AddEntry(zip, manifest, archivePath, std::move(data), sourcePath, truncated, isText, redactUserPaths, userName,
             salt);
}

std::string BuildReadme(std::string_view melangeVersion) {
    std::string s;
    s += "Melange logs export\n";
    s += "Melange " + std::string(melangeVersion) + "\n\n";
    s += "This zip contains:\n";
    s += "  README.txt        this file\n";
    s += "  manifest.json     every file below, its size and SHA-256 checksum\n";
    s += "  system.json       OS, CPU, GPU/GL and exe identification\n";
    s += "  gpu/compat.*      GPU compatibility report: driver, GL extensions, Cg profiles, shaders and passes\n";
    s += "  logs/sessions/*   structured JSONL event logs (recent sessions)\n";
    s += "  logs/Melange.log, logs/Melange.prev.log   the plain text log\n";
    s += "  logs/engine/*     the engine's own XOM/Net log files, if found\n";
    s += "  dumps/*.dmp       crash/hang minidumps, if any were found\n";
    s += "  config/*.ini      Melange.ini and any other .ini next to the game exe\n";
    s += "  mods/*.json       installed Melange modules and detected .asi plugins\n";
    s += "  replays/*         the newest desync bundle and match recording, if any\n\n";
    s += "Privacy note - this zip can contain:\n";
    s += "  - your Windows user name in file paths (replaced with %USERNAME% by default)\n";
    s += "  - Steam ids, persona names and lobby ids (ids are replaced with a short hash unique to\n";
    s += "    this export; persona names in chat/trace text are not touched)\n";
    s += "  - IP addresses and ports (replaced the same way as Steam ids)\n";
    s += "  - chat text, if chat logging was on\n";
    s += "  - crash minidumps, which contain parts of the game's memory\n";
    s += "Your computer name is replaced with %COMPUTERNAME% in text files and file names.\n\n";
    s += "Nothing here is uploaded anywhere. This zip is only written to the place you chose.\n";
    return s;
}

std::string BuildManifestJson(const Options& opt, const std::vector<ManifestEntry>& entries,
                              const std::vector<std::string>& sessionIds, const std::vector<std::string>& absent) {
    jsonmini::Obj optsJson;
    optsJson.Int("sessions", opt.sessions)
        .Bool("includeDumps", opt.includeDumps)
        .Bool("includeFullDumps", opt.includeFullDumps)
        .Bool("redactUserPaths", opt.redactUserPaths);

    jsonmini::Arr entriesJson;
    for (const auto& e : entries) {
        jsonmini::Obj o;
        o.Str("path", e.archivePath).UInt("size", e.size).Str("sha256", e.sha256).Str("source", e.source)
            .Bool("truncated", e.truncated);
        entriesJson.Raw(o.End());
    }
    jsonmini::Arr sessionsJson;
    for (const auto& s : sessionIds) sessionsJson.Str(s);
    jsonmini::Arr absentJson;
    for (const auto& s : absent) absentJson.Str(s);

    SYSTEMTIME st;
    GetSystemTime(&st);
    char ts[32];
    snprintf(ts, sizeof(ts), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond);

    jsonmini::Obj root;
    root.Str("melangeVersion", MELANGE_VERSION)
        .Str("generatedAtUtc", ts)
        .Raw("options", optsJson.End())
        .Raw("entries", entriesJson.End())
        .Raw("sessionIds", sessionsJson.End())
        .Raw("absent", absentJson.End());
    return root.End();
}

std::string CurrentUserName() {
    wchar_t buf[256];
    DWORD n = 256;
    if (GetUserNameW(buf, &n)) return Narrow(buf);
    return {};
}

// Status

std::mutex g_statusMx;
State g_state = State::Idle;
std::wstring g_lastPath;
std::string g_lastError;
std::atomic<bool> g_busy{false};

void SetState(State s) {
    std::lock_guard lk(g_statusMx);
    g_state = s;
}
void SetDone(const std::wstring& path) {
    std::lock_guard lk(g_statusMx);
    g_state = State::Done;
    g_lastPath = path;
    g_lastError.clear();
}
void SetFailed(const std::string& err) {
    std::lock_guard lk(g_statusMx);
    g_state = State::Failed;
    g_lastError = err;
}
void SetCancelled() {
    std::lock_guard lk(g_statusMx);
    g_state = State::Cancelled;
}

// Export. Runs on a worker thread; touches no main-thread state.
bool DoExport(const std::wstring& zipPath, const Options& opt, std::string* error) {
    uint64_t frameStart = melange::events::FrameCount();
    if (GetCurrentThreadId() == melange::events::MainThreadId())
        LOG_WARN("[LogExport] ExportTo called on the main thread; this blocks rendering until it's done");

    melange::jlog::Flush();

    ZipBuilder zip;
    std::vector<ManifestEntry> manifest;
    std::vector<std::string> sessionIds;
    std::vector<std::string> absent;
    std::string salt = melange::hashutil::RandomSalt();
    std::string userName = CurrentUserName();
    constexpr uint64_t kCap = 64ull * 1024 * 1024;
    constexpr uint64_t kFullDumpCap = 256ull * 1024 * 1024;

    // logs/sessions/<id>/events*.jsonl
    sessionIds.push_back(melange::jlog::CurrentSession().id);
    auto sessionDirs = melange::jlog::RecentSessionDirs(static_cast<size_t>(std::max(1, opt.sessions)));
    if (sessionDirs.empty()) {
        absent.push_back("logs/sessions (no session folders found)");
    } else {
        for (const auto& dir : sessionDirs) {
            std::string id = Narrow(BaseNameW(dir));
            if (std::find(sessionIds.begin(), sessionIds.end(), id) == sessionIds.end()) sessionIds.push_back(id);
            for (const auto& f : NewestMatching(dir, L"events*.jsonl", 64)) {
                std::string arc = "logs/sessions/" + id + "/" + Narrow(BaseNameW(f));
                AddFileEntry(zip, manifest, arc, f, kCap, opt.redactUserPaths, userName, salt);
            }
        }
    }

    // logs/Melange.log, logs/Melange.prev.log
    {
        std::wstring dataDir = melange::game::DataDir();
        bool any = false;
        for (const wchar_t* name : {L"Melange.log", L"Melange.prev.log"}) {
            std::wstring p = dataDir + L"\\" + name;
            size_t before = manifest.size();
            AddFileEntry(zip, manifest, "logs/" + Narrow(name), p, kCap, opt.redactUserPaths, userName, salt);
            any = any || manifest.size() > before;
        }
        if (!any) absent.push_back("logs/Melange.log (not found)");
    }

    // logs/engine/XOM*-*.log, Net_*.log from the game folder
    {
        std::wstring gameDir = melange::game::GameDir();
        auto xom = NewestMatching(gameDir, L"XOM*-*.log", 3);
        auto net = NewestMatching(gameDir, L"Net_*.log", 3);
        if (xom.empty() && net.empty()) absent.push_back("logs/engine (no engine log files found)");
        for (const auto& f : xom) AddFileEntry(zip, manifest, "logs/engine/" + Narrow(BaseNameW(f)), f, kCap,
                                                opt.redactUserPaths, userName, salt);
        for (const auto& f : net) AddFileEntry(zip, manifest, "logs/engine/" + Narrow(BaseNameW(f)), f, kCap,
                                                opt.redactUserPaths, userName, salt);
    }

    // dumps/*.dmp, newest 3. Full-memory dumps ("-full.dmp") can hold private data, so they are opt-in.
    if (opt.includeDumps) {
        auto allDumps = NewestMatching(melange::game::DataDir() + L"\\dumps", L"*.dmp", 64);
        std::vector<std::wstring> dumps;
        for (const auto& f : allDumps) {
            if (HasSuffixCI(f, L"-full.dmp") && !opt.includeFullDumps) continue;
            dumps.push_back(f);
            if (dumps.size() >= 3) break;
        }
        if (dumps.empty()) absent.push_back("dumps (none found)");
        for (const auto& f : dumps) {
            bool isFull = HasSuffixCI(f, L"-full.dmp");
            AddFileEntry(zip, manifest, "dumps/" + Narrow(BaseNameW(f)), f, isFull ? kFullDumpCap : kCap,
                         opt.redactUserPaths, userName, salt);
        }
    } else {
        absent.push_back("dumps (IncludeDumps=0)");
    }

    // replays/: the newest desync bundle and the newest match recording.
    {
        const std::wstring docs = DocumentsDir();
        size_t before = manifest.size();
        if (!docs.empty())
            for (const wchar_t* pattern : {L"desync-*.zip", L"*.wsr"})
                for (const auto& f : NewestMatching(docs + L"\\Melange\\replays", pattern, 1))
                    AddFileEntry(zip, manifest, "replays/" + Narrow(BaseNameW(f)), f, kCap, opt.redactUserPaths, userName,
                                 salt);
        if (manifest.size() == before) absent.push_back("replays (no desync bundle or recording found)");
    }

    // config/*.ini from the game folder and the ASI loader's plugins\ and scripts\ folders.
    {
        bool any = false;
        for (const wchar_t* sub : {L"", L"plugins", L"scripts"}) {
            std::wstring dir = melange::game::GameDir() + (*sub ? L"\\" + std::wstring(sub) : std::wstring());
            std::string arcDir = *sub ? "config/" + Narrow(sub) + "/" : "config/";
            for (const auto& f : NewestMatching(dir, L"*.ini", 64)) {
                AddFileEntry(zip, manifest, arcDir + Narrow(BaseNameW(f)), f, kCap, opt.redactUserPaths, userName, salt);
                any = true;
            }
        }
        if (!any) absent.push_back("config (no .ini files found)");
    }

    // mods/modules.json: installed modules only; skipped/disabled ones are not tracked.
    {
        jsonmini::Arr arr;
        for (const auto* m : melange::modules::Installed()) {
            jsonmini::Obj o;
            o.Str("name", m->Name()).Str("description", m->Description()).Int("order", m->Order())
                .Bool("enabled", true)
                .Bool("installed", true);
            arr.Raw(o.End());
        }
        std::string data = arr.End();
        AddEntry(zip, manifest, "mods/modules.json", data, L"(generated)", false, false, opt.redactUserPaths,
                 userName, salt);
    }

    {
        std::string data = melange::sysinfo::PluginsJson();
        AddEntry(zip, manifest, "mods/plugins.json", data, L"(generated)", false, false, opt.redactUserPaths,
                 userName, salt);
    }

    {
        std::string data = melange::sysinfo::CollectJson();
        AddEntry(zip, manifest, "system.json", data, L"(generated)", false, true, opt.redactUserPaths, userName, salt);
    }

    AddEntry(zip, manifest, "gpu/compat.json", melange::mirage::compat::Json(), L"(generated)", false, true,
             opt.redactUserPaths, userName, salt);
    AddEntry(zip, manifest, "gpu/compat.txt", melange::mirage::compat::Text(), L"(generated)", false, true,
             opt.redactUserPaths, userName, salt);

    // Last: it needs every other entry's hash.
    std::string manifestJson = BuildManifestJson(opt, manifest, sessionIds, absent);
    zip.Add("manifest.json", manifestJson.data(), manifestJson.size());

    std::string readme = BuildReadme(MELANGE_VERSION);
    zip.Add("README.txt", readme.data(), readme.size());

    std::string zipBytes;
    if (!zip.Finalize(&zipBytes)) {
        if (error) *error = "miniz failed to finalize the archive";
        return false;
    }

    if (size_t slash = zipPath.find_last_of(L"\\/"); slash != std::wstring::npos)
        EnsureDirectoryRecursive(zipPath.substr(0, slash));
    HANDLE out = CreateFileW(zipPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        if (error) *error = "could not create " + Narrow(zipPath);
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
    if (!ok && error) *error = "write failed part-way through " + Narrow(zipPath);

    uint64_t frameEnd = melange::events::FrameCount();
    LOG_INFO("[LogExport] export %s: %s (%zu entries, frame %llu -> %llu)", ok ? "OK" : "FAILED", Narrow(zipPath).c_str(),
            manifest.size(), static_cast<unsigned long long>(frameStart), static_cast<unsigned long long>(frameEnd));
    return ok;
}

// An export can need hundreds of MB in this 32-bit process; an exception escaping a worker thread would
// terminate the game, so catch everything here.
bool SafeDoExport(const std::wstring& zipPath, const Options& opt, std::string* error) {
    try {
        return DoExport(zipPath, opt, error);
    } catch (const std::exception& e) {
        LOG_ERROR("[LogExport] export threw an exception: %s", e.what());
        if (error) *error = std::string("export failed: ") + e.what();
        return false;
    } catch (...) {
        LOG_ERROR("[LogExport] export threw an unknown exception");
        if (error) *error = "export failed: unknown exception";
        return false;
    }
}

// Save dialog

// Heuristic: no caption/frame and the window covers its monitor (exclusive and borderless alike).
bool IsFullscreen() {
    HWND h = static_cast<HWND>(melange::events::GameWindow());
    if (!h) return false;
    LONG style = GetWindowLongW(h, GWL_STYLE);
    if (style & (WS_CAPTION | WS_THICKFRAME)) return false;
    RECT wr{};
    if (!GetWindowRect(h, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

enum class DialogResult { Ok, Cancelled, ApiUnavailable };

DialogResult ShowSaveDialogCOM(std::wstring* outPath) {
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return DialogResult::ApiUnavailable;
    COMDLG_FILTERSPEC filter[] = {{L"Zip files", L"*.zip"}};
    dlg->SetFileTypes(1, filter);
    dlg->SetDefaultExtension(L"zip");
    dlg->SetFileName(DefaultZipName().c_str());
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(DocumentsDir().c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
        dlg->SetFolder(folder);
        folder->Release();
    }
    HRESULT hr = dlg->Show(nullptr);  // no owner, so the game keeps rendering
    DialogResult result = DialogResult::Cancelled;
    if (SUCCEEDED(hr)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                *outPath = path;
                CoTaskMemFree(path);
                result = DialogResult::Ok;
            }
            item->Release();
        }
    } else if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result = DialogResult::ApiUnavailable;  // not a cancel: fall back to the legacy dialog
    }
    dlg->Release();
    return result;
}

DialogResult ShowSaveDialogLegacy(std::wstring* outPath) {
    wchar_t buf[MAX_PATH] = L"";
    wcsncpy_s(buf, DefaultZipName().c_str(), _TRUNCATE);
    std::wstring docs = DocumentsDir();
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Zip files\0*.zip\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"zip";
    ofn.lpstrInitialDir = docs.empty() ? nullptr : docs.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        *outPath = buf;
        return DialogResult::Ok;
    }
    return DialogResult::Cancelled;
}

// Toast: a topmost click-through popup showing where a fullscreen export was saved.
LRESULT CALLBACK ToastWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(240, 240, 240));
            auto* text = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (text) DrawTextW(dc, text->c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER:
            DestroyWindow(h);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

DWORD WINAPI ToastThreadProc(LPVOID param) {
    std::unique_ptr<std::wstring> text(static_cast<std::wstring*>(param));
    static std::atomic<bool> s_classRegistered{false};
    const wchar_t* kClassName = L"MelangeExportToast";
    if (!s_classRegistered.exchange(true)) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &ToastWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClassName;
        wc.hbrBackground = CreateSolidBrush(RGB(24, 24, 24));
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
        RegisterClassW(&wc);
    }
    RECT area{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (HWND game = static_cast<HWND>(melange::events::GameWindow())) GetWindowRect(game, &area);
    int w = 560, h = 56;
    int x = area.left + ((area.right - area.left) - w) / 2;
    int y = area.top + 36;
    HWND toast = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                 kClassName, L"", WS_POPUP, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr),
                                 nullptr);
    if (!toast) return 0;
    SetLayeredWindowAttributes(toast, 0, 235, LWA_ALPHA);
    SetWindowLongPtrW(toast, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(text.get()));
    ShowWindow(toast, SW_SHOWNOACTIVATE);
    SetTimer(toast, 1, 6000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

void ShowToast(const std::wstring& text) {
    auto* data = new std::wstring(text);
    HANDLE th = CreateThread(nullptr, 0, &ToastThreadProc, data, 0, nullptr);
    if (th)
        CloseHandle(th);
    else
        delete data;
}

DWORD WINAPI SaveAsWorkerProc(LPVOID) {
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool fullscreen = IsFullscreen();
    std::wstring path;
    bool haveDialog = true;

    if (fullscreen) {
        std::wstring dir = DocumentsDir();
        dir = (dir.empty() ? std::wstring(L".") : dir) + L"\\Melange\\exports";
        EnsureDirectoryRecursive(dir);
        path = dir + L"\\" + DefaultZipName();
    } else {
        SetState(State::Dialog);
        DialogResult r = ShowSaveDialogCOM(&path);
        if (r == DialogResult::ApiUnavailable) r = ShowSaveDialogLegacy(&path);
        haveDialog = r == DialogResult::Ok;
        if (!haveDialog) {
            SetCancelled();
            LOG_INFO("[LogExport] save dialog cancelled (state Cancelled, nothing written)");
        }
    }

    if (haveDialog) {
        SetState(State::Writing);
        std::string err;
        bool ok = SafeDoExport(path, DefaultOptions(), &err);
        if (ok) {
            SetDone(path);
            if (fullscreen) ShowToast(L"Melange: logs saved to " + path);
        } else {
            SetFailed(err);
        }
    }

    if (SUCCEEDED(coHr)) CoUninitialize();
    g_busy = false;
    return 0;
}

bool OnSaveLogsVerb(std::string_view args, void*) {
    std::string path = Trim(std::string(args));
    if (path.empty()) {
        LOG_WARN("[auto] savelogs: usage: savelogs <path.zip>");
        return false;
    }
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        LOG_WARN("[auto] savelogs: an export is already running");
        return false;
    }
    SetState(State::Writing);
    auto* pathCopy = new std::wstring(Widen(path));
    HANDLE th = CreateThread(
        nullptr, 0,
        [](LPVOID param) -> DWORD {
            std::unique_ptr<std::wstring> p(static_cast<std::wstring*>(param));
            std::string err;
            bool ok = SafeDoExport(*p, DefaultOptions(), &err);
            if (ok) {
                SetDone(*p);
                LOG_INFO("[auto] savelogs: wrote %s", Narrow(*p).c_str());
            } else {
                SetFailed(err);
                LOG_ERROR("[auto] savelogs: failed: %s", err.c_str());
            }
            g_busy = false;
            return 0;
        },
        pathCopy, 0, nullptr);
    if (!th) {
        LOG_ERROR("[auto] savelogs: CreateThread failed");
        delete pathCopy;
        g_busy = false;
        return false;
    }
    CloseHandle(th);
    return true;  // result is logged asynchronously; testcmd handlers must not block
}

void OnHotkeyOrMenu(void*) { RequestSaveAs(); }

class LogExport final : public melange::Module {
public:
    const char* Name() const override { return "LogExport"; }
    const char* Description() const override {
        return "\"Save logs as...\": zip of logs, dumps, ini and system info";
    }
    int Order() const override { return 60; }

    bool Install() override {
        melange::config::EnsureKey(Name(), "Hotkey", "Ctrl+Shift+F11");
        std::string hotkeyText = melange::config::GetString(Name(), "Hotkey", "Ctrl+Shift+F11");

        uint8_t dik = 0, mods = 0;
        if (!hotkeyText.empty()) {
            if (melange::overlay::ParseHotkey(hotkeyText.c_str(), &dik, &mods))
                melange::overlay::AddHotkey(dik, mods, &OnHotkeyOrMenu, nullptr);
            else
                LOG_WARN("[LogExport] Hotkey '%s' not understood, no hotkey registered", hotkeyText.c_str());
        }
        melange::overlay::AddMenuItem("File/Save logs as...", &OnHotkeyOrMenu, nullptr, hotkeyText.c_str());
        melange::testcmd::Register("savelogs", &OnSaveLogsVerb);
        LOG_INFO("[LogExport] ready (hotkey %s)", hotkeyText.empty() ? "(none)" : hotkeyText.c_str());
        return true;
    }
};
}  // namespace

MELANGE_MODULE(LogExport);

Options DefaultOptions() {
    Options o;
    melange::config::EnsureKey("LogExport", "Sessions", "3");
    o.sessions = melange::config::GetInt("LogExport", "Sessions", 3);
    melange::config::EnsureKey("LogExport", "IncludeDumps", "1");
    o.includeDumps = melange::config::GetBool("LogExport", "IncludeDumps", true);
    melange::config::EnsureKey("LogExport", "IncludeFullDumps", "0");
    o.includeFullDumps = melange::config::GetBool("LogExport", "IncludeFullDumps", false);
    melange::config::EnsureKey("LogExport", "RedactUserPaths", "1");
    o.redactUserPaths = melange::config::GetBool("LogExport", "RedactUserPaths", true);
    return o;
}

bool RequestSaveAs() {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        LOG_WARN("[LogExport] an export is already running, ignoring request");
        return false;
    }
    SetState(State::Dialog);
    HANDLE th = CreateThread(nullptr, 0, &SaveAsWorkerProc, nullptr, 0, nullptr);
    if (!th) {
        g_busy = false;
        SetFailed("CreateThread failed");
        return false;
    }
    CloseHandle(th);
    return true;
}

bool ExportTo(const std::wstring& zipPath, const Options& opt, std::string* error) {
    bool expected = false;
    if (!g_busy.compare_exchange_strong(expected, true)) {
        if (error) *error = "an export is already running";
        LOG_WARN("[LogExport] ExportTo: an export is already running");
        return false;
    }
    SetState(State::Writing);
    bool ok = SafeDoExport(zipPath, opt, error);
    if (ok)
        SetDone(zipPath);
    else
        SetFailed(error ? *error : "export failed");
    g_busy = false;
    return ok;
}

State Status(std::wstring* lastPath, std::string* lastError) {
    std::lock_guard lk(g_statusMx);
    if (lastPath) *lastPath = g_lastPath;
    if (lastError) *lastError = g_lastError;
    return g_state;
}
}  // namespace melange::exporter
