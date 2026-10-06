// Offline self-test for the rolling recorder's writer and library (src/wormsign/{writer,library,records}.cpp):
// no game, no hooks -- it drives the .wsr writer and the library's indexing/retention/export straight from
// synthetic data. Exit code 0 = all passed.
#include <shlobj.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "tools/json_mini.h"
#include "tools/json_read.h"
#include "wormsign/format.h"
#include "wormsign/library.h"
#include "wormsign/records.h"
#include "wormsign/writer.h"

namespace wsr = melange::wormsign::wsr;
namespace rec = melange::wormsign::rec;
namespace lib = melange::wormsign::library;

namespace {
int g_fail = 0, g_pass = 0;
void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what);
    }
}

std::wstring TempDir() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%swsrtest_%lu", base, GetCurrentProcessId());
    SHCreateDirectoryExW(nullptr, dir, nullptr);
    return dir;
}
void RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring p = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirRecursive(p);
        else DeleteFileW(p.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir.c_str());
}
void SetMTime(const std::wstring& path, int secondsAgo) {
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    ULARGE_INTEGER now;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    now.QuadPart -= static_cast<uint64_t>(secondsAgo) * 10000000ull;
    FILETIME ft{now.LowPart, now.HighPart};
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}

// ---- records.h round trip ----
void TestRecordsRoundTrip() {
    std::vector<uint8_t> buf;
    struct Want { uint8_t type; uint16_t id; uint32_t a, b, time, callT, caller; std::string str; };
    std::vector<Want> want = {
        {0, 1, 0, 0, 100, 100, 0x11111111, ""},
        {5, 7, 0x2222, 0, 200, 180, 0x33333333, "hello world"},
        {2, 9, 42, 43, 300, 260, 0x44444444, std::string(255, 'x')},  // max strLen
        {1, 0xffff, 7, 0, 400, 380, 0, ""},
    };
    for (auto& w : want) rec::AppendInput(buf, rec::Input{w.type, w.id, w.a, w.b, w.time, w.callT, w.caller, w.str});
    std::vector<rec::Input> got;
    Expect(rec::DecodeInputs(buf.data(), buf.size(), &got), "input records decode");
    Expect(got.size() == want.size(), "input record count");
    for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
        const auto& w = want[i];
        const auto& g = got[i];
        Expect(g.type == w.type && g.id == w.id && g.a == w.a && g.b == w.b && g.time == w.time && g.callT == w.callT &&
                   g.caller == w.caller && g.str == w.str,
               "input record field match");
    }

    // A truncated buffer must be reported as malformed, not silently short-counted.
    std::vector<uint8_t> bad(buf.begin(), buf.end() - 3);
    std::vector<rec::Input> badOut;
    Expect(!rec::DecodeInputs(bad.data(), bad.size(), &badOut),
           "truncated input buffer detected");

    std::vector<uint8_t> rbuf;
    rec::AppendRemoteInput(rbuf, 111, 22, 333, 44);
    rec::AppendRemoteInput(rbuf, 555, 66, 777, 88);
    int count = 0;
    rec::ForEachRemoteInput(rbuf.data(), rbuf.size(), [&](uint32_t arrivedT, uint16_t id, uint32_t time, uint32_t a) {
        if (count == 0) Expect(arrivedT == 111 && id == 22 && time == 333 && a == 44, "remote input #0");
        if (count == 1) Expect(arrivedT == 555 && id == 66 && time == 777 && a == 88, "remote input #1");
        ++count;
    });
    Expect(count == 2, "remote input count");
}

// ---- writer.cpp: threaded wrapper over format.cpp ----
void TestWriterCloseAndAbandon(const std::wstring& dir) {
    const std::wstring path = dir + L"\\writer_close.wsr";
    {
        melange::wormsign::writer::Writer w;
        Expect(w.Open(path), "writer open (close case)");
        std::string head = R"({"format":1})";
        Expect(w.Enqueue(wsr::kHEAD, head.data(), head.size(), true), "enqueue HEAD");
        std::vector<uint8_t> tick(rec::kTickBytes * 3, 0x42);
        Expect(w.Enqueue(wsr::kTICK, tick.data(), tick.size(), true, 1, 3), "enqueue TICK");
        Expect(w.Close(), "writer close");
    }
    wsr::Reader r;
    std::string err;
    Expect(r.OpenFile(path, &err), "reopen closed file");
    Expect(r.Complete(), "closed file is complete");

    const std::wstring path2 = dir + L"\\writer_abandon.wsr";
    {
        melange::wormsign::writer::Writer w;
        Expect(w.Open(path2), "writer open (abandon case)");
        std::string head = R"({"format":1})";
        w.Enqueue(wsr::kHEAD, head.data(), head.size(), true);
        w.Flush();  // make sure the HEAD chunk actually reaches disk before we simulate the crash
        w.Abandon();
    }
    wsr::Reader r2;
    Expect(r2.OpenFile(path2, &err), "reopen abandoned file");
    Expect(!r2.Complete(), "abandoned file is marked incomplete");
    Expect(r2.Header() == R"({"format":1})", "abandoned file kept its HEAD chunk");
}

// A crash leaves the file as the writer last flushed it: the HEAD must be on disk without any Flush or Close (a
// 0-byte .wsr was what a crash 17 s into a match left in 0.4.0), and the crash path's TryEnqueue + FlushWithin put
// the rest there without INDX.
void TestWriterCrashSafety(const std::wstring& dir) {
    const std::wstring path = dir + L"\\writer_crash.wsr";
    melange::wormsign::writer::Writer w;
    Expect(w.Open(path), "writer open (crash case)");
    const std::string head = R"({"format":1,"crash":"test"})";
    w.Enqueue(wsr::kHEAD, head.data(), head.size(), true);
    wsr::Reader r;
    std::string err;
    bool onDisk = false;
    for (int i = 0; i < 200 && !onDisk; ++i) {  // up to 2 s; the writer thread wakes on the enqueue
        onDisk = r.OpenFile(path, &err) && r.Header() == head;
        if (!onDisk) Sleep(10);
    }
    Expect(onDisk, "the HEAD reaches the disk while the writer is still open, with no Flush or Close");
    Expect(onDisk && !r.Complete(), "an open recording reads as incomplete");

    std::vector<uint8_t> tick(rec::kTickBytes * 2, 0x24);
    Expect(w.TryEnqueue(wsr::kTICK, tick.data(), tick.size(), 1, 2), "TryEnqueue on the crash path");
    const char* note = "{\"reason\":\"crash\"}";
    Expect(w.TryEnqueue(wsr::kNOTE, note, strlen(note)), "TryEnqueue the crash NOTE");
    Expect(w.FlushWithin(2000), "FlushWithin confirms the queue is on disk");
    wsr::Reader r2;
    Expect(r2.OpenFile(path, &err) && !r2.Complete() && r2.Chunks().size() == 3,
           "after the crash flush: HEAD, TICK and NOTE on disk, still without INDX");
    w.Abandon();
    Expect(!w.TryEnqueue(wsr::kNOTE, note, strlen(note)) && !w.FlushWithin(10), "a closed writer refuses both");
}

std::string CurrentUserNameUtf8() {
    wchar_t buf[256];
    DWORD n = 256;
    if (!GetUserNameW(buf, &n)) return {};
    char out[256];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof out, nullptr, nullptr);
    return out;
}

// ---- library.cpp: build a minimal-but-real recording directly with wsr::Writer ----
// fakeSteamId is a 17-digit SteamID64-shaped number (redact::HashIdsAndIps only matches 17-digit runs); the
// embedded path uses the *real* current Windows user name, since ExportRedacted reads it live with GetUserNameW.
void MakeRecording(const std::wstring& path, long long fakeSteamId, bool flagged) {
    wsr::Writer w;
    w.Open(path);
    melange::jsonmini::Obj head;
    head.Int("format", 1).Str("exeBuild", "1077").Str("melange", "0.0.0-test").Str("contentHash", "")
        .Int("startUnix", 1700000000).Bool("online", true);
    const std::string headJson = head.End();
    w.Chunk(wsr::kHEAD, headJson.data(), headJson.size(), true);

    std::vector<uint8_t> inpt;
    rec::AppendInput(inpt, rec::Input{1, 5, 0, 0, 100, 100, 0x1000, ""});
    w.Chunk(wsr::kINPT, inpt.data(), inpt.size(), true);

    rec::TickChunk tc;
    for (uint32_t t = 1; t <= 10; ++t) {
        melange::wormsign::TickHash h{};
        h.tick = t;
        h.engine = 0x0707070707070707ull;
        tc.Add(h);
    }
    const std::vector<uint8_t> tick = tc.Take();
    w.Chunk(wsr::kTICK, tick.data(), tick.size(), true, 1, 10);

    melange::jsonmini::Obj setp;
    setp.Str("landFile", "cropcircle-w3d.xan");
    const std::string setpJson = setp.End();
    w.Chunk(wsr::kSETP, setpJson.data(), setpJson.size(), true);

    if (fakeSteamId) {
        char note[256];
        snprintf(note, sizeof note, "{\"reason\":\"match-end\",\"peer\":%lld,\"path\":\"C:\\\\Users\\\\%s\\\\x\"}", fakeSteamId,
                 CurrentUserNameUtf8().c_str());
        w.Chunk(wsr::kNOTE, note, strlen(note), true);
    }
    if (flagged) {
        // A peer-sourced divergence, matching what OnDivergenceCb (recorder.cpp) actually writes: retention
        // exempts this (a real cross-machine desync), never a replay's own "source":"replay" DVRG.
        const char* dvrg = "{\"source\":\"peer\",\"tick\":5}";
        w.Chunk(wsr::kDVRG, dvrg, strlen(dvrg), false);
    }
    w.Close();
}

// The redacted copy of a recording a crash cut off: as far as it reads, redacted, still incomplete; an empty file or
// one cut inside its HEAD is refused with a reason, never copied raw.
void TestExportCrashCut(const std::wstring& baseDir) {
    const std::wstring full = baseDir + L"\\cut_full.wsr", cut = baseDir + L"\\cut.wsr", out = baseDir + L"\\cut_out.wsr";
    MakeRecording(full, 76561198000000077LL, false);
    std::vector<uint8_t> bytes;
    {
        FILE* f = _wfopen(full.c_str(), L"rb");
        uint8_t buf[4096];
        size_t n;
        while (f && (n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        if (f) fclose(f);
    }
    wsr::Reader src;
    std::string err;
    Expect(src.OpenMemory(bytes, &err) && src.Chunks().size() == 6, "the source recording reads (5 chunks and INDX)");
    // Cut inside the last chunk before INDX (the NOTE with the SteamID): HEAD, INPT, TICK and SETP survive.
    const auto& chunks = src.Chunks();
    const size_t cutAt = chunks.size() >= 2 ? static_cast<size_t>(chunks[chunks.size() - 2].offset) + 10 : 0;
    {
        FILE* f = _wfopen(cut.c_str(), L"wb");
        if (f) fwrite(bytes.data(), 1, cutAt, f), fclose(f);
    }
    bool incomplete = false;
    Expect(lib::ExportRedacted(cut, out, &err, "salt", &incomplete), ("export a cut recording: " + err).c_str());
    Expect(incomplete, "the export says the recording was incomplete");
    wsr::Reader r;
    Expect(r.OpenFile(out, &err) && !r.Complete() && r.Chunks().size() == 4,
           "the copy holds the four whole chunks and stays incomplete");
    Expect(r.Header().find("1077") != std::string::npos, "the copy's HEAD is the recording's");

    const std::wstring empty = baseDir + L"\\empty.wsr";
    if (FILE* f = _wfopen(empty.c_str(), L"wb")) fclose(f);
    err.clear();
    Expect(!lib::ExportRedacted(empty, out, &err) && err.rfind("empty: the game crashed", 0) == 0,
           "an empty recording is refused as empty");
    const std::wstring magic = baseDir + L"\\magic.wsr";
    if (FILE* f = _wfopen(magic.c_str(), L"wb")) {
        fwrite(bytes.data(), 1, 12, f);
        fclose(f);
    }
    err.clear();
    Expect(!lib::ExportRedacted(magic, out, &err) && err.rfind("no complete header", 0) == 0,
           "a recording cut inside its HEAD is refused as headerless");
}

void TestLibraryRetentionAndExport(const std::wstring& baseDir) {
    const std::wstring dir = baseDir + L"\\replays";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    lib::SetReplaysDirForTests(dir);

    // 5 recordings, oldest to newest, one flagged. Names embed an index so mtime ties don't reorder them.
    std::vector<std::wstring> paths;
    for (int i = 0; i < 5; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"wsr-idx%d-m%d.wsr", i, i);
        std::wstring p = dir + L"\\" + name;
        MakeRecording(p, 76561198000000000LL + i, /*flagged=*/i == 1);
        SetMTime(p, (5 - i) * 60);  // index 0 is oldest (5 min ago), index 4 is newest (1 min ago)
        paths.push_back(p);
    }
    lib::Rescan();
    melange::wormsign::ReplayInfo infos[16]{};
    int total = melange::wormsign::Library(infos, 16);
    Expect(total == 5, "library sees all 5 recordings before retention");
    for (int i = 0; i < total && i < 16; ++i) Expect(infos[i].ticks == 10, "each recording indexed 10 ticks");

    // Pin the oldest (index 0) so it survives even though it is outside KeepMatches=3.
    Expect(melange::wormsign::Pin(paths[0].c_str(), true), "pin the oldest recording");

    lib::Configure(/*keepMatches=*/3, /*maxMB=*/200);
    total = melange::wormsign::Library(infos, 16);
    // Kept: pinned #0, flagged #1, and the 3 newest unpinned/unflagged (#2,#3,#4) = 5 total, nothing pruned.
    Expect(total == 5, "pinned + flagged are exempt from KeepMatches, nothing pruned yet");
    Expect(GetFileAttributesW(paths[0].c_str()) != INVALID_FILE_ATTRIBUTES, "pinned recording still on disk");
    Expect(GetFileAttributesW(paths[1].c_str()) != INVALID_FILE_ATTRIBUTES, "flagged recording still on disk");

    // Add two more unflagged recordings so the unpinned/unflagged set (3,4,5,6) exceeds KeepMatches=3.
    for (int i = 5; i < 7; ++i) {
        wchar_t name[64];
        swprintf_s(name, L"wsr-idx%d-m%d.wsr", i, i);
        std::wstring p = dir + L"\\" + name;
        MakeRecording(p, 0, false);
        SetMTime(p, (7 - i) * 10);
        paths.push_back(p);
    }
    lib::Rescan();
    lib::Configure(3, 200);
    total = melange::wormsign::Library(infos, 16);
    Expect(total == 5, "KeepMatches=3 + 2 exempt = 5 survive");
    Expect(GetFileAttributesW(paths[2].c_str()) == INVALID_FILE_ATTRIBUTES, "oldest unpinned recording pruned");
    Expect(GetFileAttributesW(paths[0].c_str()) != INVALID_FILE_ATTRIBUTES, "pinned recording survives pruning");
    Expect(GetFileAttributesW(paths[1].c_str()) != INVALID_FILE_ATTRIBUTES, "flagged recording survives pruning");
    Expect(GetFileAttributesW(paths[6].c_str()) != INVALID_FILE_ATTRIBUTES, "newest recording survives pruning");

    // Export redaction: the fake path/steamid-shaped text must not survive into the exported copy.
    const std::wstring exported = baseDir + L"\\exported.wsr";
    std::string err;
    Expect(lib::ExportRedacted(paths[0].c_str(), exported, &err), ("export redacted: " + err).c_str());
    wsr::Reader r;
    Expect(r.OpenFile(exported, &err), "reopen exported file");
    const std::string me = CurrentUserNameUtf8();
    const bool checkUser = me.size() >= 3;  // redact::RedactUserName skips names shorter than 3 chars
    bool sawNote = false, tickUnchanged = false;
    r.ForEach(wsr::kNOTE, [&](const wsr::ChunkRef&, const std::vector<uint8_t>& p) {
        sawNote = true;
        std::string s(p.begin(), p.end());
        if (checkUser) {
            Expect(s.find(me) == std::string::npos, "exported NOTE no longer contains the Windows user name");
            Expect(s.find("%USERNAME%") != std::string::npos, "exported NOTE carries the %USERNAME% token");
        }
        Expect(s.find("76561198000000000") == std::string::npos, "exported NOTE no longer contains the raw SteamID");
    });
    Expect(sawNote, "exported file still has a NOTE chunk");
    r.ForEach(wsr::kTICK, [&](const wsr::ChunkRef&, const std::vector<uint8_t>& p) {
        tickUnchanged = p.size() == 4 + (1 + rec::kTickBytes) * 10 && p[0] == 1 && p[5] == 0x7;
    });
    Expect(tickUnchanged, "exported TICK payload is untouched (no identity data there to redact)");
}
}  // namespace

int main() {
    const std::wstring tmp = TempDir();
    TestRecordsRoundTrip();
    TestWriterCloseAndAbandon(tmp);
    TestWriterCrashSafety(tmp);
    TestExportCrashCut(tmp);
    TestLibraryRetentionAndExport(tmp);
    RemoveDirRecursive(tmp);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
