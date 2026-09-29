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
namespace rec = melange::wormsign::records;
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
    for (auto& w : want) rec::AppendInput(buf, w.type, w.id, w.a, w.b, w.time, w.callT, w.caller, w.str.c_str());
    size_t i = 0;
    const int64_t n = rec::ForEachInput(buf.data(), buf.size(), [&](uint8_t type, uint16_t id, uint32_t a, uint32_t b,
                                                                      uint32_t time, uint32_t callT, uint32_t caller,
                                                                      const char* str, uint8_t strLen) {
        if (i >= want.size()) return;
        auto& w = want[i];
        Expect(type == w.type && id == w.id && a == w.a && b == w.b && time == w.time && callT == w.callT &&
                   caller == w.caller && strLen == w.str.size() && std::string(str, strLen) == w.str,
               "input record field match");
        ++i;
    });
    Expect(n == static_cast<int64_t>(want.size()), "input record count");
    Expect(i == want.size(), "input record callback count");

    // A truncated buffer must be reported as malformed, not silently short-counted.
    std::vector<uint8_t> bad(buf.begin(), buf.end() - 3);
    Expect(rec::ForEachInput(bad.data(), bad.size(), [](uint8_t, uint16_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                         uint32_t, const char*, uint8_t) {}) < 0,
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
    rec::AppendInput(inpt, 1, 5, 0, 0, 100, 100, 0x1000, "");
    w.Chunk(wsr::kINPT, inpt.data(), inpt.size(), true);

    std::vector<uint8_t> tick(rec::kTickBytes * 10, 0x7);
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
        const char* dvrg = "\x01\x02\x03\x04";
        w.Chunk(wsr::kDVRG, dvrg, 4, false);
    }
    w.Close();
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
        tickUnchanged = p.size() == rec::kTickBytes * 10 && p[0] == 0x7;
    });
    Expect(tickUnchanged, "exported TICK payload is untouched (no identity data there to redact)");
}
}  // namespace

int main() {
    const std::wstring tmp = TempDir();
    TestRecordsRoundTrip();
    TestWriterCloseAndAbandon(tmp);
    TestLibraryRetentionAndExport(tmp);
    RemoveDirRecursive(tmp);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
