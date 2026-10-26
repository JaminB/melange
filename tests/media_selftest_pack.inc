// media_selftest cases for the pack installer (owner: C4a): manifest parse, the streaming extractor's path rules
// (traversal, extra entry, size and hash mismatch, the 4 MiB heap high-water mark), a file:/// download with Range
// resume, the yt-dlp lane manifest (hash, serial rollback). Included at file scope, so it may #include what it needs;
// it defines pack_cases::Run(), and CHECK(cond, what) is available. Sources: cmake/holo/media_pack.cmake.
//
// The heap high-water mark: this file replaces the global operator new/delete of media_selftest.exe with counting
// versions (malloc + _msize), and the extractor counts its own and miniz's buffers (pack::core::Heap()). The install
// case holds both peaks to 4 MiB while it streams a 12 MB entry (Critique M1).
#include <malloc.h>

#include <miniz.h>

#include <cstdlib>
#include <new>

#include "media/pack_internal.h"
#include "media/pack_logic.h"
#include "media/pack_manifest.h"
#include "store/fetch.h"
#include "store/index.h"
#include "tools/hash.h"

namespace pack_cases {
std::atomic<int64_t> g_newCur{0}, g_newPeak{0};
inline void NewCount(int64_t d) {
    const int64_t now = g_newCur.fetch_add(d) + d;
    int64_t peak = g_newPeak.load();
    while (now > peak && !g_newPeak.compare_exchange_weak(peak, now)) {
    }
}
}  // namespace pack_cases

void* operator new(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    pack_cases::NewCount(static_cast<int64_t>(_msize(p)));
    return p;
}
void* operator new[](size_t n) { return operator new(n); }
void operator delete(void* p) noexcept {
    if (!p) return;
    pack_cases::NewCount(-static_cast<int64_t>(_msize(p)));
    free(p);
}
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete(void* p, size_t) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

// media_selftest_host.inc: launches the real holo-media.exe of an installed dev pack (when one is compiled in).
namespace host_cases {
void RealHelper(const std::wstring& packDir, const std::wstring& ytdlp);
}

namespace pack_cases {
namespace pl = melange::media::pack::logic;
namespace pc = melange::media::pack::core;
using melange::hashutil::Sha256Hex;

constexpr char kPrefix[] = "https://github.com/JaminB/melange/releases/download/media-";

// ---------------------------------------------------------------- fixtures
std::wstring g_tmp;

std::wstring TempRoot() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    return std::wstring(buf) + L"melange-media-pack-selftest-" + std::to_wstring(GetCurrentProcessId());
}

std::string FileUrl(const std::wstring& path) {
    std::string s = "file:///" + pc::Narrow(path);
    for (char& c : s)
        if (c == '\\') c = '/';
    std::string out;
    for (unsigned char c : s) {
        if (c == ' ' || c == '%' || c >= 0x80) {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

bool WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size();
    CloseHandle(f);
    return ok;
}

std::string ReadBytes(const std::wstring& path) {
    std::string s;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char b[65536];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Noise(size_t n, uint32_t seed) {  // incompressible
    std::string s(n, '\0');
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        s[i] = static_cast<char>(x);
    }
    return s;
}

std::string Pattern(size_t n) {  // compressible: exercises the deflate path in many callback chunks
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>("melange holo media pack "[i % 24] ^ ((i >> 12) & 7));
    return s;
}

struct Entry {
    std::string name, data;
    int level = 6;  // 0 = stored
};

// A pack: the files, pack.json describing them, and a manifest for the zip.
struct Fixture {
    std::vector<Entry> files;  // without pack.json
    std::string version = "7";
    std::string PackJson(const std::string& ver) const {
        std::string j = "{\"format\": \"melange-media-pack-1\", \"version\": \"" + ver + "\", \"files\": [";
        for (size_t i = 0; i < files.size(); ++i)
            j += (i ? "," : "") + std::string("{\"path\": \"") + files[i].name + "\", \"size\": " + std::to_string(files[i].data.size()) +
                 ", \"sha256\": \"" + Sha256Hex(files[i].data.data(), files[i].data.size()) + "\"}";
        return j + "]}\n";
    }
};

bool WriteZip(const std::wstring& path, const std::vector<Entry>& entries) {
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, pc::Narrow(path).c_str(), 0)) return false;
    bool ok = true;
    for (const Entry& e : entries) {
        if (!e.name.empty() && e.name.back() == '/')
            ok = mz_zip_writer_add_mem(&z, e.name.c_str(), nullptr, 0, 0) && ok;
        else
            ok = mz_zip_writer_add_mem(&z, e.name.c_str(), e.data.data(), e.data.size(), static_cast<mz_uint>(e.level)) && ok;
    }
    ok = mz_zip_writer_finalize_archive(&z) && ok;
    mz_zip_writer_end(&z);
    return ok;
}

pl::Manifest ManifestFor(const Fixture& fx, const std::string& packJson, const std::wstring& zip) {
    pl::Manifest m;
    m.version = fx.version;
    m.dev = true;
    m.zipUrl = FileUrl(zip);
    const std::string z = ReadBytes(zip);
    m.zipSize = z.size();
    m.zipSha256 = Sha256Hex(z.data(), z.size());
    for (const Entry& e : fx.files) m.files.push_back({e.name, e.data.size(), Sha256Hex(e.data.data(), e.data.size())});
    m.files.push_back({"pack.json", packJson.size(), Sha256Hex(packJson.data(), packJson.size())});
    return m;
}

Fixture StandardFixture() {
    Fixture fx;
    fx.files = {{"holo-media.exe", Noise(3u << 20, 1), 6},
                {"libmpv-2.dll", Pattern(12u << 20), 6},
                {"deno.exe", Noise(5u << 20, 2), 0},
                {"LICENSES/mpv.txt", "LGPL 2.1+\n", 6},
                {"SOURCES.md", "# Sources\n", 6}};
    return fx;
}

std::vector<Entry> ZipEntries(const Fixture& fx, const std::string& packJson) {
    std::vector<Entry> v = {{"LICENSES/", "", 0}};
    for (const Entry& e : fx.files) v.push_back(e);
    v.push_back({"pack.json", packJson, 6});
    return v;
}

// ---------------------------------------------------------------- pure rules
void Rules() {
    CHECK(pl::SafePackPath("holo-media.exe") && pl::SafePackPath("LICENSES/mpv.txt") && pl::SafePackPath("a/b/c.txt"), "safe paths");
    for (const char* bad : {"", "../evil.dll", "a/../b", "/abs.exe", "C:/x.exe", "a\\b", "a//b", "a/", ".hidden", "dot.", "con.txt",
                            "LPT1", "a/b/c/d", "sp ace.txt", "x:y", "nul"})
        CHECK(!pl::SafePackPath(bad), std::string("unsafe path refused: ") + bad);
    CHECK(pl::ValidVersion("1") && pl::ValidVersion("dev-1729") && pl::ValidVersion("1.2_rc"), "versions");
    for (const char* bad : {"", ".1", "1.", "a..b", "ytdlp", "DL", "cache", "a/b", "-1", "123456789012345678901234567890123"})
        CHECK(!pl::ValidVersion(bad), std::string("bad version refused: ") + bad);
    CHECK(pl::ValidLaneVersion("2026.10.21") && pl::ValidLaneVersion("2026.10.21.1") && !pl::ValidLaneVersion("2026..10") &&
              !pl::ValidLaneVersion("v2026.10.21") && !pl::ValidLaneVersion("1.2"),
          "lane versions");

    const std::string good = std::string(kPrefix) + "v1/melange-media-1.zip";
    CHECK(pl::AllowedUrl(good, kPrefix, false), "allowed prefix");
    for (const std::string& bad :
         {std::string("https://github.com/JaminB/melange/releases/download/v0.12.0/x.zip"), std::string("http://github.com/JaminB/melange/releases/download/media-v1/x.zip"),
          std::string("https://github.com/JaminB/melange/releases/download/media-"), good + "?x=1", good + "#f", std::string(kPrefix) + "v1/../../x.zip",
          std::string(kPrefix) + "v1//x.zip", std::string(kPrefix) + "v1/x y.zip", std::string("https://github.com/JaminB/melange.evil.com/x"),
          std::string("file:///C:/pack.zip"), std::string("HTTPS://github.com/JaminB/melange/releases/download/media-v1/x.zip")})
        CHECK(!pl::AllowedUrl(bad, kPrefix, false), "refused url " + bad);
    CHECK(pl::AllowedUrl("file:///C:/dev/pack.zip", kPrefix, true) && !pl::AllowedUrl("file:///C:/dev/../pack.zip", kPrefix, true),
          "file:/// only where allowed, without ..");

    pl::Manifest m;
    m.version = "1";
    m.zipUrl = good;
    m.zipSize = 100;
    m.zipSha256 = std::string(64, 'a');
    for (const char* p : {"holo-media.exe", "libmpv-2.dll", "deno.exe", "pack.json", "LICENSES/mpv.txt"}) m.files.push_back({p, 10, std::string(64, 'b')});
    std::string why, url;
    CHECK(pl::CheckManifest(m, &why), "a good manifest: " + why);
    CHECK(pl::ChoosePackUrl("", m, kPrefix, &url, &why) && url == good, "compiled url");
    CHECK(pl::ChoosePackUrl("file:///C:/t/p.zip", m, kPrefix, &url, &why) && url == "file:///C:/t/p.zip", "file:/// override");
    CHECK(!pl::ChoosePackUrl("https://example.com/p.zip", m, kPrefix, &url, &why), "foreign override refused");
    auto bad = [&](const char* what, auto mutate) {
        pl::Manifest x = m;
        mutate(x);
        CHECK(!pl::CheckManifest(x, &why), std::string("manifest refused: ") + what);
    };
    bad("duplicate", [](pl::Manifest& x) { x.files.push_back(x.files[0]); });
    bad("duplicate by case", [](pl::Manifest& x) { x.files.push_back({"HOLO-media.exe", 1, std::string(64, 'c')}); });
    bad("file is a folder", [](pl::Manifest& x) { x.files.push_back({"LICENSES", 1, std::string(64, 'c')}); });
    bad("missing deno", [](pl::Manifest& x) { x.files.erase(x.files.begin() + 2); });
    bad("bad sha", [](pl::Manifest& x) { x.files[0].sha256 = std::string(64, 'B'); });
    bad("traversal", [](pl::Manifest& x) { x.files[4].path = "../mpv.txt"; });
    bad("too big", [](pl::Manifest& x) { x.files[1].size = pl::kMaxFileBytes + 1; });
    bad("zip size 0", [](pl::Manifest& x) { x.zipSize = 0; });
    bad("reserved version", [](pl::Manifest& x) { x.version = "ytdlp"; });

    // pack.json
    pl::PackJson pj;
    const std::string pjText = "{\"format\":\"melange-media-pack-1\",\"version\":\"1\",\"extra\":1,\"files\":["
                               "{\"path\":\"holo-media.exe\",\"size\":10,\"sha256\":\"" + std::string(64, 'b') + "\"},"
                               "{\"path\":\"libmpv-2.dll\",\"size\":10,\"sha256\":\"" + std::string(64, 'b') + "\"},"
                               "{\"path\":\"deno.exe\",\"size\":10,\"sha256\":\"" + std::string(64, 'b') + "\"},"
                               "{\"path\":\"LICENSES/mpv.txt\",\"size\":10,\"sha256\":\"" + std::string(64, 'b') + "\"}]}";
    CHECK(pl::ParsePackJson(pjText, &pj, &why) && pj.files.size() == 4, "pack.json parses: " + why);
    CHECK(pl::PackJsonMatches(pj, m, &why), "pack.json matches the manifest: " + why);
    pl::PackJson pj2 = pj;
    pj2.version = "2";
    CHECK(!pl::PackJsonMatches(pj2, m, &why), "pack.json of another version refused");
    pj2 = pj;
    pj2.files[1].sha256 = std::string(64, 'c');
    CHECK(!pl::PackJsonMatches(pj2, m, &why), "pack.json with another hash refused");
    pj2 = pj;
    pj2.files.push_back({"extra.dll", 1, std::string(64, 'd')});
    CHECK(!pl::PackJsonMatches(pj2, m, &why), "pack.json with an extra file refused");
    for (const std::string& t : {std::string("[]"), std::string("{\"format\":\"x\",\"version\":\"1\",\"files\":[]}"),
                                 std::string("{\"format\":\"melange-media-pack-1\",\"version\":\"1\",\"files\":[{\"path\":\"../a\",\"size\":1,\"sha256\":\"") +
                                     std::string(64, 'b') + "\"}]}",
                                 std::string("{\"format\":\"melange-media-pack-1\",\"version\":\"1\",\"files\":[{\"path\":\"a\",\"size\":-1,\"sha256\":\"") +
                                     std::string(64, 'b') + "\"}]}",
                                 std::string("{\"format\":\"melange-media-pack-1\",\"version\":\"1\",\"files\":[{\"path\":\"pack.json\",\"size\":1,\"sha256\":\"") +
                                     std::string(64, 'b') + "\"}]}"})
        CHECK(!pl::ParsePackJson(t, &pj2, &why), "bad pack.json refused: " + t.substr(0, 60));

    // the extraction plan
    std::vector<pl::ZipEntry> z;
    for (const pl::File& f : m.files) z.push_back({f.path, f.size, false, false, 8});
    z.push_back({"LICENSES/", 0, true, false, 0});
    std::vector<int> plan;
    CHECK(pl::PlanExtract(z, m, &plan, &why) && plan.back() == -1 && plan[0] == 0, "a good plan: " + why);
    auto badPlan = [&](const char* what, auto mutate) {
        std::vector<pl::ZipEntry> x = z;
        mutate(x);
        CHECK(!pl::PlanExtract(x, m, &plan, &why), std::string("plan refused: ") + what);
    };
    badPlan("extra entry", [](auto& x) { x.push_back({"evil.dll", 10, false, false, 8}); });
    badPlan("traversal entry", [](auto& x) { x.push_back({"../evil.dll", 10, false, false, 8}); });
    badPlan("backslash entry", [](auto& x) { x.push_back({"LICENSES\\mpv.txt", 10, false, false, 8}); });
    badPlan("case variant", [](auto& x) { x[0].name = "Holo-Media.exe"; });
    badPlan("duplicate", [](auto& x) { x.push_back(x[0]); });
    badPlan("size mismatch", [](auto& x) { x[1].size = 11; });
    badPlan("missing file", [](auto& x) { x.erase(x.begin() + 2); });
    badPlan("encrypted", [](auto& x) { x[0].encrypted = true; });
    badPlan("method", [](auto& x) { x[0].method = 12; });
    badPlan("stray folder", [](auto& x) { x.push_back({"plugins/", 0, true, false, 0}); });

    // the lane manifest and its serial
    const std::string sha = std::string(64, 'e');
    auto lane = [&](const std::string& url, const char* serial) {
        return "{\"format\":\"melange-media-ytdlp-1\",\"version\":\"2026.10.21\",\"url\":\"" + url + "\",\"size\":100,\"sha256\":\"" +
               sha + "\",\"serial\":" + serial + "}";
    };
    pl::Lane l;
    const std::string laneExe = std::string(kPrefix) + "ytdlp/yt-dlp-2026.10.21.exe";
    CHECK(pl::ParseLane(lane(laneExe, "3"), kPrefix, false, &l, &why) && l.serial == 3 && l.size == 100, "lane parses: " + why);
    CHECK(!pl::ParseLane(lane("https://github.com/yt-dlp/yt-dlp/releases/download/x/yt-dlp.exe", "3"), kPrefix, false, &l, &why),
          "lane exe from another repo refused");
    CHECK(!pl::ParseLane(lane("file:///C:/yt-dlp.exe", "3"), kPrefix, false, &l, &why), "file:/// lane exe refused for an https lane");
    CHECK(pl::ParseLane(lane("file:///C:/yt-dlp.exe", "3"), kPrefix, true, &l, &why), "file:/// lane exe for a dev lane");
    CHECK(!pl::ParseLane(lane(laneExe, "0"), kPrefix, false, &l, &why), "serial 0 refused");
    CHECK(!pl::ParseLane(lane(laneExe, "-2"), kPrefix, false, &l, &why), "negative serial refused");
    CHECK(!pl::ParseLane(lane(laneExe, "2.5"), kPrefix, false, &l, &why), "fractional serial refused");
    pl::Lane have{"2026.10.01", "", std::string(64, 'a'), 90, 5};
    pl::Lane offer = have;
    CHECK(pl::DecideLane(pl::Lane{}, offer) == pl::LaneAction::Install, "nothing installed: install");
    CHECK(pl::DecideLane(have, offer) == pl::LaneAction::Current, "same: current");
    offer.serial = 4;
    CHECK(pl::DecideLane(have, offer) == pl::LaneAction::Rollback, "lower serial: rollback");
    offer = have;
    offer.sha256 = sha;
    CHECK(pl::DecideLane(have, offer) == pl::LaneAction::Conflict, "same serial, other file: conflict");
    offer.serial = 6;
    CHECK(pl::DecideLane(have, offer) == pl::LaneAction::Install, "higher serial, new file: install");
    offer = have;
    offer.serial = 6;
    CHECK(pl::DecideLane(have, offer) == pl::LaneAction::Record, "higher serial, same file: record");
    CHECK(pl::LaneCheckDue(1000000, 0, false) && !pl::LaneCheckDue(1000000, 1000000 - 3600, false) &&
              pl::LaneCheckDue(1000000, 1000000 - 86400, false) && pl::LaneCheckDue(1000000, 1000000 - 601, true) &&
              !pl::LaneCheckDue(1000000, 1000000 - 60, true) && pl::LaneCheckDue(1000000, 1000000 + 50, false),
          "lane check intervals");

    CHECK(pl::NeedFreeBytes(m) == 50 + 100 + (512ull << 20), "free space: unpacked + zip + 512 MiB");
    CHECK(pl::ThrottleSleepMs(1024 * 1024, 0, 0) == 0, "no throttle");
    CHECK(pl::ThrottleSleepMs(2048 * 1024, 0, 2048) == 1000 && pl::ThrottleSleepMs(1024 * 1024, 400, 2048) == 100 &&
              pl::ThrottleSleepMs(1024 * 1024, 600, 2048) == 0,
          "throttle sleeps to the cap");
    CHECK(pl::IsTmpFolder("1.tmp") && pl::IsTmpFolder("1.del-0a1b.tmp") && !pl::IsTmpFolder("1") && !pl::IsTmpFolder(".tmp") &&
              !pl::IsTmpFolder("ytdlp.tmp"),
          "tmp folder names");

    uint64_t a = 0, b = 0, t = 0;
    CHECK(melange::store::fetch::ParseContentRange("bytes 100-199/1000", &a, &b, &t) && a == 100 && b == 199 && t == 1000, "content-range");
    CHECK(melange::store::fetch::ParseContentRange("bytes 5-9/*", &a, &b, &t) && a == 5 && t == 0, "content-range unknown total");
    for (const char* cr : {"", "bytes", "bytes 9-5/10", "bytes 5-10/10", "items 1-2/3", "bytes -5/10", "bytes 1-2/3x", "bytes 1-2",
                           "bytes 99999999999999999999-1/2"})
        CHECK(!melange::store::fetch::ParseContentRange(cr, &a, &b, &t), std::string("bad content-range: ") + cr);
}

// ---------------------------------------------------------------- downloads
struct RecordSink : melange::store::fetch::Sink {
    uint64_t offset = UINT64_MAX, total = 0;
    std::string data;
    bool Start(uint64_t o, uint64_t t) override {
        offset = o;
        total = t;
        return true;
    }
    bool Write(const void* p, size_t n) override {
        data.append(static_cast<const char*>(p), n);
        return true;
    }
};

void Downloads() {
    const std::wstring src = g_tmp + L"\\src.bin";
    const std::string body = Noise(3u << 20, 9);
    CHECK(WriteBytes(src, body), "fixture written");
    const std::string url = FileUrl(src), sha = Sha256Hex(body.data(), body.size());
    std::string why;

    melange::store::fetch::Options o;
    o.resumeFrom = 1000;
    RecordSink rs;
    CHECK(melange::store::fetch::Get(url, rs, o, &why) && rs.offset == 1000 && rs.total == body.size() && rs.data == body.substr(1000),
          "file:/// Range: starts at the offset");
    o.resumeFrom = body.size() + 5;
    RecordSink rs2;
    CHECK(melange::store::fetch::Get(url, rs2, o, &why) && rs2.offset == 0 && rs2.data == body, "a range past the end restarts at 0");
    melange::store::fetch::StringSink plain;
    CHECK(melange::store::fetch::Get(url, plain, melange::store::fetch::Options{}, &why) && plain.data == body, "no range: whole body");

    pc::Hooks h;
    std::string err, detail;
    const std::wstring part = g_tmp + L"\\dl.part";
    uint64_t firstProgress = UINT64_MAX;
    h.progress = [&](uint64_t done, uint64_t) {
        if (firstProgress == UINT64_MAX) firstProgress = done;
    };
    CHECK(pc::FetchResumable(url, part, body.size(), sha, h, &err, &detail) && ReadBytes(part) == body, "fresh download: " + detail);
    // A dropped download: the first 1 MB kept, the rest fetched with a Range.
    CHECK(WriteBytes(part, body.substr(0, 1u << 20)), "partial written");
    firstProgress = UINT64_MAX;
    CHECK(pc::FetchResumable(url, part, body.size(), sha, h, &err, &detail) && ReadBytes(part) == body, "resumed download: " + detail);
    CHECK(firstProgress >= (1u << 20), "the resume did not start from 0");
    // A kept part with wrong bytes: the hash fails at the end, the part is deleted, the next try starts clean.
    std::string bad = body.substr(0, 1u << 20);
    bad[5] ^= 1;
    CHECK(WriteBytes(part, bad), "bad partial written");
    CHECK(!pc::FetchResumable(url, part, body.size(), sha, h, &err, &detail) && err == "hash" && !Exists(part), "corrupt part: hash, deleted");
    CHECK(pc::FetchResumable(url, part, body.size(), sha, h, &err, &detail), "clean retry");
    // Longer than expected: the part is dropped and refetched; the source itself too long fails the size check.
    CHECK(WriteBytes(part, body + "xx"), "long part");
    CHECK(pc::FetchResumable(url, part, body.size(), sha, h, &err, &detail), "an oversize part is discarded");
    DeleteFileW(part.c_str());
    CHECK(!pc::FetchResumable(url, part, body.size() - 1, sha, h, &err, &detail) && err == "network" && !Exists(part),
          "a source longer than the manifest: refused by the cap");
    CHECK(!pc::FetchResumable(url, part, body.size(), std::string(64, '0'), h, &err, &detail) && err == "hash", "wrong sha256");
    CHECK(!pc::FetchResumable(FileUrl(g_tmp + L"\\missing.bin"), part, 10, sha, h, &err, &detail) && err == "network", "missing source");
    std::atomic<bool> cancel{true};
    h.cancel = &cancel;
    CHECK(!pc::FetchResumable(url, part, body.size(), sha, h, &err, &detail) && err == "cancelled", "cancelled");
    DeleteFileW(part.c_str());
    DeleteFileW(src.c_str());
}

// ---------------------------------------------------------------- the install
pc::Layout NewLayout(const wchar_t* name) {
    pc::Layout l;
    l.root = g_tmp + L"\\" + name;
    pc::DeleteTree(l.root);
    pc::MakeDirs(l.root);
    return l;
}

void Install() {
    Fixture fx = StandardFixture();
    const std::string pj = fx.PackJson(fx.version);
    const std::wstring zip = g_tmp + L"\\pack.zip";
    CHECK(WriteZip(zip, ZipEntries(fx, pj)), "pack zip written");
    const pl::Manifest m = ManifestFor(fx, pj, zip);
    std::string why, err, detail;
    CHECK(pl::CheckManifest(m, &why), "fixture manifest: " + why);
    pc::Layout l = NewLayout(L"media");
    // Leftovers the install must clean up, and things it must leave alone.
    pc::MakeDirs(l.root + L"\\6");
    WriteBytes(l.root + L"\\6\\pack.json", "{}");
    pc::MakeDirs(l.root + L"\\notes");
    pc::MakeDirs(l.PackDir(m.version));
    WriteBytes(l.PackDir(m.version) + L"\\stale.txt", "old");

    // Free the fixture's own big strings first, so the heap peak below is the installer's.
    const std::string libmpv = fx.files[1].data;
    fx = Fixture{};
    pc::Heap().Reset();
    g_newPeak = g_newCur.load();
    const int64_t newBase = g_newCur.load();
    pc::Hooks h;
    std::vector<std::string> phases;
    h.phase = [&](const char* p) {
        if (phases.empty() || phases.back() != p) phases.push_back(p);
    };
    const bool ok = pc::InstallPack(l, m, m.zipUrl, h, &err, &detail);
    const int64_t peak = pc::Heap().peak.load() + (g_newPeak.load() - newBase);
    CHECK(ok, "install: " + err + " " + detail);
    printf("    install heap high-water: %lld KiB (miniz and buffers %lld KiB, operator new %lld KiB)\n", peak >> 10,
           pc::Heap().peak.load() >> 10, (g_newPeak.load() - newBase) >> 10);
    CHECK(peak > 0 && peak <= static_cast<int64_t>(pc::kMaxHeapBytes), "install heap high-water <= 4 MiB");
    CHECK(phases.size() >= 3 && phases.front() == "downloading" && phases.back() == "installing", "phases reported");
    CHECK(pc::PackPresent(l, m) && pc::VerifyPackFiles(l, m, &detail), "installed files verify: " + detail);
    CHECK(ReadBytes(l.PackDir(m.version) + L"\\libmpv-2.dll") == libmpv, "libmpv extracted intact");
    CHECK(!Exists(l.PackDir(m.version) + L"\\stale.txt"), "a stale pack folder was replaced");
    CHECK(!Exists(l.PackDir(m.version) + L".tmp"), "no tmp left");
    CHECK(!Exists(l.root + L"\\6"), "an older pack version was removed");
    CHECK(Exists(l.root + L"\\notes"), "an unrelated folder was kept");
    CHECK(pc::RecordedPackJsonTime(l, m) != 0 && pc::RecordedPackJsonTime(l, m) == pc::PackJsonTime(l, m), "installed.json records pack.json");
    CHECK(!Exists(l.Dl() + L"\\pack-7-" + pc::Widen(m.zipSha256.substr(0, 8)) + L".zip.part"), "the zip was deleted");

    // Launch-time hold: a held file can be neither written nor deleted, and a changed file fails the re-hash.
    HANDLE held = INVALID_HANDLE_VALUE;
    const pl::File* exe = m.Find("holo-media.exe");
    const std::wstring exePath = l.PackDir(m.version) + L"\\holo-media.exe";
    CHECK(pc::OpenHashed(exePath, l.PackDir(m.version), exe->size, exe->sha256, &held, &detail), "open hashed: " + detail);
    HANDLE w = CreateFileW(exePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(w == INVALID_HANDLE_VALUE, "a held file cannot be opened for writing");
    if (w != INVALID_HANDLE_VALUE) CloseHandle(w);
    CHECK(!DeleteFileW(exePath.c_str()), "a held file cannot be deleted");
    CHECK(!pc::Uninstall(l, &err) && err == "in-use" && pc::PackPresent(l, m), "uninstall refused while a file is held, nothing lost");
    HANDLE r = CreateFileW(exePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(r != INVALID_HANDLE_VALUE, "a held file can still be opened for reading (the helper's launch)");
    if (r != INVALID_HANDLE_VALUE) CloseHandle(r);
    CloseHandle(held);
    CHECK(!pc::OpenHashed(exePath, l.PackDir(m.version), exe->size, std::string(64, '1'), &held, &detail) && held == INVALID_HANDLE_VALUE,
          "a wrong hash fails the launch check");
    CHECK(!pc::OpenHashed(exePath, l.root + L"\\notes", exe->size, exe->sha256, &held, &detail), "a file outside its folder fails");

    // A second install of the same pack (another instance finished first) and a re-install over the present one.
    CHECK(pc::InstallPack(l, m, m.zipUrl, pc::Hooks{}, &err, &detail) && pc::PackPresent(l, m), "re-install: " + detail);

    // Sweep: dead tmp folders and old parts go; fresh parts stay.
    pc::MakeDirs(l.root + L"\\7.tmp\\x");
    pc::MakeDirs(l.LaneRoot() + L"\\2026.01.01.tmp");
    pc::MakeDirs(l.Dl());
    WriteBytes(l.Dl() + L"\\old.zip.part", "x");
    WriteBytes(l.Dl() + L"\\new.zip.part", "x");
    {
        HANDLE f = CreateFileW((l.Dl() + L"\\old.zip.part").c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        ULARGE_INTEGER u;
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        u.QuadPart -= 8ull * 24 * 3600 * 10000000ull;
        ft.dwLowDateTime = u.LowPart;
        ft.dwHighDateTime = u.HighPart;
        SetFileTime(f, nullptr, nullptr, &ft);
        CloseHandle(f);
    }
    pc::Sweep(l);
    CHECK(!Exists(l.root + L"\\7.tmp") && !Exists(l.LaneRoot() + L"\\2026.01.01.tmp"), "sweep removed tmp folders");
    CHECK(!Exists(l.Dl() + L"\\old.zip.part") && Exists(l.Dl() + L"\\new.zip.part"), "sweep removed only week-old parts");
    CHECK(pc::PackPresent(l, m) && Exists(l.root + L"\\notes"), "sweep kept the pack and other folders");

    // DeleteTree never follows a junction.
    const std::wstring outside = g_tmp + L"\\outside";
    pc::MakeDirs(outside);
    WriteBytes(outside + L"\\keep.txt", "keep");
    const std::wstring link = l.PackDir(m.version) + L"\\LICENSES\\link";
    const std::string cmd = "cmd /c mklink /J \"" + pc::Narrow(link) + "\" \"" + pc::Narrow(outside) + "\" >nul 2>nul";
    const bool junction = system(cmd.c_str()) == 0 && Exists(link + L"\\keep.txt");
    if (!junction) printf("    (no junction support here; skipped the junction case)\n");

    CHECK(pc::Uninstall(l, &err) && !Exists(l.PackDir(m.version)) && !Exists(l.root + L"\\installed.json"), "uninstall: " + err);
    if (junction) CHECK(Exists(outside + L"\\keep.txt"), "uninstall did not follow a junction");
    CHECK(Exists(l.root + L"\\notes"), "uninstall left unrelated folders");
    DeleteFileW(zip.c_str());
}

// Hostile and broken zips: each must fail with nothing left in the pack folder.
void Hostile() {
    Fixture fx;
    fx.version = "9";
    fx.files = {{"holo-media.exe", Noise(64 << 10, 3), 6}, {"libmpv-2.dll", Pattern(256 << 10), 6}, {"deno.exe", Noise(32 << 10, 4), 0}};
    const std::string pj = fx.PackJson(fx.version);
    const std::wstring zip = g_tmp + L"\\hostile.zip";
    std::string err, detail;
    auto expectFail = [&](const char* what, const std::vector<Entry>& entries, const pl::Manifest& m, const char* wantErr) {
        pc::Layout l = NewLayout(L"hostile");
        CHECK(WriteZip(zip, entries), std::string("zip written: ") + what);
        pl::Manifest mm = m;
        const std::string z = ReadBytes(zip);
        mm.zipSize = z.size();
        mm.zipSha256 = Sha256Hex(z.data(), z.size());  // the zip itself matches; the content does not
        mm.zipUrl = FileUrl(zip);
        const bool ok = pc::InstallPack(l, mm, mm.zipUrl, pc::Hooks{}, &err, &detail);
        CHECK(!ok && err == wantErr, std::string("refused (") + wantErr + "): " + what + " -> " + err + " " + detail);
        CHECK(!Exists(l.PackDir(mm.version)) && !Exists(l.PackDir(mm.version) + L".tmp"), std::string("nothing installed: ") + what);
        CHECK(!Exists(g_tmp + L"\\evil.dll") && !Exists(l.root + L"\\evil.dll"), std::string("nothing escaped: ") + what);
    };
    std::vector<Entry> good = {fx.files[0], fx.files[1], fx.files[2], {"pack.json", pj, 6}};
    const pl::Manifest m = [&] {
        CHECK(WriteZip(zip, good), "good hostile-base zip");
        return ManifestFor(fx, pj, zip);
    }();
    {
        pc::Layout l = NewLayout(L"hostile");
        CHECK(pc::InstallPack(l, m, m.zipUrl, pc::Hooks{}, &err, &detail), "the base zip installs: " + detail);
    }
    auto with = [&](auto mutate) {
        std::vector<Entry> v = good;
        mutate(v);
        return v;
    };
    // InstallPack's own codes: "zip" (refused by the plan), "hash" (an entry's bytes), "manifest" (pack.json); pack.cpp
    // shows the first and last as "hash" too.
    expectFail("extra entry", with([](auto& v) { v.push_back({"evil.dll", "MZ", 6}); }), m, "zip");
    expectFail("traversal entry", with([](auto& v) { v.push_back({"../evil.dll", "MZ", 6}); }), m, "zip");
    expectFail("traversal in a folder", with([](auto& v) { v.push_back({"LICENSES/../../evil.dll", "MZ", 6}); }), m, "zip");
    expectFail("duplicate entry", with([](auto& v) { v.push_back(v[0]); }), m, "zip");
    expectFail("missing entry", with([](auto& v) { v.erase(v.begin() + 2); }), m, "zip");
    expectFail("size mismatch", with([](auto& v) { v[0].data += "x"; }), m, "zip");
    expectFail("same size, other bytes", with([](auto& v) { v[1].data[100] ^= 1; }), m, "hash");
    expectFail("same size, other bytes, stored", with([](auto& v) { v[2].data[7] ^= 1; }), m, "hash");
    expectFail("case variant", with([](auto& v) { v[2].name = "DENO.exe"; }), m, "zip");
    expectFail("pack.json of another version", with([&](auto& v) { v[3].data = fx.PackJson("10"); }), [&] {
        pl::Manifest x = m;
        for (pl::File& f : x.files)
            if (f.path == "pack.json") {
                const std::string other = fx.PackJson("10");
                f.size = other.size();
                f.sha256 = Sha256Hex(other.data(), other.size());
            }
        return x;
    }(), "manifest");
    // Not a zip at all, with a matching hash.
    {
        pc::Layout l = NewLayout(L"hostile");
        WriteBytes(zip, "this is not a zip file");
        pl::Manifest x = m;
        x.zipSize = 22;
        const std::string s = ReadBytes(zip);
        x.zipSha256 = Sha256Hex(s.data(), s.size());
        CHECK(!pc::InstallPack(l, x, FileUrl(zip), pc::Hooks{}, &err, &detail) && err == "zip", "not a zip: " + err);
    }
    // The download's own hash differs from the manifest.
    {
        pc::Layout l = NewLayout(L"hostile");
        WriteZip(zip, good);
        pl::Manifest x = m;
        x.zipSha256 = std::string(64, 'f');
        x.zipSize = ReadBytes(zip).size();
        CHECK(!pc::InstallPack(l, x, FileUrl(zip), pc::Hooks{}, &err, &detail) && err == "hash", "zip hash mismatch: " + err);
    }
    pc::DeleteTree(g_tmp + L"\\hostile");
    DeleteFileW(zip.c_str());
}

// ---------------------------------------------------------------- the yt-dlp lane
void Lane() {
    pc::Layout l = NewLayout(L"lane");
    std::string action, err, detail;
    pc::LaneState st;
    auto publish = [&](const std::string& version, const std::string& exe, uint64_t serial, const char* badSha = nullptr) {
        const std::wstring exePath = g_tmp + L"\\yt-dlp-" + pc::Widen(version) + L".exe";
        WriteBytes(exePath, exe);
        const std::string j = "{\"format\":\"melange-media-ytdlp-1\",\"version\":\"" + version + "\",\"url\":\"" + FileUrl(exePath) +
                              "\",\"size\":" + std::to_string(exe.size()) + ",\"sha256\":\"" +
                              (badSha ? std::string(badSha) : Sha256Hex(exe.data(), exe.size())) + "\",\"serial\":" + std::to_string(serial) + "}";
        WriteBytes(g_tmp + L"\\media-ytdlp.json", j);
        return FileUrl(g_tmp + L"\\media-ytdlp.json");
    };
    const std::string exeA = "MZ yt-dlp A " + Noise(300 << 10, 5), exeB = "MZ yt-dlp B " + Noise(200 << 10, 6);
    std::string url = publish("2026.10.01", exeA, 5);
    CHECK(pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && action == "install" && st.lane.serial == 5 &&
              ReadBytes(st.exe) == exeA,
          "first lane install: " + err + " " + detail);
    CHECK(pc::ReadLane(l).exe == st.exe && pc::ReadLane(l).lastCheck > 0, "lane.json recorded");
    CHECK(pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && action == "current", "same lane: current");
    url = publish("2026.09.01", exeB, 4);
    CHECK(!pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && err == "rollback", "lower serial: rollback refused");
    CHECK(pc::ReadLane(l).lane.version == "2026.10.01" && ReadBytes(pc::ReadLane(l).exe) == exeA, "rollback kept the installed yt-dlp");
    url = publish("2026.10.02", exeB, 5);
    CHECK(!pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && err == "conflict", "same serial, other file: refused");
    url = publish("2026.10.01", exeA, 6);
    CHECK(pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && action == "record" && pc::ReadLane(l).lane.serial == 6,
          "higher serial, same file: recorded");
    url = publish("2026.10.21", exeB, 7, "0000000000000000000000000000000000000000000000000000000000000000");
    CHECK(!pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && err == "hash", "a wrong exe hash refused");
    CHECK(pc::ReadLane(l).lane.version == "2026.10.01" && !pc::ReadLane(l).exe.empty(), "a failed update keeps the current yt-dlp");
    url = publish("2026.10.21", exeB, 7);
    CHECK(pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && action == "install" && ReadBytes(st.exe) == exeB,
          "update: " + err + " " + detail);
    CHECK(!Exists(l.LaneRoot() + L"\\2026.10.01"), "the old yt-dlp folder was removed");
    // The exe deleted by hand: the same serial installs it again.
    DeleteFileW(st.exe.c_str());
    CHECK(pc::UpdateLane(l, url, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && action == "install" && ReadBytes(st.exe) == exeB,
          "a missing exe is fetched again");
    // A lane manifest from an https URL outside the prefix is refused before any fetch.
    CHECK(!pc::UpdateLane(l, "https://example.com/media-ytdlp.json", kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && err == "manifest",
          "foreign lane URL refused");
    // A file:/// lane naming an https exe of another host.
    WriteBytes(g_tmp + L"\\media-ytdlp.json", "{\"format\":\"melange-media-ytdlp-1\",\"version\":\"2026.11.01\",\"url\":\"https://example.com/y.exe\","
                                              "\"size\":10,\"sha256\":\"" + std::string(64, 'a') + "\",\"serial\":9}");
    CHECK(!pc::UpdateLane(l, FileUrl(g_tmp + L"\\media-ytdlp.json"), kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && err == "manifest",
          "lane exe on a foreign host refused");
    pc::DeleteTree(l.root);
}

// The manifest compiled into this build: the placeholder (no pack before R1), the published one, or a dev pack from
// scripts/make-dev-pack.ps1. A real one must pass the build's own rules; a dev pack whose zip is still on disk installs.
void Compiled() {
    namespace mf = melange::media::manifest;
    if (!mf::kHavePack) {
        CHECK(!mf::kDev, "the placeholder manifest is not a dev pack");
        return;
    }
    pl::Manifest m;
    m.version = mf::kPackVersion;
    m.zipUrl = mf::kZipUrl;
    m.zipSize = mf::kZipSize;
    m.zipSha256 = mf::kZipSha256;
    m.laneUrl = mf::kYtdlpLaneUrl;
    m.dev = mf::kDev;
    for (const mf::File& f : mf::kFiles) m.files.push_back({f.path, f.size, f.sha256});
    std::string why, url, err, detail;
    CHECK(pl::CheckManifest(m, &why), "the compiled manifest passes CheckManifest: " + why);
    CHECK(pl::ChoosePackUrl("", m, kPrefix, &url, &why), "the compiled pack URL is allowed: " + why);
    CHECK(pl::AllowedUrl(m.laneUrl, kPrefix, m.dev), "the compiled lane URL is allowed");
    if (!m.dev) {
        CHECK(!pl::IsFileUrl(m.zipUrl) && !pl::IsFileUrl(m.laneUrl), "a release manifest has no file:/// URL");
        return;
    }
    printf("    dev pack %s compiled in\n", m.version.c_str());
    std::wstring zip;
    if (!melange::store::FileUrlToPath(m.zipUrl, &zip) || !Exists(zip)) return;
    pc::Layout l = NewLayout(L"compiled");
    CHECK(pc::InstallPack(l, m, url, pc::Hooks{}, &err, &detail) && pc::VerifyPackFiles(l, m, &detail), "the dev pack installs: " + detail);
    pc::LaneState st;
    std::string action;
    if (pl::IsFileUrl(m.laneUrl))
        CHECK(pc::UpdateLane(l, m.laneUrl, kPrefix, pc::Hooks{}, &st, &action, &err, &detail) && !st.exe.empty(),
              "the dev lane installs: " + err + " " + detail);
    // A dev pack from scripts/media-pack.ps1 -Dev carries the real helper: start it as the core does.
    if (pc::VerifyPackFiles(l, m, &detail)) host_cases::RealHelper(l.PackDir(m.version), st.exe);
    pc::DeleteTree(l.root);
}

inline void Run() {
    g_tmp = TempRoot();
    pc::DeleteTree(g_tmp);
    pc::MakeDirs(g_tmp);
    Rules();
    Downloads();
    Install();
    Hostile();
    Lane();
    Compiled();
    pc::DeleteTree(g_tmp);
}
}  // namespace pack_cases
