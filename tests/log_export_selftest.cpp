// Offline self-test for the log export's core (src/tools/log_export_core.cpp): no game. Builds a fixture tree with
// several session folders, pids, replays, dumps and engine logs, runs the exports Melange.exe and the game run, then
// reads the zips back: which game was chosen, what came with it, the redaction and manifest.json.
// Exit code 0 = all passed.
#include <shlobj.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "miniz.h"
#include "tools/json_read.h"
#include "tools/log_export_core.h"
#include "wormsign/format.h"

namespace ex = melange::exporter::core;
namespace json = melange::json;
namespace wsr = melange::wormsign::wsr;

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

constexpr uint64_t kSecond = 10'000'000ull;
constexpr uint64_t kMinute = 60 * kSecond;
constexpr uint64_t kHour = 60 * kMinute;

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring TempDir() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%slogexporttest_%lu", base, GetCurrentProcessId());
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

void SetMTime(const std::wstring& path, uint64_t ticksUtc) {
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME ft{static_cast<DWORD>(ticksUtc), static_cast<DWORD>(ticksUtc >> 32)};
    SetFileTime(f, nullptr, nullptr, &ft);
    CloseHandle(f);
}

void WriteText(const std::wstring& path, const std::string& text, uint64_t mtime = 0) {
    size_t slash = path.find_last_of(L'\\');
    SHCreateDirectoryExW(nullptr, path.substr(0, slash).c_str(), nullptr);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    if (mtime) SetMTime(path, mtime);
}

// A complete .wsr whose HEAD carries a SteamID, an IP and the user's profile path.
void WriteWsr(const std::wstring& path, const std::string& user, uint64_t mtime) {
    const std::string head = "{\"format\":1,\"peer\":\"76561198000000001\",\"host\":\"10.1.2.3:27015\",\"dir\":\"C:\\\\Users\\\\" +
                             user + "\\\\Documents\"}";
    wsr::Writer w;
    w.Open(path);
    w.Chunk(wsr::kHEAD, head.data(), head.size());
    const uint8_t inpt[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    w.Chunk(wsr::kINPT, inpt, sizeof inpt, true, 1, 2);
    w.Close();
    SetMTime(path, mtime);
}

uint64_t Start(const wchar_t* sessionName) {
    uint32_t pid = 0;
    uint64_t t = 0;
    ex::ParseSessionName(sessionName, &pid, &t);
    return t;
}

uint64_t Replay(const wchar_t* name) {
    uint32_t pid = 0;
    uint64_t t = 0;
    ex::ParseReplayName(name, &pid, &t);
    return t;
}

struct Zip {
    std::vector<std::string> names;
    mz_zip_archive z{};
    bool ok = false;
    explicit Zip(const std::wstring& path) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (!f) return;
        std::vector<char> bytes;
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + n);
        fclose(f);
        data_ = std::move(bytes);
        ok = mz_zip_reader_init_mem(&z, data_.data(), data_.size(), 0) != 0;
        if (!ok) return;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i) {
            char name[512];
            mz_zip_reader_get_filename(&z, i, name, sizeof name);
            names.push_back(name);
        }
    }
    ~Zip() {
        if (ok) mz_zip_reader_end(&z);
    }
    bool Has(const std::string& name) const { return std::find(names.begin(), names.end(), name) != names.end(); }
    bool HasPrefix(const std::string& prefix) const {
        for (const auto& n : names)
            if (n.compare(0, prefix.size(), prefix) == 0) return true;
        return false;
    }
    size_t Count(const std::string& prefix) const {
        size_t c = 0;
        for (const auto& n : names) c += n.compare(0, prefix.size(), prefix) == 0;
        return c;
    }
    std::string Read(const std::string& name) {
        size_t sz = 0;
        void* p = ok ? mz_zip_reader_extract_file_to_heap(&z, name.c_str(), &sz, 0) : nullptr;
        if (!p) return {};
        std::string s(static_cast<const char*>(p), sz);
        mz_free(p);
        return s;
    }

private:
    std::vector<char> data_;
};

bool Contains(const std::string& hay, const std::string& needle) {
    return !needle.empty() && hay.find(needle) != std::string::npos;
}
bool ContainsCI(std::string hay, std::string needle) {
    for (auto& c : hay) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (auto& c : needle) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return Contains(hay, needle);
}

// The first "hash:xxxxxxxx" in `text`, "" if none.
std::string FirstHash(const std::string& text) {
    size_t p = text.find("hash:");
    return p == std::string::npos || p + 13 > text.size() ? std::string() : text.substr(p, 13);
}

void TestParsers() {
    uint32_t pid = 0;
    uint64_t t = 0;
    Expect(ex::ParseSessionName(L"2026-01-02_12-00-00_pid200", &pid, &t) && pid == 200 && t != 0, "session name parses");
    Expect(!ex::ParseSessionName(L"2026-01-02_12-00-00_pidX", &pid, &t), "session name with no pid digits is rejected");
    Expect(!ex::ParseSessionName(L"2026-13-02_12-00-00_pid1", &pid, &t), "month 13 is rejected");
    Expect(!ex::ParseSessionName(L"2026-01-02_12-00-00_pid99999999999", &pid, &t), "a pid over 32 bits is rejected");
    Expect(!ex::ParseSessionName(L"notes", &pid, &t), "an unrelated folder is not a session");
    Expect(ex::ParseReplayName(L"wsr-20260102-120500-p200-m1.wsr", &pid, &t) && pid == 200, "wsr name parses");
    Expect(ex::ParseReplayName(L"desync-20260102-121000-p4294967295-m1-t500.zip", &pid, &t) && pid == 4294967295u,
           "desync name parses, the largest pid included");
    Expect(!ex::ParseReplayName(L"wsr-20260102-120500-p-m1.wsr", &pid, &t), "replay name without a pid is rejected");
    Expect(!ex::ParseReplayName(L"wsr-20260102-120500-p12", &pid, &t), "replay name cut off after the pid is rejected");
    Expect(!ex::ParseReplayName(L"other-20260102-120500-p12-m1.wsr", &pid, &t), "unknown prefix is rejected");
    const std::wstring name = ex::DefaultZipName();
    Expect(name.size() == 32 && name.compare(0, 13, L"Melange-logs-") == 0 && name.compare(28, 4, L".zip") == 0,
           "DefaultZipName is Melange-logs-YYYYMMDD-HHMMSS.zip");
}

struct Fixture {
    std::wstring root, logs, fallback, data, game, replays, applocal;
    std::string user, computer;
    uint64_t s200 = 0, s100 = 0;
};

Fixture Build(const std::wstring& tmp) {
    Fixture fx;
    fx.root = tmp;
    fx.logs = tmp + L"\\logs";
    fx.fallback = tmp + L"\\game\\Melange\\logs";
    fx.data = tmp + L"\\game\\Melange";
    fx.game = tmp + L"\\game";
    fx.replays = tmp + L"\\replays";
    fx.applocal = tmp + L"\\applocal";
    wchar_t buf[256];
    DWORD n = 256;
    if (GetUserNameW(buf, &n)) fx.user = Narrow(buf);
    n = 256;
    if (GetComputerNameW(buf, &n)) fx.computer = Narrow(buf);

    fx.s100 = Start(L"2026-01-01_10-00-00_pid100");
    fx.s200 = Start(L"2026-01-02_12-00-00_pid200");
    const uint64_t s150 = Start(L"2026-01-01_11-00-00_pid150");
    const std::string secret = "{\"msg\":\"lobby joined\",\"steam\":\"76561198000000001\",\"ip\":\"192.168.1.20:27015\","
                               "\"path\":\"C:\\\\Users\\\\" + fx.user + "\\\\Documents\",\"pc\":\"" + fx.computer + "\"}\n";
    // The newest game (pid 200) in the primary root; an older one (100) there too; another older one (150) in the
    // <game>\Melange\logs fallback root. Not a session: a folder whose name only looks close.
    WriteText(fx.logs + L"\\2026-01-01_10-00-00_pid100\\events.jsonl", "{\"msg\":\"game 100\"}\n", fx.s100 + kHour);
    WriteText(fx.logs + L"\\2026-01-02_12-00-00_pid200\\events.jsonl", secret, fx.s200 + 2 * kHour);
    WriteText(fx.logs + L"\\2026-01-02_12-00-00_pid200\\events.1.jsonl", "{\"msg\":\"rotated\"}\n", fx.s200 + kHour);
    WriteText(fx.fallback + L"\\2026-01-01_11-00-00_pid150\\events.jsonl", "{\"msg\":\"game 150\"}\n", s150 + kHour);
    WriteText(fx.logs + L"\\2027-01-01_00-00-00_pidabc\\events.jsonl", "{}\n", fx.s200 + 9 * kHour);

    WriteText(fx.data + L"\\Melange.log", "INFO started from C:\\Users\\" + fx.user + "\\Desktop\n", fx.s200 + 2 * kHour);
    WriteText(fx.data + L"\\Melange.prev.log", "INFO the run before\n", fx.s100 + kHour);
    WriteText(fx.data + L"\\dumps\\in-window.dmp", "MDMP", fx.s200 + kHour);
    WriteText(fx.data + L"\\dumps\\in-window-full.dmp", "MDMP", fx.s200 + kHour);
    WriteText(fx.data + L"\\dumps\\other-game.dmp", "MDMP", fx.s100 + 10 * kMinute);

    WriteText(fx.game + L"\\XOM0-" + std::wstring(fx.computer.begin(), fx.computer.end()) + L".log",
              "engine log on " + fx.computer + "\n", fx.s200 + 2 * kHour + 5 * kMinute);
    WriteText(fx.game + L"\\XOM1-OLD.log", "engine log from another day\n", fx.s100);
    WriteText(fx.game + L"\\Melange.ini", "[Logging]\nDir=\n; C:\\Users\\" + fx.user + "\n");
    WriteText(fx.game + L"\\Mods\\thumper-state.json", "{\"enabled\":{\"foo\":true}}");
    WriteText(fx.game + L"\\Mods\\.store\\installed.json", "{\"foo\":{\"version\":\"1.2.3\"}}");
    WriteText(fx.game + L"\\Mods\\foo\\spice.json",
              "{\"spiceVersion\":1,\"id\":\"foo\",\"name\":\"Foo\",\"version\":\"1.2.3\",\"kind\":\"content\"}");
    WriteText(fx.game + L"\\Mods\\broken\\spice.json", "{nope");

    WriteText(fx.applocal + L"\\launcher.log", "launcher at C:\\Users\\" + fx.user + " talking to 192.168.1.20\n");
    WriteText(fx.applocal + L"\\launcher.1.log", "older launcher run\n");

    // Pid 200's recordings and bundle (one over a year older: a reused pid, not this game), pid 100's.
    SHCreateDirectoryExW(nullptr, fx.replays.c_str(), nullptr);
    WriteWsr(fx.replays + L"\\wsr-20260102-120500-p200-m1.wsr", fx.user, Replay(L"wsr-20260102-120500-p200-m1.wsr"));
    WriteWsr(fx.replays + L"\\wsr-20260102-130000-p200-m2.wsr", fx.user, Replay(L"wsr-20260102-130000-p200-m2.wsr"));
    WriteWsr(fx.replays + L"\\wsr-20250101-000000-p200-m1.wsr", fx.user, Replay(L"wsr-20250101-000000-p200-m1.wsr"));
    WriteWsr(fx.replays + L"\\wsr-20260101-100500-p100-m1.wsr", fx.user, Replay(L"wsr-20260101-100500-p100-m1.wsr"));
    WriteText(fx.replays + L"\\desync-20260102-121000-p200-m1-t500.zip", "PK-bundle-200",
              Replay(L"desync-20260102-121000-p200-m1-t500.zip"));
    // The newest bundle by mtime belongs to pid 100: "Save logs as..." takes it, the last-game export must not.
    WriteText(fx.replays + L"\\desync-20260101-100600-p100-m1-t10.zip", "PK-bundle-100", fx.s200 + 3 * kHour);
    return fx;
}

ex::Request LauncherRequest(const Fixture& fx) {
    ex::Request rq;
    rq.scope = ex::Scope::LastGame;
    rq.producer = "launcher";
    rq.src.sessionRoots = {fx.logs, fx.fallback, fx.logs + L"\\"};  // the same root twice: listed once
    rq.src.dataDirs = {fx.game + L"\\scripts\\Melange", fx.data};
    rq.src.gameDir = fx.game;
    rq.src.replaysDir = fx.replays;
    rq.src.launcherLogDir = fx.applocal;
    return rq;
}

void TestSessions(const Fixture& fx) {
    const auto all = ex::FindSessions({fx.logs, fx.fallback, fx.logs + L"\\", fx.root + L"\\missing"});
    Expect(all.size() == 3, "three session folders across two roots, the repeated root counted once");
    Expect(all.size() == 3 && all[0].id == "2026-01-02_12-00-00_pid200" && all[0].pid == 200,
           "the newest session is first");
    Expect(all.size() == 3 && all[1].id == "2026-01-01_11-00-00_pid150" && all[2].pid == 100,
           "sessions from the fallback root sort in by time");
}

void TestLastGameFromLauncher(const Fixture& fx) {
    const std::wstring out = fx.root + L"\\out\\last.zip";
    ex::Result res;
    const bool ok = ex::Export(out, LauncherRequest(fx), &res);
    Expect(ok, "launcher last-game export succeeds");
    Expect(res.sessionId == "2026-01-02_12-00-00_pid200" && res.pid == 200, "the newest session and its pid are chosen");
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    Expect(GetFileAttributesExW(out.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow == res.bytes && res.bytes > 0,
           "Result.bytes is the zip's size");

    Zip z(out);
    Expect(z.ok, "the zip opens");
    Expect(z.Has("logs/sessions/2026-01-02_12-00-00_pid200/events.jsonl") &&
               z.Has("logs/sessions/2026-01-02_12-00-00_pid200/events.1.jsonl"),
           "the chosen session's event files are in");
    Expect(z.Count("logs/sessions/") == 2, "no other session is in");
    Expect(z.Has("replays/wsr-20260102-120500-p200-m1.wsr") && z.Has("replays/wsr-20260102-130000-p200-m2.wsr") &&
               z.Has("replays/desync-20260102-121000-p200-m1-t500.zip"),
           "every recording and bundle of pid 200 is in");
    Expect(z.Count("replays/") == 3, "no other pid's replays, and not the reused pid from a year before");
    Expect(z.Has("dumps/in-window.dmp") && z.Count("dumps/") == 1,
           "only the dump from the game's window (the full dump stays out by default)");
    Expect(z.Has("logs/engine/XOM0-%COMPUTERNAME%.log"), "the engine log nearest the game, computer name redacted");
    Expect(!z.Has("logs/engine/XOM1-OLD.log"), "an engine log from another day stays out when one is in the window");
    Expect(z.Has("logs/Melange.log") && z.Has("logs/Melange.prev.log"), "Melange.log and .prev from the data dir");
    Expect(z.Has("logs/launcher/launcher.log") && z.Has("logs/launcher/launcher.1.log"), "launcher.log and launcher.1.log");
    Expect(z.Has("config/Melange.ini"), "Melange.ini");
    Expect(z.Has("mods/thumper-state.json") && z.Has("mods/store-installed.json") && z.Has("mods/spice.json") &&
               z.Has("mods/plugins.json"),
           "the mods lists");
    Expect(!z.Has("mods/modules.json") && !z.HasPrefix("gpu/"), "no in-game-only providers from the launcher");
    Expect(z.Has("system.json") && z.Has("manifest.json") && z.Has("README.txt"), "system.json, manifest, README");

    const std::string spice = z.Read("mods/spice.json");
    Expect(Contains(spice, "\"id\":\"foo\"") && Contains(spice, "\"version\":\"1.2.3\"") &&
               Contains(spice, "\"dir\":\"broken\",\"spiceJson\":false"),
           "spice.json lists ids and versions, and flags an unreadable manifest");

    // Redaction: the same pass and the same salt over text logs and recordings.
    const std::string events = z.Read("logs/sessions/2026-01-02_12-00-00_pid200/events.jsonl");
    Expect(!Contains(events, "76561198000000001") && !Contains(events, "192.168.1.20"), "ids and IPs are hashed");
    if (fx.user.size() >= 3) {
        Expect(!ContainsCI(events, fx.user) && Contains(events, "%USERNAME%"), "the user name is replaced in events");
        Expect(!ContainsCI(z.Read("logs/launcher/launcher.log"), fx.user) &&
                   !Contains(z.Read("logs/launcher/launcher.log"), "192.168.1.20"),
               "launcher.log is redacted like the rest");
        Expect(!ContainsCI(z.Read("logs/Melange.log"), fx.user), "Melange.log is redacted");
    }
    if (fx.computer.size() >= 3)
        Expect(!ContainsCI(events, fx.computer), "the computer name is replaced");  // (by %USERNAME% if it holds it)
    std::string wsrBytes = z.Read("replays/wsr-20260102-120500-p200-m1.wsr");
    wsr::Reader rd;
    std::string err;
    const bool opened = rd.OpenMemory(std::vector<uint8_t>(wsrBytes.begin(), wsrBytes.end()), &err);
    Expect(opened && rd.Complete(), "the exported recording is a complete .wsr");
    Expect(opened && !Contains(rd.Header(), "76561198000000001") && !Contains(rd.Header(), "10.1.2.3"),
           "the recording's HEAD ids and IPs are hashed");
    if (fx.user.size() >= 3) Expect(opened && !ContainsCI(rd.Header(), fx.user), "the recording's user path is redacted");
    Expect(!FirstHash(events).empty() && FirstHash(events) == FirstHash(rd.Header()),
           "one salt per export: the same SteamID hashes the same in the log and the recording");

    // manifest.json
    json::Value m;
    json::Error je;
    Expect(json::Parse(z.Read("manifest.json"), &m, &je) && m.IsObject(), "manifest.json parses");
    const json::Value* scope = m.Get("scope");
    const json::Value* producer = m.Get("producer");
    Expect(scope && scope->string == "lastGame" && producer && producer->string == "launcher", "manifest scope and producer");
    const json::Value* game = m.Get("game");
    const json::Value* sid = game ? game->Get("sessionId") : nullptr;
    const json::Value* pid = game ? game->Get("pid") : nullptr;
    const json::Value* live = game ? game->Get("live") : nullptr;
    Expect(sid && sid->string == "2026-01-02_12-00-00_pid200" && pid && pid->number == 200 && live && !live->boolean,
           "manifest records the chosen session and pid");
    Expect(game && game->Get("startUtc") && game->Get("endUtc"), "manifest records the game's window");
    const json::Value* entries = m.Get("entries");
    bool allIn = entries && entries->IsArray() && entries->items.size() == res.entries;
    if (entries)
        for (const auto& e : entries->items) {
            const json::Value* p = e.Get("path");
            allIn = allIn && p && z.Has(p->string);
            const json::Value* src = e.Get("source");
            if (src && fx.user.size() >= 3) allIn = allIn && !ContainsCI(src->string, fx.user);
        }
    Expect(allIn, "every manifest entry is in the zip, with a redacted source path");
    Expect(z.names.size() == res.entries + 2, "nothing in the zip is missing from the manifest");
    const json::Value* ids = m.Get("sessionIds");
    Expect(ids && ids->items.size() == 1 && ids->items[0].string == "2026-01-02_12-00-00_pid200", "sessionIds is the one game");
}

// In the game: the current process's session even when a newer folder exists, and the live window.
void TestLastGameInGame(const Fixture& fx) {
    ex::Request rq = LauncherRequest(fx);
    rq.producer = "game";
    rq.src.currentSessionDir = fx.logs + L"\\2026-01-01_10-00-00_pid100";
    rq.src.currentPid = 100;
    bool called = false;
    rq.prov.beforeCollect = [&] { called = true; };
    rq.prov.modulesJson = [] { return std::string("[{\"name\":\"LogExport\"}]"); };
    rq.prov.gpuCompatJson = [] { return std::string("{\"gpu\":1}"); };
    rq.prov.gpuCompatText = [] { return std::string("gpu"); };
    const std::wstring out = fx.root + L"\\out\\ingame.zip";
    ex::Result res;
    Expect(ex::Export(out, rq, &res), "in-game last-game export succeeds");
    Expect(called, "beforeCollect runs (jlog::Flush in the game)");
    Expect(res.sessionId == "2026-01-01_10-00-00_pid100" && res.pid == 100, "the current process's session is chosen");
    Zip z(out);
    Expect(z.Has("logs/sessions/2026-01-01_10-00-00_pid100/events.jsonl") && z.Count("logs/sessions/") == 1,
           "only the current session");
    Expect(z.Has("replays/wsr-20260101-100500-p100-m1.wsr") && z.Has("replays/desync-20260101-100600-p100-m1-t10.zip") &&
               z.Count("replays/") == 2,
           "only the current pid's replays");
    Expect(z.Has("mods/modules.json") && z.Has("gpu/compat.json") && z.Has("gpu/compat.txt"), "in-game providers are used");
    json::Value m;
    json::Error je;
    json::Parse(z.Read("manifest.json"), &m, &je);
    const json::Value* game = m.Get("game");
    const json::Value* live = game ? game->Get("live") : nullptr;
    Expect(live && live->boolean, "a running game's session is marked live");
}

// "Save logs as...": the newest N sessions across roots and the newest replay of each kind, whatever its pid.
void TestRecentSessions(const Fixture& fx) {
    ex::Request rq = LauncherRequest(fx);
    rq.scope = ex::Scope::RecentSessions;
    rq.opt.sessions = 2;
    const std::wstring out = fx.root + L"\\out\\recent.zip";
    ex::Result res;
    Expect(ex::Export(out, rq, &res), "recent-sessions export succeeds");
    Zip z(out);
    Expect(z.HasPrefix("logs/sessions/2026-01-02_12-00-00_pid200/") && z.HasPrefix("logs/sessions/2026-01-01_11-00-00_pid150/") &&
               !z.HasPrefix("logs/sessions/2026-01-01_10-00-00_pid100/"),
           "the newest two sessions");
    Expect(z.Has("replays/desync-20260101-100600-p100-m1-t10.zip") && z.Count("replays/") == 2,
           "the newest bundle by time and the newest recording");
    Expect(z.Count("dumps/") == 2, "the newest dumps, full dumps still excluded");
    json::Value m;
    json::Error je;
    json::Parse(z.Read("manifest.json"), &m, &je);
    const json::Value* scope = m.Get("scope");
    Expect(scope && scope->string == "recentSessions" && !m.Get("game"), "manifest scope recentSessions, no game block");
}

// Nothing to find: still a zip, with the gaps listed.
void TestEmpty(const Fixture& fx) {
    ex::Request rq;
    rq.producer = "launcher";
    rq.src.sessionRoots = {fx.root + L"\\nothing"};
    const std::wstring out = fx.root + L"\\out\\empty.zip";
    ex::Result res;
    Expect(ex::Export(out, rq, &res), "an export with nothing to collect still succeeds");
    Expect(res.sessionId.empty() && res.pid == 0, "no session, no pid");
    Zip z(out);
    json::Value m;
    json::Error je;
    json::Parse(z.Read("manifest.json"), &m, &je);
    const json::Value* absent = m.Get("absent");
    bool listed = false;
    if (absent)
        for (const auto& a : absent->items) listed = listed || a.string.find("logs/sessions") == 0;
    Expect(listed, "the missing session is listed under absent");
    const json::Value* game = m.Get("game");
    Expect(game && game->Get("sessionId") && game->Get("sessionId")->IsNull(), "manifest game.sessionId is null");
}
}  // namespace

int main() {
    const std::wstring tmp = TempDir();
    TestParsers();
    const Fixture fx = Build(tmp);
    TestSessions(fx);
    TestLastGameFromLauncher(fx);
    TestLastGameInGame(fx);
    TestRecentSessions(fx);
    TestEmpty(fx);
    RemoveDirRecursive(tmp);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
