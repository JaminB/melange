// Offline self-test for component C's structured logging core (src/core/jlog.cpp). Runs standalone, without
// the game or WUMFix.asi: exercises the writer thread, session folders, rotation, the level filter, the tail
// ring and the crash-flush path against a scratch directory under %TEMP%. See docs/m0-design.md SS3 "C"
// acceptance items 1, 5, 6, 7, and the M0 report for component C ("offline tests" section).
//
// Not a frozen contract; only wumfix/jlog.h (the public API under test) and two small internal headers used to
// start/stop sessions in-process (jlog_internal.h) and to test the bus deny/allow list in isolation from the
// event bus itself (jlog_bus_filter.h).
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "core/jlog_bus_filter.h"
#include "core/jlog_internal.h"
#include "wumfix/jlog.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* what) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
}

// ------------------------------------------------------------------------------------------- tiny JSON reader
// Just enough to validate our own output: balanced objects/arrays, correctly-escaped strings, and extraction
// of top-level "key":value fields as raw substrings. Not a general-purpose parser.
size_t SkipString(const std::string& s, size_t i) {  // s[i] == '"'; returns index just past the closing quote
    ++i;
    while (i < s.size() && s[i] != '"') {
        if (s[i] == '\\') ++i;
        ++i;
    }
    return i + 1;
}

// Returns the index just past the value starting at s[i] (a string, number, object, array, true/false/null).
size_t SkipValue(const std::string& s, size_t i) {
    if (s[i] == '"') return SkipString(s, i);
    if (s[i] == '{' || s[i] == '[') {
        char open = s[i], close = open == '{' ? '}' : ']';
        int depth = 1;
        ++i;
        while (i < s.size() && depth > 0) {
            if (s[i] == '"')
                i = SkipString(s, i);
            else {
                if (s[i] == open) ++depth;
                else if (s[i] == close) --depth;
                ++i;
            }
        }
        return i;
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']') ++i;
    return i;
}

// Parses a top-level JSON object into raw (still-JSON-encoded) field strings. Returns false on anything that
// doesn't look like a well-formed object, which is exactly what we want a validity check to catch.
bool ParseTopObject(const std::string& line, std::map<std::string, std::string>& out) {
    if (line.empty() || line.front() != '{' || line.back() != '}') return false;
    size_t i = 1;
    while (i < line.size() - 1) {
        while (i < line.size() && (line[i] == ' ' || line[i] == ',')) ++i;
        if (i >= line.size() - 1) break;
        if (line[i] != '"') return false;
        size_t keyEnd = SkipString(line, i);
        std::string key = line.substr(i + 1, keyEnd - i - 2);
        if (keyEnd >= line.size() || line[keyEnd] != ':') return false;
        size_t valStart = keyEnd + 1;
        size_t valEnd = SkipValue(line, valStart);
        if (valEnd > line.size() - 1) return false;
        out[key] = line.substr(valStart, valEnd - valStart);
        i = valEnd;
    }
    return true;
}

std::wstring TempDir(const wchar_t* tag) {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%swumfix_selftest_%s_%lu", base, tag, GetTickCount());
    return dir;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring path = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveTree(path);
            else
                DeleteFileW(path.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

std::vector<std::string> ReadLines(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// ------------------------------------------------------------------------------------------- test groups
void TestSchemaAndTail() {
    std::wstring dir = TempDir(L"schema");
    wf::jlog::internal::Options opt;
    opt.rootOverride = dir;  // force the sandbox, not the real Documents\WUMFix\logs (see internal::Init())
    opt.levelsSpec = "*:trace";
    Check(wf::jlog::internal::Init(opt), "schema: Init succeeds");

    for (int i = 0; i < 5; ++i) {
        wf::jlog::Rec("test", wf::jlog::Level::Info, "hello")
            .Int("i", i)
            .Str("s", "a\"b\\c\nd")
            .Float("f", 1.5)
            .Bool("b", true)
            .Emit();
    }
    Check(wf::jlog::Flush(2000), "schema: Flush completes");

    std::vector<wf::jlog::Line> tail;
    size_t n = wf::jlog::Tail(0, tail, 100);
    Check(n == 5, "schema: Tail(0) returns all 5 records");
    if (n == 5) {
        std::vector<wf::jlog::Line> more;
        Check(wf::jlog::Tail(tail.back().seq, more, 100) == 0, "schema: Tail(lastSeq) returns nothing further");
    }

    auto lines = ReadLines(wf::jlog::CurrentSession().dir + L"\\events.jsonl");
    Check(lines.size() == 5, "schema: events.jsonl has 5 lines");
    uint64_t lastSeq = 0;
    bool first = true;
    static const char* kRequired[] = {"v", "seq", "t", "wall", "frame", "tid", "lvl", "cat", "msg"};
    for (const auto& line : lines) {
        std::map<std::string, std::string> fields;
        bool ok = ParseTopObject(line, fields);
        Check(ok, "schema: line parses as a well-formed JSON object");
        if (!ok) continue;
        for (const char* k : kRequired) Check(fields.count(k) != 0, "schema: required key present");
        uint64_t seq = fields.count("seq") ? strtoull(fields["seq"].c_str(), nullptr, 10) : 0;
        Check(first || seq > lastSeq, "schema: seq strictly increasing");
        lastSeq = seq;
        first = false;
    }

    auto stats = wf::jlog::GetStats();
    Check(stats.records == 5, "schema: GetStats().records == 5");
    Check(stats.dropped == 0, "schema: GetStats().dropped == 0");

    wf::jlog::internal::ShutdownForTests();
    RemoveTree(dir);
}

void TestLevelFilter() {
    std::wstring dir = TempDir(L"levels");
    wf::jlog::internal::Options opt;
    opt.rootOverride = dir;  // force the sandbox, not the real Documents\WUMFix\logs (see internal::Init())
    opt.levelsSpec = "*:info,verbose:trace,quiet:error";
    Check(wf::jlog::internal::Init(opt), "levels: Init succeeds");

    Check(wf::jlog::Enabled("other", wf::jlog::Level::Info), "levels: default category allows info");
    Check(!wf::jlog::Enabled("other", wf::jlog::Level::Debug), "levels: default category blocks debug");
    Check(wf::jlog::Enabled("verbose", wf::jlog::Level::Trace), "levels: per-category override allows trace");
    Check(!wf::jlog::Enabled("quiet", wf::jlog::Level::Warn), "levels: per-category override blocks warn");
    Check(wf::jlog::Enabled("quiet", wf::jlog::Level::Error), "levels: per-category override allows error");

    wf::jlog::internal::ShutdownForTests();
    RemoveTree(dir);
}

void TestRotation() {
    std::wstring dir = TempDir(L"rotate");
    wf::jlog::internal::Options opt;
    opt.rootOverride = dir;  // force the sandbox, not the real Documents\WUMFix\logs (see internal::Init())
    opt.levelsSpec = "*:trace";
    opt.maxFileMB = 1;
    Check(wf::jlog::internal::Init(opt), "rotate: Init succeeds");

    std::string filler(120, 'x');
    for (int i = 0; i < 12000; ++i) wf::jlog::Rec("test", wf::jlog::Level::Info, "filler").Str("pad", filler).Emit();
    Check(wf::jlog::Flush(5000), "rotate: Flush completes");

    Check(GetFileAttributesW((wf::jlog::CurrentSession().dir + L"\\events.1.jsonl").c_str()) != INVALID_FILE_ATTRIBUTES,
          "rotate: events.1.jsonl exists after exceeding MaxFileMB");
    Check(wf::jlog::GetStats().filesRotated >= 1, "rotate: GetStats().filesRotated >= 1");

    wf::jlog::internal::ShutdownForTests();
    RemoveTree(dir);
}

void TestSessionPruning() {
    std::wstring dir = TempDir(L"prune");
    CreateDirectoryW(dir.c_str(), nullptr);
    // Five fake old session folders, oldest-looking names first (date-prefixed, like the real ones).
    const wchar_t* fake[] = {L"2020-01-01_00-00-00_pid1", L"2020-01-02_00-00-00_pid1", L"2020-01-03_00-00-00_pid1",
                             L"2020-01-04_00-00-00_pid1", L"2020-01-05_00-00-00_pid1"};
    for (auto* name : fake) CreateDirectoryW((dir + L"\\" + name).c_str(), nullptr);

    wf::jlog::internal::Options opt;
    opt.rootOverride = dir;  // force the sandbox, not the real Documents\WUMFix\logs (see internal::Init())
    opt.levelsSpec = "*:info";
    opt.maxSessions = 3;
    opt.maxTotalMB = 512;
    Check(wf::jlog::internal::Init(opt), "prune: Init succeeds");

    int survivors = 0;
    for (auto* name : fake)
        if (GetFileAttributesW((dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES) ++survivors;
    Check(survivors == 3, "prune: exactly MaxSessions old folders survive");
    Check(GetFileAttributesW(wf::jlog::CurrentSession().dir.c_str()) != INVALID_FILE_ATTRIBUTES,
          "prune: the new current session folder exists on top of the kept old ones");

    wf::jlog::internal::ShutdownForTests();
    RemoveTree(dir);
}

void TestCrashFlush() {
    std::wstring dir = TempDir(L"crash");
    wf::jlog::internal::Options opt;
    opt.rootOverride = dir;  // force the sandbox, not the real Documents\WUMFix\logs (see internal::Init())
    opt.levelsSpec = "*:info";
    Check(wf::jlog::internal::Init(opt), "crash: Init succeeds");

    wf::jlog::Rec("test", wf::jlog::Level::Info, "before-crash").Emit();
    // FlushFromCrash() only try-locks the queue (see jlog.cpp), so it races the normal writer thread's own
    // 100ms tick; retry briefly rather than assume our call is the one that won the race.
    bool found = false;
    for (int attempt = 0; attempt < 20 && !found; ++attempt) {
        wf::jlog::FlushFromCrash();
        for (auto& l : ReadLines(wf::jlog::CurrentSession().dir + L"\\events.jsonl"))
            if (l.find("before-crash") != std::string::npos) found = true;
        if (!found) Sleep(10);
    }
    Check(found, "crash: FlushFromCrash() writes the pending record without a normal Flush()");

    wf::jlog::internal::ShutdownForTests();
    RemoveTree(dir);
}

void TestBadPathFallback() {
    // '?' is never valid in a Windows path, so this reliably fails regardless of which drive letters exist
    // (docs/m0-design.md SS3 "C" acceptance item 11: "Dir=Q:\nope falls back ... with a Warn record").
    std::wstring badRoot = L"C:\\wumfix_selftest_??_invalid";
    std::wstring fallback = TempDir(L"fallback");
    wf::jlog::internal::Options opt;
    opt.rootOverride = badRoot;
    opt.fallbackRoot = fallback;
    opt.levelsSpec = "*:info";
    Check(wf::jlog::internal::Init(opt), "badpath: Init succeeds via the fallback root");
    Check(wf::jlog::CurrentSession().root == fallback, "badpath: CurrentSession().root is the fallback, not the bad one");
    Check(wf::jlog::Flush(2000), "badpath: Flush completes");

    auto lines = ReadLines(wf::jlog::CurrentSession().dir + L"\\events.jsonl");
    Check(!lines.empty() && lines.front().find("\"lvl\":\"warn\"") != std::string::npos &&
              lines.front().find("fallback") != std::string::npos,
          "badpath: the first record is a warn about the fallback");

    wf::jlog::internal::ShutdownForTests();
    RemoveTree(fallback);
}

void TestBusFilter() {
    wf::jlog::busfilter::Init("Camera.HasUpdated,Land.CheckVoxel", "");
    Check(!wf::jlog::busfilter::ShouldLog("Camera.HasUpdated"), "busfilter: default deny list blocks a listed name");
    Check(wf::jlog::busfilter::ShouldLog("GameLogic.Turn.Started"), "busfilter: an unlisted name is logged");

    wf::jlog::busfilter::SetAllow("Camera.HasUpdated", true);
    Check(wf::jlog::busfilter::ShouldLog("Camera.HasUpdated"), "busfilter: allow overrides deny");

    wf::jlog::busfilter::SetAllow("Camera.HasUpdated", false);
    Check(!wf::jlog::busfilter::ShouldLog("Camera.HasUpdated"), "busfilter: removing the allow restores the deny");

    wf::jlog::busfilter::Init("EventDeny,from,ini", "EventAllow,from,ini");
    Check(wf::jlog::busfilter::IsDenied("from"), "busfilter: Init() parses comma-separated deny list");
    Check(wf::jlog::busfilter::IsAllowed("from"), "busfilter: Init() parses comma-separated allow list");
}

}  // namespace

int main() {
    TestSchemaAndTail();
    TestLevelFilter();
    TestRotation();
    TestSessionPruning();
    TestCrashFlush();
    TestBadPathFallback();
    TestBusFilter();

    std::cout << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
