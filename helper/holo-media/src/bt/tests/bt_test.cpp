// holo_bt_test: the torrent engine offline. The pure rules (tracker addresses, stream URLs, piece math, folder
// names) and the cache lifecycle (instance lock, sweep, link-safe delete) always; with HOLO_HAVE_TORRENT the
// libtorrent engine end to end against a seeder session on 127.0.0.1: metadata, file flags, web seeds and local
// trackers dropped, select, a byte-exact stream read, seek, a blocked read that Cancel releases, remove with folder
// deletion, and Shutdown within 5 s. With HOLO_HAVE_MPV the holobt:// stream callbacks too.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "bt/bt.h"
#include "bt/bt_cache.h"
#include "bt/bt_logic.h"

#if HOLO_HAVE_TORRENT
#include <libtorrent/alert_types.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_info.hpp>

#include "bt/bt_libtorrent.h"
#endif
#if HOLO_HAVE_MPV
#include "bt/bt_mpv.h"
#endif

namespace {
namespace logic = holo::bt::logic;
int g_fail = 0, g_pass = 0;
void Check(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}
#define CHECK(c, w) Check((c), (w))

bool V4(const char* s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    const uint8_t x[4] = {static_cast<uint8_t>(a), static_cast<uint8_t>(b), static_cast<uint8_t>(c), static_cast<uint8_t>(d)};
    return logic::PublicV4(x);
}
bool V6(std::initializer_list<int> bytes) {
    uint8_t x[16] = {};
    int i = 0;
    for (int b : bytes) x[i++] = static_cast<uint8_t>(b);
    return logic::PublicV6(x);
}

void TestAddresses() {
    for (const char* s : {"0.1.2.3", "10.0.0.1", "127.0.0.1", "127.255.0.9", "169.254.1.1", "172.16.0.1", "172.31.255.255",
                          "192.168.1.1", "100.64.0.1", "100.127.255.255", "192.0.0.8", "198.18.0.1", "224.0.0.1",
                          "239.255.255.250", "255.255.255.255", "240.0.0.1"})
        CHECK(!V4(s), std::string("v4 refused ") + s);
    for (const char* s : {"8.8.8.8", "1.1.1.1", "172.15.0.1", "172.32.0.1", "100.63.0.1", "100.128.0.1", "192.169.0.1",
                          "169.253.0.1", "11.0.0.1", "198.20.0.1", "223.255.255.255"})
        CHECK(V4(s), std::string("v4 allowed ") + s);
    CHECK(!V6({}), "v6 ::");
    CHECK(!V6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), "v6 ::1");
    CHECK(!V6({0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), "v6 fe80::1");
    CHECK(!V6({0xfe, 0xbf, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), "v6 febf::1");
    CHECK(!V6({0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), "v6 fc00::1");
    CHECK(!V6({0xfd, 0x12, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), "v6 fd12::1");
    CHECK(!V6({0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), "v6 ff02::1");
    CHECK(!V6({0xfe, 0xc0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}), "v6 fec0::1 site-local");
    CHECK(!V6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 0, 1}), "v6 ::ffff:192.168.0.1");
    CHECK(!V6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1}), "v6 ::ffff:127.0.0.1");
    CHECK(V6({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 8, 8, 8, 8}), "v6 ::ffff:8.8.8.8");
    CHECK(!V6({0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0, 10, 0, 0, 1}), "v6 64:ff9b::10.0.0.1");
    CHECK(V6({0x20, 0x01, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88}), "v6 2001:4860::8888");
}

void TestTrackerHost() {
    std::string h;
    int p = -1;
    CHECK(logic::TrackerHost("udp://tracker.example.org:6969/announce", &h, &p) && h == "tracker.example.org" && p == 6969, "udp host");
    CHECK(logic::TrackerHost("https://Tracker.Example.org/announce?x=1", &h, &p) && h == "tracker.example.org" && p == 0, "https host lower");
    CHECK(logic::TrackerHost("http://[2001:db8::1]:80/a", &h, &p) && h == "2001:db8::1" && p == 80, "v6 literal");
    CHECK(logic::TrackerHost("http://1.2.3.4", &h, &p) && h == "1.2.3.4", "v4 literal, no path");
    CHECK(!logic::TrackerHost("http://user@tracker.example.org/", &h, &p), "user info refused");
    CHECK(!logic::TrackerHost("http://tracker.example.org:0/", &h, &p), "port 0");
    CHECK(!logic::TrackerHost("http://tracker.example.org:65536/", &h, &p), "port too big");
    CHECK(!logic::TrackerHost("http://tracker.example.org:/", &h, &p), "empty port");
    CHECK(!logic::TrackerHost("http://[nothex]/", &h, &p), "bad v6 literal");
    CHECK(!logic::TrackerHost("http://[1.2.3.4]/", &h, &p), "bracketed v4");
    CHECK(!logic::TrackerHost("http://exa%6dple.org/", &h, &p), "percent escape in host");
    CHECK(!logic::TrackerHost("http://xn--bcher-kva.example/", &h, &p) || h == "xn--bcher-kva.example", "punycode stays literal");
    CHECK(!logic::TrackerHost("ftp://tracker.example.org/", &h, &p), "ftp");
    CHECK(!logic::TrackerHost("http:///announce", &h, &p), "empty host");
    for (const char* n : {"localhost", "a.localhost", "nas.local", "router.lan", "box.home.arpa", "svc.internal", "router"})
        CHECK(logic::LocalHostName(n), std::string("local name ") + n);
    for (const char* n : {"tracker.example.org", "open.tracker.cl"}) CHECK(!logic::LocalHostName(n), std::string("public name ") + n);
}

void TestStreamUrl() {
    const std::string ih(40, 'a'), ih2(64, '0');
    std::string got;
    int idx = -1;
    CHECK(logic::ParseStreamUrl(holo::bt::StreamUrl(ih, 0), &got, &idx) && got == ih && idx == 0, "stream url round trip");
    CHECK(logic::ParseStreamUrl(holo::bt::StreamUrl(ih2, 999), &got, &idx) && got == ih2 && idx == 999, "v2 index 999");
    CHECK(!logic::ParseStreamUrl("holobt://" + ih + "/1000", &got, &idx), "index cap");
    CHECK(!logic::ParseStreamUrl("holobt://" + ih + "/01", &got, &idx), "leading zero");
    CHECK(!logic::ParseStreamUrl("holobt://" + ih + "/", &got, &idx), "no index");
    CHECK(!logic::ParseStreamUrl("holobt://" + ih + "/1/x", &got, &idx), "extra path");
    CHECK(!logic::ParseStreamUrl("holobt://" + std::string(40, 'A') + "/1", &got, &idx), "uppercase infohash");
    CHECK(!logic::ParseStreamUrl("holobt://" + std::string(39, 'a') + "/1", &got, &idx), "short infohash");
    CHECK(!logic::ParseStreamUrl("file:///c:/x", &got, &idx), "other scheme");
    CHECK(!logic::ParseStreamUrl("holobt://" + ih + "/-1", &got, &idx), "negative");
}

void TestFiles() {
    CHECK(logic::CleanPath("Show\\S01\\e1.mkv") == "Show/S01/e1.mkv", "backslashes to slashes");
    CHECK(logic::CleanPath(std::string("a\x01" "b.mp4")) == "ab.mp4", "control chars removed");
    CHECK(logic::CleanPath(std::string(2000, 'x')).size() == 1024, "path cap");
    CHECK(logic::PlayableFile("a/b.MKV", false, false), "mkv playable");
    CHECK(!logic::PlayableFile("a/b.mkv", true, false), "symlink never");
    CHECK(!logic::PlayableFile(".pad/123", false, true), "pad never");
    CHECK(!logic::PlayableFile("a/list.m3u8", false, false), "playlist refused");
    CHECK(!logic::PlayableFile("a/readme.txt", false, false), "txt refused");
    CHECK(!logic::PlayableFile("a/b.exe", false, false), "exe not video");
}

void TestSpan() {
    logic::Span s;
    s.offset = 1000;
    s.size = 5000;
    s.pieceLen = 1024;
    s.numPieces = 10;
    CHECK(s.Valid(), "span valid");
    CHECK(s.First() == 0 && s.Last() == 5, "first/last piece");
    CHECK(s.PieceAt(23) == 0 && s.PieceAt(24) == 1, "piece boundary");
    CHECK(s.ToPieceEnd(0) == 24 && s.ToPieceEnd(24) == 1024, "to piece end");
    CHECK(s.ToPieceEnd(4990) == 10 && s.ToPieceEnd(5000) == 0 && s.ToPieceEnd(-1) == 0, "end of file");
    CHECK(s.PieceAt(-5) == 0 && s.PieceAt(99999) == 5, "clamped");
    logic::Span z;
    z.offset = 0;
    z.size = 0;
    z.pieceLen = 16384;
    z.numPieces = 1;
    CHECK(z.ToPieceEnd(0) == 0 && z.First() == 0 && z.Last() == 0, "empty file");
    const int64_t pl = 1 << 20;
    CHECK(logic::WindowPieces(pl, 0) == 16, "16 MB floor at 1 MB pieces");
    CHECK(logic::WindowPieces(pl, 4e6) == 96, "window cap in pieces");
    CHECK(logic::WindowPieces(16 << 20, 1e6) == 2, "two piece floor");
    CHECK(logic::WindowPieces(0, 1e6) == 1, "bad piece length");
    const double r = logic::NominalRate(4000000000ll);
    CHECK(r > 700000 && r < 800000, "nominal rate of a 4 GB file");
    CHECK(logic::WindowPieces(pl, r) == 22, "22 MB window for a 4 GB film");
    CHECK(logic::DeadlineMs(0, pl, r) == 0, "first piece now");
    CHECK(logic::DeadlineMs(1, 4096, 1e9) == 50, "deadline floor");
    CHECK(logic::DeadlineMs(1, pl, 1.0) == 4000, "deadline step cap");
    CHECK(logic::DeadlineMs(1000000, pl, 1.0) == 600000, "deadline cap");
}

void TestNames() {
    CHECK(logic::InstanceFolderName(L"1234-abcdef0123"), "instance name");
    CHECK(!logic::InstanceFolderName(L"1234"), "no dash");
    CHECK(!logic::InstanceFolderName(L"-abc"), "no pid");
    CHECK(!logic::InstanceFolderName(L"12a4-abc"), "pid digits");
    CHECK(!logic::InstanceFolderName(L"1234-"), "no rand");
    CHECK(!logic::InstanceFolderName(L"1234-ab.c"), "dot in rand");
    CHECK(!logic::InstanceFolderName(L"1234-" + std::wstring(33, L'a')), "rand too long");
    CHECK(!logic::InstanceFolderName(L"Documents"), "a user folder");
    CHECK(logic::TorrentFolderName(std::wstring(40, L'f')), "torrent folder v1");
    CHECK(logic::TorrentFolderName(std::wstring(64, L'0')), "torrent folder v2");
    CHECK(!logic::TorrentFolderName(std::wstring(40, L'F')), "uppercase");
    CHECK(!logic::TorrentFolderName(L"notes"), "other names");
}

// ---------------------------------------------------------------- cache lifecycle
std::wstring TempRoot() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring r = std::wstring(tmp) + L"holo_bt_test_" + std::to_wstring(GetCurrentProcessId());
    holo::bt::cache::DeleteTree(r);
    CreateDirectoryW(r.c_str(), nullptr);
    return r;
}
void WriteFile(const std::wstring& p, const std::string& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void Mkdir(const std::wstring& p) { CreateDirectoryW(p.c_str(), nullptr); }
void AgeFolder(const std::wstring& p, int minutes) {
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart -= static_cast<ULONGLONG>(minutes) * 60 * 10000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, &ft, nullptr, nullptr);
    CloseHandle(h);
}

void TestCache(const std::wstring& root) {
    namespace cache = holo::bt::cache;
    // A junction inside a tree being deleted: the link goes, its target stays.
    const std::wstring outside = root + L"\\outside", victim = root + L"\\victim";
    Mkdir(outside);
    WriteFile(outside + L"\\keep.txt", "keep");
    Mkdir(victim);
    Mkdir(victim + L"\\sub");
    WriteFile(victim + L"\\sub\\a.bin", "a");
    WriteFile(victim + L"\\ro.bin", "r");
    SetFileAttributesW((victim + L"\\ro.bin").c_str(), FILE_ATTRIBUTE_READONLY);
    std::wstring cmd = L"cmd /c mklink /J \"" + victim + L"\\link\" \"" + outside + L"\" >nul";
    _wsystem(cmd.c_str());
    const bool linked = Exists(victim + L"\\link\\keep.txt");
    CHECK(cache::DeleteTree(victim), "delete tree");
    CHECK(!Exists(victim), "tree gone");
    CHECK(Exists(outside + L"\\keep.txt"), linked ? "junction target untouched" : "target untouched (no junction made)");
    CHECK(cache::DeleteTree(root + L"\\missing"), "deleting nothing is fine");

    // Instance sweep.
    const std::wstring mc = root + L"\\media-cache";
    Mkdir(mc);
    const std::wstring self = mc + L"\\100-aaaa", live = mc + L"\\200-bbbb", dead = mc + L"\\300-cccc",
                       young = mc + L"\\400-dddd", old = mc + L"\\500-eeee", other = mc + L"\\keepme";
    for (const auto& d : {live, dead, young, old, other}) {
        Mkdir(d);
        WriteFile(d + L"\\data.bin", "x");
    }
    std::string why;
    CHECK(cache::HoldInstanceLock(self, &why), "hold lock: " + why);
    CHECK(Exists(self + L"\\lock"), "lock file");
    HANDLE liveLock = CreateFileW((live + L"\\lock").c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, 0, nullptr);
    WriteFile(dead + L"\\lock", "300\n");
    AgeFolder(old, 30);
    AgeFolder(other, 30);
    const int swept = cache::SweepInstances(self);
    CHECK(swept == 2, "sweep count " + std::to_string(swept));
    CHECK(!Exists(dead), "dead owner swept");
    CHECK(!Exists(old), "old folder without lock swept");
    CHECK(Exists(live + L"\\data.bin"), "live owner kept");
    CHECK(Exists(young + L"\\data.bin"), "young folder without lock kept");
    CHECK(Exists(other + L"\\data.bin"), "foreign name kept");
    CHECK(Exists(self + L"\\lock"), "self kept");
    CloseHandle(liveLock);
    // The held lock refuses another instance's delete attempt.
    HANDLE probe = CreateFileW((self + L"\\lock").c_str(), DELETE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(probe == INVALID_HANDLE_VALUE, "own lock held");
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);

    // Torrent leftovers of this instance.
    Mkdir(self + L"\\bt");
    const std::wstring ihDir = self + L"\\bt\\" + std::wstring(40, L'a');
    Mkdir(ihDir);
    WriteFile(ihDir + L"\\v.mkv", "v");
    Mkdir(self + L"\\bt\\notes");
    CHECK(cache::SweepTorrents(self) == 1, "torrent sweep count");
    CHECK(!Exists(ihDir) && Exists(self + L"\\bt\\notes"), "only infohash folders swept");
    CHECK(cache::TorrentDir(self + L"\\", "ab") == self + L"\\bt\\ab", "torrent dir");
}

#if HOLO_HAVE_TORRENT
// ---------------------------------------------------------------- the libtorrent engine
using holo::bt::Engine;
using holo::bt::Event;
using Clock = std::chrono::steady_clock;

std::string RandomBytes(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::string s(n, '\0');
    for (char& c : s) c = static_cast<char>(rng() & 0xFF);
    return s;
}

struct Seeder {
    std::unique_ptr<lt::session> ses;
    int port = 0;
    bool Start() {
        lt::settings_pack p;
        p.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        p.set_bool(lt::settings_pack::enable_dht, false);
        p.set_bool(lt::settings_pack::enable_lsd, false);
        p.set_bool(lt::settings_pack::enable_upnp, false);
        p.set_bool(lt::settings_pack::enable_natpmp, false);
        p.set_int(lt::settings_pack::alert_mask, lt::alert_category::error);
        ses = std::make_unique<lt::session>(lt::session_params(p));
        for (int i = 0; i < 50 && port == 0; ++i) {
            port = ses->listen_port();
            if (!port) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return port != 0;
    }
    // Makes a torrent of <root>\<name>\ (v1 + v2 hybrid, so it has pad files) and seeds it. Returns the v1 infohash.
    std::string Seed(const std::wstring& root, const std::string& name, int pieceLen, const char* webSeed) {
        std::string rootU8(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, root.c_str(), -1, nullptr, 0, nullptr, nullptr)), ' ');
        WideCharToMultiByte(CP_UTF8, 0, root.c_str(), -1, rootU8.data(), static_cast<int>(rootU8.size()), nullptr, nullptr);
        rootU8.pop_back();  // the terminator
        lt::create_torrent ct(lt::list_files(rootU8 + "\\" + name), pieceLen);
        if (webSeed) ct.add_url_seed(webSeed);
        lt::set_piece_hashes(ct, rootU8);
        const std::vector<char> buf = ct.generate_buf();
        lt::add_torrent_params atp = lt::load_torrent_buffer(buf);
        const lt::info_hash_t hashes = atp.ti->info_hashes();
        atp.save_path = rootU8;
        atp.flags |= lt::torrent_flags::seed_mode;
        ses->add_torrent(std::move(atp));
        std::string hex;
        for (const char c : hashes.v1) {
            static const char kHex[] = "0123456789abcdef";
            hex.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 15]);
            hex.push_back(kHex[static_cast<unsigned char>(c) & 15]);
        }
        return hex;
    }
};

// Polls until `pred` sees the event it wants (or the time runs out); every event lands in *all.
template <class F>
bool PollFor(Engine* e, int ms, std::vector<Event>* all, F pred) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < end) {
        std::vector<Event> evs;
        e->Poll(&evs);
        bool hit = false;
        for (Event& ev : evs) {
            hit = hit || pred(ev);
            all->push_back(std::move(ev));
        }
        if (hit) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void TestEngine(const std::wstring& root) {
    namespace t = holo::bt::testing;
    namespace url = melange::media::url;
    t::LoopbackOnly(true);

    // Tracker vetting without DNS (literals and local names only).
    CHECK(!t::TrackerAllowed("udp://127.0.0.1:6969/announce"), "loopback tracker refused");
    CHECK(!t::TrackerAllowed("http://[::1]:80/announce"), "v6 loopback tracker refused");
    CHECK(!t::TrackerAllowed("http://192.168.1.1/announce"), "LAN tracker refused");
    CHECK(!t::TrackerAllowed("http://169.254.169.254/latest"), "metadata service refused");
    CHECK(!t::TrackerAllowed("http://localhost:8080/announce"), "localhost refused");
    CHECK(!t::TrackerAllowed("http://router/announce"), "single label refused");
    CHECK(!t::TrackerAllowed("http://0x7f.0.0.1/announce"), "odd v4 spelling refused");
    CHECK(!t::TrackerAllowed("http://2130706433/announce"), "decimal v4 refused");
    CHECK(t::TrackerAllowed("udp://8.8.8.8:6969/announce"), "public literal allowed");

    const std::wstring seedRoot = root + L"\\seed", cacheDir = root + L"\\media-cache\\100-aaaa";
    Mkdir(seedRoot);
    Mkdir(seedRoot + L"\\Show");
    const std::string video = RandomBytes(6 * 1024 * 1024 + 12345, 7);
    WriteFile(seedRoot + L"\\Show\\video.mp4", video);
    WriteFile(seedRoot + L"\\Show\\notes.txt", "hello");
    WriteFile(seedRoot + L"\\Show\\list.m3u", "http://192.168.1.1/x.mp4\n");
    Mkdir(seedRoot + L"\\Slow");
    const std::string slow = RandomBytes(4 * 1024 * 1024, 9);
    WriteFile(seedRoot + L"\\Slow\\slow.mkv", slow);

    Seeder seeder;
    CHECK(seeder.Start(), "seeder listens on 127.0.0.1");
    const std::string ih = seeder.Seed(seedRoot, "Show", 256 * 1024, "http://127.0.0.1:9/webseed/");
    CHECK(url::ValidInfohash(ih), "seed infohash");

    holo::bt::Settings s;
    s.cacheDir = cacheDir;
    std::string why;
    auto e = holo::bt::CreateEngine(s, &why);
    CHECK(e != nullptr, "engine: " + why);
    if (!e) return;
    CHECK(e->Version().rfind("libtorrent ", 0) == 0, "version " + e->Version());

    url::Classified cl;
    const std::string raw = "magnet:?xt=urn:btih:" + ih + "&dn=Show&tr=http%3A%2F%2F127.0.0.1%3A1%2Fannounce" +
                            "&tr=udp%3A%2F%2F10.0.0.1%3A80&x.pe=192.168.1.5:6881&ws=http%3A%2F%2F192.168.1.1%2F";
    CHECK(url::Classify(raw, &cl) && cl.kind == url::Kind::Magnet, "classify seed magnet");
    CHECK(!e->Add(ih, raw, s, &why), "raw (non-canonical) magnet refused");
    CHECK(!e->Add(std::string(40, 'b'), cl.url, s, &why), "infohash mismatch refused");
    CHECK(e->Add(ih, cl.url, s, &why), "add: " + why);
    CHECK(e->Add(ih, cl.url, s, &why), "second add is a no-op");
    CHECK(e->Select(ih, 0, &why), "select before metadata is deferred");
    CHECK(t::ConnectPeer(e.get(), ih, "127.0.0.1", seeder.port), "connect to seeder");

    std::vector<Event> evs;
    const Event* files = nullptr;
    CHECK(PollFor(e.get(), 30000, &evs, [](const Event& ev) { return ev.kind == Event::Kind::Files; }), "metadata arrives");
    for (const Event& ev : evs)
        if (ev.kind == Event::Kind::Files) files = &ev;
    int vIdx = -1, tIdx = -1, mIdx = -1;
    if (files) {
        CHECK(files->ih == ih && files->name == "Show", "files event names the torrent");
        CHECK(files->files.size() == 3, "three real files (pads hidden): " + std::to_string(files->files.size()));
        for (const auto& f : files->files) {
            if (f.path == "Show/video.mp4") vIdx = f.index, CHECK(f.video && f.size == video.size(), "mp4 is video");
            if (f.path == "Show/notes.txt") tIdx = f.index, CHECK(!f.video, "txt is not video");
            if (f.path == "Show/list.m3u") mIdx = f.index, CHECK(!f.video, "m3u refused");
        }
    }
    CHECK(vIdx >= 0 && tIdx >= 0 && mIdx >= 0, "all three paths listed");
    CHECK(t::WebSeedCount(e.get(), ih) == 0, "url seed from the metadata removed");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    e->Poll(&evs);
    CHECK(t::TrackerCount(e.get(), ih) == 0, "loopback and private trackers never added");
    // The deferred select of file 0 was checked against the metadata (file 0 is not the mp4 in sorted order, or is).
    CHECK(!e->Select(ih, tIdx, &why), "txt cannot be selected");
    CHECK(!e->Select(ih, mIdx, &why), "m3u cannot be selected");
    CHECK(!e->Select(ih, 999, &why), "no such file");
    CHECK(e->Select(ih, vIdx, &why), "select video: " + why);
    CHECK(e->Open(ih, tIdx, &why) == nullptr, "no stream over a txt");
    CHECK(e->Open(std::string(40, 'c'), 0, &why) == nullptr, "no stream over an unknown torrent");

    auto st = e->Open(ih, vIdx, &why);
    CHECK(st != nullptr, "open stream: " + why);
    if (st) {
        CHECK(st->Size() == static_cast<int64_t>(video.size()), "stream size");
        std::string got;
        std::vector<char> buf(100000);
        const auto t0 = Clock::now();
        std::thread pump([&] {  // the main loop keeps polling while mpv's thread reads
            while (Clock::now() - t0 < std::chrono::seconds(60) && got.size() < video.size()) {
                std::vector<Event> x;
                e->Poll(&x);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        while (got.size() < video.size()) {
            const int64_t n = st->Read(buf.data(), buf.size());
            if (n <= 0) break;
            got.append(buf.data(), static_cast<size_t>(n));
        }
        pump.join();
        CHECK(got == video, "stream bytes equal the seeded file (" + std::to_string(got.size()) + ")");
        CHECK(st->Read(buf.data(), buf.size()) == 0, "read at the end is 0");
        CHECK(st->Seek(static_cast<int64_t>(video.size()) - 1000), "seek near the end");
        CHECK(st->Read(buf.data(), 5000) == 1000 && memcmp(buf.data(), video.data() + video.size() - 1000, 1000) == 0, "tail read");
        CHECK(st->Seek(300000) && st->Read(buf.data(), 10) == 10 && memcmp(buf.data(), video.data() + 300000, 10) == 0, "seek back");
        CHECK(!st->Seek(-1) && !st->Seek(static_cast<int64_t>(video.size()) + 1), "seek out of range");
    }
    bool status = false;
    PollFor(e.get(), 2500, &evs, [&](const Event& ev) {
        if (ev.kind == Event::Kind::Status && ev.ih == ih && ev.status.metadata && ev.status.progress > 0.99) status = true;
        return status;
    });
    CHECK(status, "status reports metadata and the selected file complete");

#if HOLO_HAVE_MPV
    {
        mpv_stream_cb_info info{};
        char bad[] = "holobt://nothex/0";
        CHECK(holo::bt::MpvStreamOpen(e.get(), bad, &info) == MPV_ERROR_LOADING_FAILED, "mpv open of a bad uri fails");
        std::string u = holo::bt::StreamUrl(ih, tIdx);
        CHECK(holo::bt::MpvStreamOpen(e.get(), u.data(), &info) == MPV_ERROR_LOADING_FAILED, "mpv open of a txt fails");
        u = holo::bt::StreamUrl(ih, vIdx);
        CHECK(holo::bt::MpvStreamOpen(e.get(), u.data(), &info) == 0 && info.read_fn && info.close_fn && info.cancel_fn, "mpv open");
        if (info.cookie) {
            char b[16];
            CHECK(info.seek_fn(info.cookie, 0) == 0 && info.size_fn(info.cookie) == static_cast<int64_t>(video.size()), "mpv seek/size");
            CHECK(info.read_fn(info.cookie, b, 16) == 16 && memcmp(b, video.data(), 16) == 0, "mpv read");
            info.cancel_fn(info.cookie);
            CHECK(info.read_fn(info.cookie, b, 16) == -1, "mpv read after cancel");
            info.close_fn(info.cookie);
        }
    }
#endif

    // A slow torrent: a read of a missing piece blocks until Cancel releases it.
    lt::settings_pack slowPack;
    slowPack.set_int(lt::settings_pack::upload_rate_limit, 8 * 1024);
    seeder.ses->apply_settings(slowPack);
    const std::string ih2 = seeder.Seed(seedRoot, "Slow", 256 * 1024, nullptr);
    url::Classified cl2;
    CHECK(url::Classify("magnet:?xt=urn:btih:" + ih2, &cl2), "classify slow magnet");
    CHECK(e->Add(ih2, cl2.url, s, &why), "add slow: " + why);
    CHECK(t::ConnectPeer(e.get(), ih2, "127.0.0.1", seeder.port), "connect slow");
    CHECK(PollFor(e.get(), 30000, &evs, [&](const Event& ev) { return ev.kind == Event::Kind::Files && ev.ih == ih2; }), "slow metadata");
    auto st2 = e->Open(ih2, 0, &why);
    CHECK(st2 != nullptr, "open slow stream: " + why);
    if (st2) {
        CHECK(st2->Seek(3 * 1024 * 1024 + 7), "seek into a missing piece");
        std::atomic<int64_t> res{1};
        std::atomic<bool> done{false};
        std::thread reader([&] {
            char b[4096];
            res = st2->Read(b, sizeof b);
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        CHECK(!done, "read blocks on a missing piece");
        const auto c0 = Clock::now();
        st2->Cancel();
        reader.join();
        CHECK(res == -1, "cancelled read returns -1");
        CHECK(Clock::now() - c0 < std::chrono::seconds(1), "cancel releases the read at once");
    }

    // Remove: streams fail, the folder goes, Removed is reported.
    const std::wstring dir = holo::bt::cache::TorrentDir(cacheDir, ih);
    CHECK(Exists(dir + L"\\Show\\video.mp4"), "data under <cache>\\bt\\<ih>");
    e->Remove(ih);
    CHECK(PollFor(e.get(), 6000, &evs, [&](const Event& ev) { return ev.kind == Event::Kind::Removed && ev.ih == ih; }), "removed event");
    CHECK(!Exists(dir), "torrent folder deleted");
    if (st) {
        char b[8];
        CHECK(st->Read(b, 8) == -1, "a stream of a removed torrent fails");
    }
    e->Remove(std::string(40, 'd'));
    CHECK(PollFor(e.get(), 1000, &evs, [&](const Event& ev) { return ev.kind == Event::Kind::Removed && ev.ih == std::string(40, 'd'); }),
          "remove of an unknown torrent still answers");

    const auto s0 = Clock::now();
    e->Shutdown();
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - s0).count();
    CHECK(took < 5000, "shutdown within 5 s (" + std::to_string(took) + " ms)");
    CHECK(!Exists(holo::bt::cache::TorrentDir(cacheDir, ih2)), "shutdown deletes the remaining torrent");
    CHECK(!e->Add(ih2, cl2.url, s, &why), "no add after shutdown");
    st.reset();
    st2.reset();
    e.reset();
    seeder.ses.reset();
}
#endif
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    TestAddresses();
    TestTrackerHost();
    TestStreamUrl();
    TestFiles();
    TestSpan();
    TestNames();
    const std::wstring root = TempRoot();
    TestCache(root);
#if HOLO_HAVE_TORRENT
    TestEngine(root);
#else
    printf("holo_bt_test: built without HOLO_TORRENT, engine cases skipped\n");
#endif
    holo::bt::cache::DeleteTree(root);
    printf("holo_bt_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
