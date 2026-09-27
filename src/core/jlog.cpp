// Structured JSONL logging core: session folders, the writer thread, rotation, level filters and the
// in-memory tail ring. Public contract: src/sdk/wumfix/jlog.h (frozen; see docs/m0-design.md SS2.5, SS3 "C").
//
// Never calls wf::log (that would recurse through the WF_ tap in jlog_adapters.cpp).
#include "wumfix/jlog.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "core/events.h"
#include "core/game.h"
#include "core/jlog_internal.h"
#include "version.h"

namespace wf::jlog {
namespace {

// ------------------------------------------------------------------------------------------------- JSON helpers
void AppendEscaped(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);  // raw UTF-8 byte, valid inside a JSON string
                }
        }
    }
    out += '"';
}

// `out` accumulates comma-separated "key":value fragments with no surrounding braces (the caller wraps them).
void AppendKey(std::string& out, const char* k) {
    if (!out.empty()) out += ',';
    AppendEscaped(out, k);
    out += ':';
}

double Finite(double v) { return std::isfinite(v) ? v : 0.0; }

void AppendNumber(std::string& out, double v) {
    char buf[48];
    // %.17g round-trips a double; trim is unnecessary for JSON validity.
    snprintf(buf, sizeof(buf), "%.17g", Finite(v));
    out += buf;
}

// ------------------------------------------------------------------------------------------------- level filter
const char* LevelName(Level l) {
    switch (l) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
        case Level::Fatal: return "fatal";
    }
    return "?";
}

Level ParseLevel(std::string_view s) {
    if (s == "trace") return Level::Trace;
    if (s == "debug") return Level::Debug;
    if (s == "warn") return Level::Warn;
    if (s == "error") return Level::Error;
    if (s == "fatal") return Level::Fatal;
    return Level::Info;
}

struct LevelFilter {
    Level def = Level::Info;
    std::unordered_map<std::string, Level> perCategory;
};

// Set once at Init(); read-mostly afterwards, so a raw atomic pointer (no reader lock) is enough.
std::atomic<LevelFilter*> g_filter{nullptr};

LevelFilter* ParseLevelsSpec(std::string_view spec) {
    auto* f = new LevelFilter();
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string_view part = spec.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos);
        pos = comma == std::string_view::npos ? spec.size() : comma + 1;
        size_t colon = part.find(':');
        if (colon == std::string_view::npos) continue;
        std::string_view cat = part.substr(0, colon);
        Level lvl = ParseLevel(part.substr(colon + 1));
        if (cat == "*")
            f->def = lvl;
        else
            f->perCategory.emplace(std::string(cat), lvl);
    }
    return f;
}

// ------------------------------------------------------------------------------------------------- state
// Starts at 1, not 0, so that Tail(0, ...) - "everything since the beginning" - never has to special-case the
// very first record: seq 0 is reserved as the "nothing seen yet" sentinel an incremental reader starts from.
std::atomic<uint64_t> g_seq{1};
std::atomic<uint64_t> g_startTick{0};  // GetTickCount64() at session start

struct QueuedLine {
    std::string text;  // one full JSON line, no trailing newline
    Level lvl;
    bool startsFile = false;  // the writer rolls the file over before writing this line (a session header)
};

// Bytes queued for the current file, counted where seq is assigned (under g_queueMx) so the rollover decision
// and the session header that opens each new file keep seq strictly increasing within every file.
uint64_t g_queuedFileBytes = 0;
uint32_t g_filePart = 0;

std::mutex g_queueMx;
std::vector<QueuedLine> g_queue;
std::condition_variable g_wakeWriter;
bool g_wantExit = false;
std::atomic<bool> g_running{false};

std::mutex g_flushMx;
std::condition_variable g_flushCv;
std::atomic<uint64_t> g_flushRequest{0};
std::atomic<uint64_t> g_flushDone{0};

std::mutex g_tailMx;
std::deque<Line> g_tailRing;
size_t g_tailCapacity = 5000;

std::atomic<uint64_t> g_recordsAccepted{0};
std::atomic<uint64_t> g_recordsDropped{0};  // pending note for the writer (reset when written)
std::atomic<uint64_t> g_droppedTotal{0};    // since session start, for GetStats()
// Main-thread record cost (Rec construction to the end of Emit), 0.25 us buckets up to 200 us; the last bucket
// collects everything slower. Read by internal::EmitP95Us() for the jlog.stats verb (docs/m0-design.md SS3 C8).
constexpr int kEmitBuckets = 801;
std::atomic<uint32_t> g_emitHist[kEmitBuckets];
double g_qpcToUs = 0;
std::atomic<uint64_t> g_bytesWritten{0};
std::atomic<uint64_t> g_filesRotated{0};

Session g_session;
std::wstring g_sessionRoot;  // root actually in use (after fallback resolution)
uint32_t g_maxFileMB = 32;
uint32_t g_maxSessions = 20;
uint32_t g_maxTotalMB = 512;

// The active file. Only the writer thread touches the HANDLE for writing; FlushFromCrash reads it via an
// atomic snapshot so it never races a rotate that closes/reopens it out from under a normal write.
std::atomic<HANDLE> g_fileHandle{INVALID_HANDLE_VALUE};
uint64_t g_fileBytes = 0;
uint32_t g_rotateIndex = 0;

HANDLE g_writerThread = nullptr;

// ------------------------------------------------------------------------------------------------- filesystem
bool MakeDirRecursive(const std::wstring& dir) {
    if (dir.empty() || CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    if (!MakeDirRecursive(dir.substr(0, slash))) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool DirWritable(const std::wstring& dir) {
    if (!MakeDirRecursive(dir)) return false;
    std::wstring probe = dir + L"\\.wumfix_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

std::wstring DocumentsLogsDefault() {
    PWSTR docs = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) out = docs;
    if (docs) CoTaskMemFree(docs);
    if (out.empty()) return L"";
    return out + L"\\WUMFix\\logs";
}

std::wstring FormatSessionId(const SYSTEMTIME& st, DWORD pid) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%04u-%02u-%02u_%02u-%02u-%02u_pid%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
             st.wSecond, static_cast<unsigned long>(pid));
    return buf;
}

// Lists immediate subdirectories of `root` whose name looks like a session folder (has an underscore, so we
// don't trip over an unrelated file a user dropped in there), oldest first.
std::vector<std::wstring> ListSessionDirsOldestFirst(const std::wstring& root) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (name.find(L'_') == std::wstring::npos) continue;
        out.push_back(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());  // the date-first name format sorts chronologically
    return out;
}

uint64_t DirSizeBytes(const std::wstring& dir) {
    uint64_t total = 0;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        total += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

void DeleteDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteDirRecursive(path);
            else
                DeleteFileW(path.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

// Keeps the newest maxSessions folders and total <= maxTotalMB, deleting the oldest first. Called once at
// startup, before the current session's own folder exists, so nothing "current" can be deleted.
void PruneOldSessions(const std::wstring& root, uint32_t maxSessions, uint32_t maxTotalMB) {
    auto dirs = ListSessionDirsOldestFirst(root);
    while (dirs.size() > maxSessions) {
        DeleteDirRecursive(root + L"\\" + dirs.front());
        dirs.erase(dirs.begin());
    }
    uint64_t capBytes = static_cast<uint64_t>(maxTotalMB) * 1024ull * 1024ull;
    uint64_t total = 0;
    std::vector<uint64_t> sizes(dirs.size());
    for (size_t i = 0; i < dirs.size(); ++i) {
        sizes[i] = DirSizeBytes(root + L"\\" + dirs[i]);
        total += sizes[i];
    }
    size_t i = 0;
    while (total > capBytes && i < dirs.size()) {
        DeleteDirRecursive(root + L"\\" + dirs[i]);
        total -= sizes[i];
        ++i;
    }
}

// ------------------------------------------------------------------------------------------------- wall clock
std::string FormatWall() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    TIME_ZONE_INFORMATION tzi{};
    DWORD tzr = GetTimeZoneInformation(&tzi);
    // Bias is minutes to ADD to local time to get UTC, so the offset to show is the negation of it.
    long biasMin = -(tzi.Bias + (tzr == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : tzr == TIME_ZONE_ID_STANDARD ? tzi.StandardBias : 0));
    char sign = biasMin < 0 ? '-' : '+';
    long absMin = biasMin < 0 ? -biasMin : biasMin;
    char buf[48];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02u.%03u%c%02ld:%02ld", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, sign, absMin / 60, absMin % 60);
    return buf;
}

// ------------------------------------------------------------------------------------------------- writer thread
void Rotate() {
    if (g_fileHandle.load() == INVALID_HANDLE_VALUE) return;
    HANDLE old = g_fileHandle.exchange(INVALID_HANDLE_VALUE);
    if (old != INVALID_HANDLE_VALUE) CloseHandle(old);
    std::wstring base = g_session.dir + L"\\events.jsonl";
    std::wstring rolled = g_session.dir + L"\\events." + std::to_wstring(++g_rotateIndex) + L".jsonl";
    MoveFileExW(base.c_str(), rolled.c_str(), MOVEFILE_REPLACE_EXISTING);
    HANDLE h = CreateFileW(base.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_fileHandle.store(h);
    g_fileBytes = 0;
    g_filesRotated.fetch_add(1, std::memory_order_relaxed);
}

void WriteLines(const std::vector<QueuedLine>& lines) {
    if (lines.empty()) return;
    bool needFlush = false;
    std::string batch;
    for (const auto& l : lines) {
        if (l.startsFile) Rotate();
        batch.clear();
        batch.reserve(l.text.size() + 1);
        batch += l.text;
        batch += '\n';
        HANDLE h = g_fileHandle.load();
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, batch.data(), static_cast<DWORD>(batch.size()), &written, nullptr);
            g_fileBytes += written;
            g_bytesWritten.fetch_add(written, std::memory_order_relaxed);
        }
        if (l.lvl >= Level::Error) needFlush = true;
    }
    if (needFlush) {
        HANDLE h = g_fileHandle.load();
        if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
    }
}

DWORD WINAPI WriterMain(LPVOID) {
    for (;;) {
        std::vector<QueuedLine> local;
        uint64_t flushTarget = 0;
        uint64_t dropped = 0, droppedSeq = 0;
        {
            std::unique_lock lk(g_queueMx);
            g_wakeWriter.wait_for(lk, std::chrono::milliseconds(100), [] { return g_wantExit || !g_queue.empty(); });
            if (g_wantExit && g_queue.empty()) {
                lk.unlock();
                break;
            }
            local.swap(g_queue);
            flushTarget = g_flushRequest.load(std::memory_order_acquire);
            // Taken under the queue lock: every swapped line has a smaller seq, every later one a larger seq.
            dropped = g_recordsDropped.exchange(0, std::memory_order_acq_rel);
            if (dropped) droppedSeq = g_seq.fetch_add(1, std::memory_order_relaxed);
        }
        if (dropped) {
            std::string note = "{\"v\":1,\"seq\":" + std::to_string(droppedSeq) +
                               ",\"t\":0,\"wall\":\"" + FormatWall() +
                               "\",\"frame\":0,\"tid\":0,\"main\":false,\"lvl\":\"warn\",\"cat\":\"session\","
                               "\"msg\":\"dropped\",\"data\":{\"count\":" + std::to_string(dropped) + "}}";
            local.push_back({std::move(note), Level::Warn});
        }
        WriteLines(local);
        // Explicit Flush() request: force the buffers out even if nothing above needed it.
        if (flushTarget > g_flushDone.load(std::memory_order_relaxed)) {
            HANDLE h = g_fileHandle.load();
            if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
        }
        g_flushDone.store(flushTarget, std::memory_order_release);
        {
            std::lock_guard lk(g_flushMx);
            g_flushCv.notify_all();
        }
    }
    return 0;
}

}  // namespace

// ------------------------------------------------------------------------------------------------- Rec
struct Rec::Impl {
    int64_t t0 = 0;  // QPC at construction (main thread only), for the emit-cost histogram
    bool enabled = false;
    bool emitted = false;
    std::string category;
    Level level = Level::Info;
    std::string msg;
    std::string data;  // accumulated "key":val fragments, no surrounding braces
};

// Only Rec's own members may name its private Impl type, so the "is this record filtered out" fast path lives
// here rather than in a free helper: still just one small allocation and no string copying when disabled.
Rec::Rec(std::string_view category, Level lvl, std::string_view msg) : p_(new Impl()) {
    p_->enabled = Enabled(category, lvl);
    if (!p_->enabled) return;
    if (GetCurrentThreadId() == wf::events::MainThreadId()) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        p_->t0 = q.QuadPart;
    }
    p_->level = lvl;
    p_->category.assign(category);
    p_->msg.assign(msg);
}

Rec::~Rec() {
    Emit();
    delete p_;
}

Rec& Rec::Int(const char* k, int64_t v) {
    if (!p_->enabled) return *this;
    AppendKey(p_->data, k);
    p_->data += std::to_string(v);
    return *this;
}
Rec& Rec::Uint(const char* k, uint64_t v) {
    if (!p_->enabled) return *this;
    AppendKey(p_->data, k);
    p_->data += std::to_string(v);
    return *this;
}
Rec& Rec::Hex(const char* k, uint64_t v) {
    if (!p_->enabled) return *this;
    char buf[24];
    snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    AppendKey(p_->data, k);
    AppendEscaped(p_->data, buf);
    return *this;
}
Rec& Rec::Float(const char* k, double v) {
    if (!p_->enabled) return *this;
    AppendKey(p_->data, k);
    AppendNumber(p_->data, v);
    return *this;
}
Rec& Rec::Str(const char* k, std::string_view v) {
    if (!p_->enabled) return *this;
    AppendKey(p_->data, k);
    AppendEscaped(p_->data, v);
    return *this;
}
Rec& Rec::Bool(const char* k, bool v) {
    if (!p_->enabled) return *this;
    AppendKey(p_->data, k);
    p_->data += v ? "true" : "false";
    return *this;
}
Rec& Rec::Vec3(const char* k, const float v[3]) {
    if (!p_->enabled) return *this;
    AppendKey(p_->data, k);
    p_->data += '[';
    AppendNumber(p_->data, v[0]);
    p_->data += ',';
    AppendNumber(p_->data, v[1]);
    p_->data += ',';
    AppendNumber(p_->data, v[2]);
    p_->data += ']';
    return *this;
}
Rec& Rec::Raw(const char* k, std::string_view json) {
    if (!p_->enabled) return *this;
    AppendKey(p_->data, k);
    p_->data += json;
    return *this;
}

void Rec::Emit() {
    if (!p_->enabled || p_->emitted) return;
    p_->emitted = true;

    uint64_t nowTick = GetTickCount64();
    double t = (nowTick - g_startTick.load(std::memory_order_relaxed)) / 1000.0;
    uint64_t frame = wf::events::FrameCount();
    DWORD tid = GetCurrentThreadId();
    bool main = tid == wf::events::MainThreadId();

    // Everything after the seq; the seq itself is assigned under g_queueMx so that file order == seq order.
    auto tailFields = [&](Level lvl, std::string_view cat, std::string_view msg, std::string_view data) {
        std::string s;
        s.reserve(160 + msg.size() + data.size());
        s += ",\"t\":";
        AppendNumber(s, t);
        s += ",\"wall\":";
        AppendEscaped(s, FormatWall());
        s += ",\"frame\":";
        s += std::to_string(frame);
        s += ",\"tid\":";
        s += std::to_string(tid);
        s += ",\"main\":";
        s += main ? "true" : "false";
        s += ",\"lvl\":";
        AppendEscaped(s, LevelName(lvl));
        s += ",\"cat\":";
        AppendEscaped(s, cat);
        s += ",\"msg\":";
        AppendEscaped(s, msg);
        if (!data.empty()) {
            s += ",\"data\":{";
            s += data;
            s += '}';
        }
        s += '}';
        return s;
    };
    std::string rest = tailFields(p_->level, p_->category, p_->msg, p_->data);
    auto withSeq = [](uint64_t seq, const std::string& r) { return "{\"v\":1,\"seq\":" + std::to_string(seq) + r; };

    uint64_t seq = 0;
    std::string line;
    const uint64_t capBytes = static_cast<uint64_t>(g_maxFileMB) * 1024ull * 1024ull;
    g_recordsAccepted.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lk(g_queueMx);
        seq = g_seq.fetch_add(1, std::memory_order_relaxed);
        line = withSeq(seq, rest);
        if (g_queue.size() >= 65536) {
            g_recordsDropped.fetch_add(1, std::memory_order_relaxed);
            g_droppedTotal.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (capBytes && g_queuedFileBytes && g_queuedFileBytes + line.size() + 1 > capBytes) {
                // Roll over: the new file opens with its own session record (schema v1: "the first record of
                // every file is session/start"), numbered just before this line.
                std::string data = "\"version\":\"" WUMFIX_VERSION "\",\"exeSha256\":\"" + wf::game::Exe().sha256 +
                                   "\",\"pid\":" + std::to_string(GetCurrentProcessId()) +
                                   ",\"part\":" + std::to_string(++g_filePart);
                std::string hdr = withSeq(seq, tailFields(Level::Info, "session", "start", data));
                seq = g_seq.fetch_add(1, std::memory_order_relaxed);
                line = withSeq(seq, rest);
                g_queuedFileBytes = hdr.size() + 1;
                g_queue.push_back({std::move(hdr), Level::Info, true});
            }
            g_queuedFileBytes += line.size() + 1;
            g_queue.push_back({line, p_->level});
        }
    }
    {
        std::lock_guard lk(g_tailMx);
        g_tailRing.push_back(Line{seq, p_->level, p_->category, std::move(line)});
        if (g_tailRing.size() > g_tailCapacity) g_tailRing.pop_front();
    }
    bool urgent = p_->level >= Level::Warn;    if (urgent) g_wakeWriter.notify_one();
    if (p_->t0 && g_qpcToUs > 0) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        double us = (q.QuadPart - p_->t0) * g_qpcToUs;
        int b = static_cast<int>(us * 4.0);
        g_emitHist[b < 0 ? 0 : (b >= kEmitBuckets ? kEmitBuckets - 1 : b)].fetch_add(1, std::memory_order_relaxed);
    }
}

bool Enabled(std::string_view category, Level lvl) {
    LevelFilter* f = g_filter.load(std::memory_order_acquire);
    if (!f) return lvl >= Level::Info;  // logging not started yet (e.g. very early boot): keep Info+ silently
    auto it = f->perCategory.find(std::string(category));
    Level min = it != f->perCategory.end() ? it->second : f->def;
    return lvl >= min;
}

const Session& CurrentSession() { return g_session; }

std::vector<std::wstring> RecentSessionDirs(size_t max) {
    std::vector<std::wstring> out;
    auto dirs = ListSessionDirsOldestFirst(g_sessionRoot);
    for (auto it = dirs.rbegin(); it != dirs.rend() && out.size() < max; ++it) out.push_back(g_sessionRoot + L"\\" + *it);
    return out;
}

bool Flush(uint32_t timeoutMs) {
    if (!g_running.load()) return true;
    uint64_t target = g_flushRequest.fetch_add(1, std::memory_order_acq_rel) + 1;
    {
        std::lock_guard lk(g_queueMx);
        g_wakeWriter.notify_one();
    }
    std::unique_lock lk(g_flushMx);
    return g_flushCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                              [target] { return g_flushDone.load(std::memory_order_acquire) >= target; });
}

// No lock acquisition beyond a try_lock, and no heap allocation: safe to call from the crash filter.
void FlushFromCrash() {
    if (!g_running.load()) return;
    std::vector<QueuedLine> local;
    {
        std::unique_lock<std::mutex> lk(g_queueMx, std::try_to_lock);
        if (!lk.owns_lock()) return;  // another thread holds it; best effort only
        local.swap(g_queue);
    }
    for (const auto& l : local) {
        HANDLE h = g_fileHandle.load();
        if (h == INVALID_HANDLE_VALUE) continue;
        DWORD written = 0;
        WriteFile(h, l.text.data(), static_cast<DWORD>(l.text.size()), &written, nullptr);
        WriteFile(h, "\n", 1, &written, nullptr);
    }
    HANDLE h = g_fileHandle.load();
    if (h != INVALID_HANDLE_VALUE) FlushFileBuffers(h);
}

size_t Tail(uint64_t afterSeq, std::vector<Line>& out, size_t max) {
    std::lock_guard lk(g_tailMx);
    size_t added = 0;
    for (const auto& l : g_tailRing) {
        if (l.seq <= afterSeq) continue;
        out.push_back(l);
        if (++added >= max) break;
    }
    return added;
}

Stats GetStats() {
    return Stats{g_recordsAccepted.load(), g_droppedTotal.load(), g_bytesWritten.load(), g_filesRotated.load()};
}

// ------------------------------------------------------------------------------------------------- internal
namespace internal {

double EmitP95Us(uint64_t* samples) {
    uint64_t n = 0;
    for (auto& b : g_emitHist) n += b.load(std::memory_order_relaxed);
    if (samples) *samples = n;
    if (!n) return 0.0;
    uint64_t want = (n * 95 + 99) / 100, acc = 0;
    for (int i = 0; i < kEmitBuckets; ++i) {
        acc += g_emitHist[i].load(std::memory_order_relaxed);
        if (acc >= want) return (i + 1) / 4.0;  // upper edge of the bucket
    }
    return kEmitBuckets / 4.0;
}

bool Init(const Options& opt) {
    {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcToUs = f.QuadPart ? 1e6 / static_cast<double>(f.QuadPart) : 0.0;
    }
    g_maxFileMB = opt.maxFileMB;
    g_maxSessions = opt.maxSessions;
    g_maxTotalMB = opt.maxTotalMB;
    g_tailCapacity = opt.tailCapacity ? opt.tailCapacity : 5000;
    g_filter.store(ParseLevelsSpec(opt.levelsSpec), std::memory_order_release);

    std::wstring root = !opt.rootOverride.empty() ? opt.rootOverride : DocumentsLogsDefault();
    bool usedFallback = false;
    if (root.empty() || !DirWritable(root)) {
        root = opt.fallbackRoot;
        usedFallback = true;
        if (root.empty() || !DirWritable(root)) return false;
    }
    g_sessionRoot = root;
    // The current session's folder is created right after this, so keep one fewer old folder: MaxSessions counts
    // the current session too (3.C acceptance 6: MaxSessions=3 and 5 launches leave exactly 3 folders).
    PruneOldSessions(root, g_maxSessions > 0 ? g_maxSessions - 1 : 0, g_maxTotalMB);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    DWORD pid = GetCurrentProcessId();
    g_session.id = [&] {
        char b[64];
        snprintf(b, sizeof(b), "%04u-%02u-%02u_%02u-%02u-%02u_pid%lu", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond, static_cast<unsigned long>(pid));
        return std::string(b);
    }();
    g_session.root = root;
    g_session.dir = root + L"\\" + FormatSessionId(st, pid);
    MakeDirRecursive(g_session.dir);

    g_startTick.store(GetTickCount64(), std::memory_order_relaxed);
    g_rotateIndex = 0;
    g_fileBytes = 0;
    g_queuedFileBytes = 0;
    g_filePart = 0;
    std::wstring file = g_session.dir + L"\\events.jsonl";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_fileHandle.store(h);

    g_wantExit = false;
    g_running.store(true, std::memory_order_release);
    g_writerThread = CreateThread(nullptr, 0, &WriterMain, nullptr, 0, nullptr);

    if (usedFallback)
        Rec("session", Level::Warn, "primary log directory unavailable, using fallback").Str("dir", wf::game::Narrow(root)).Emit();
    return true;
}

void ShutdownForTests() {
    if (!g_running.exchange(false)) return;
    {
        std::lock_guard lk(g_queueMx);
        g_wantExit = true;
        g_wakeWriter.notify_all();
    }
    if (g_writerThread) {
        WaitForSingleObject(g_writerThread, 5000);
        CloseHandle(g_writerThread);
        g_writerThread = nullptr;
    }
    HANDLE h = g_fileHandle.exchange(INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    delete g_filter.exchange(nullptr);
    {
        std::lock_guard lk(g_queueMx);
        g_queue.clear();
    }
    {
        std::lock_guard lk(g_tailMx);
        g_tailRing.clear();
    }
    g_seq.store(1);
    g_recordsAccepted.store(0);
    g_recordsDropped.store(0);
    g_droppedTotal.store(0);
    for (auto& b : g_emitHist) b.store(0);
    g_bytesWritten.store(0);
    g_filesRotated.store(0);
    g_session = Session{};
}

}  // namespace internal
}  // namespace wf::jlog
