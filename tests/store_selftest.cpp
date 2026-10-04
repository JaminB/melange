// Offline self-test for the plugin store: index parsing and caps, URL rules, version selection, dependency plans,
// the zip rules, the install engine against a temp Mods\ folder, file:// fetches and the shared change gate.
// Exit code 0 = all passed.
#include <windows.h>

#include <miniz.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "levels/session.h"
#include "store/fetch.h"
#include "store/index.h"
#include "store/install.h"
#include "store/zipcheck.h"
#include "tools/hash.h"

namespace st = melange::store;
namespace zc = melange::store::zipcheck;
namespace in = melange::store::install;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

const std::string kSha(64, 'a');

std::string Sub(std::string s, const std::string& from, const std::string& to) {
    const size_t at = s.find(from);
    if (at != std::string::npos) s.replace(at, from.size(), to);
    return s;
}

std::string Ver(const std::string& v, const std::string& extra = "") {
    return "{\"version\":\"" + v + "\",\"released\":\"2026-10-01\",\"melange\":\">=0.1.0\",\"kind\":\"client-only\","
           "\"permissions\":{\"unsafe\":false,\"filesystem\":\"none\"},\"dependencies\":[],\"conflicts\":[],"
           "\"url\":\"https://github.com/JaminB/melange-plugins/releases/download/x/x.zip\",\"sha256\":\"" + kSha +
           "\",\"size\":100,\"unpackedSize\":200,\"files\":3,\"changelog\":\"c\"" + extra + "}";
}

std::string Plugin(const std::string& id, const std::string& versions, const std::string& extra = "") {
    return "{\"id\":\"" + id + "\",\"name\":\"" + id + " name\",\"authors\":[\"someone\"],\"description\":\"d\","
           "\"homepage\":\"https://example.com\",\"licence\":\"MIT\",\"categories\":[\"graphics\"],\"gameBuilds\":[\"1077\"],"
           "\"screenshots\":[{\"path\":\"plugins/" + id + "/screenshots/1.png\",\"sha256\":\"" + kSha +
           "\",\"size\":10,\"caption\":\"c\"}],\"versions\":[" + versions + "]" + extra + "}";
}

std::string IndexOf(const std::vector<std::string>& plugins, int serial = 3) {
    std::string s = "{\"indexVersion\":1,\"serial\":" + std::to_string(serial) + ",\"plugins\":[";
    for (size_t i = 0; i < plugins.size(); ++i) s += (i ? "," : "") + plugins[i];
    return s + "]}";
}

void TestIndex() {
    st::Index idx;
    std::string err;
    Expect(st::ParseIndex(IndexOf({Plugin("hd-water", Ver("1.0.0") + "," + Ver("1.1.0")), Plugin("alpha", Ver("0.1.0"))}), &idx, &err),
           "a good index parses: " + err);
    Expect(idx.serial == 3 && idx.plugins.size() == 2 && idx.plugins[0].id == "alpha", "plugins sorted by id");
    Expect(idx.plugins[1].versions.size() == 2 && idx.plugins[1].versions[0].version == "1.1.0", "versions newest first");
    Expect(idx.plugins[1].screenshots.size() == 1 && idx.plugins[1].versions[0].size == 100, "fields read");
    {
        const std::string im = ",\"imports\":[{\"title\":\"Maps 1\",\"publisher\":\"p\",\"host\":\"example.com\",\"size\":1000}]";
        Expect(st::ParseIndex(IndexOf({Plugin("imp", Ver("1.0.0"), im)}), &idx, &err) && idx.plugins.size() == 1 &&
                   idx.plugins[0].imports.size() == 1 && idx.plugins[0].imports[0].host == "example.com" && idx.plugins[0].imports[0].size == 1000,
               "imports read");
        Expect(st::ParseIndex(IndexOf({Plugin("imp", Ver("1.0.0"), ",\"imports\":[{\"title\":\"x\"}]")}), &idx, &err) && idx.plugins.empty(),
               "an imports entry without host and size drops the plugin");
    }

    Expect(!st::ParseIndex(std::string(st::kMaxIndexBytes + 1, ' '), &idx, &err), "over 1 MiB refused");
    Expect(!st::ParseIndex("{\"indexVersion\":2,\"serial\":1,\"plugins\":[]}", &idx, &err) && err.find("indexVersion") != std::string::npos,
           "unknown indexVersion refused");
    Expect(!st::ParseIndex("{\"indexVersion\":1,\"plugins\":[]}", &idx, &err), "missing serial refused");
    Expect(!st::ParseIndex("{\"indexVersion\":1,\"serial\":1,\"plugins\":[{\"name\":\"\xff\"}]}", &idx, &err), "invalid UTF-8 refused");
    {
        std::vector<std::string> many;
        for (int i = 0; i < 501; ++i) many.push_back("{\"id\":\"p" + std::to_string(i) + "\"}");
        Expect(!st::ParseIndex(IndexOf(many), &idx, &err), "501 plugins refused");
    }
    {
        std::string vs;
        for (int i = 0; i < 51; ++i) vs += (i ? "," : "") + Ver("1.0." + std::to_string(i));
        Expect(st::ParseIndex(IndexOf({Plugin("many", vs), Plugin("ok", Ver("1.0.0"))}), &idx, &err) && idx.plugins.size() == 1 &&
                   idx.skipped.size() == 1, "a plugin with 51 versions is dropped");
    }
    Expect(st::ParseIndex(IndexOf({"{\"id\":\"long\",\"name\":\"" + std::string(81, 'n') + "\",\"versions\":[" + Ver("1.0.0") + "]}"}),
                          &idx, &err) && idx.plugins.empty(), "a name over 80 characters drops the plugin");
    Expect(st::ParseIndex(IndexOf({Plugin("Bad_Id", Ver("1.0.0"))}), &idx, &err) && idx.plugins.empty(), "an invalid id is dropped");
    Expect(st::ParseIndex(IndexOf({Plugin("a-b", Ver("1.0.0")), Plugin("a_b", Ver("1.0.0"))}), &idx, &err) && idx.plugins.size() == 1,
           "-/_ twins: only one listed");
    Expect(st::ParseIndex(IndexOf({Plugin("bad", Sub(Ver("1.0.0"), kSha, "XYZ"))}), &idx, &err) && idx.plugins.empty(),
           "a bad sha256 drops the version");
    Expect(st::ParseIndex(IndexOf({Plugin("net", Sub(Ver("1.0.0"), "\"filesystem\":\"none\"}", "\"filesystem\":\"none\",\"network\":true}"))}),
                          &idx, &err) && idx.plugins.empty(), "permissions.network refused");
    Expect(st::ParseIndex(IndexOf({Sub(Plugin("desc", Ver("1.0.0")), "\"description\":\"d\"", "\"description\":\"" + std::string(401, 'd') + "\"")}),
                          &idx, &err) && idx.plugins.empty(), "a description over 400 characters drops the plugin");
    Expect(st::ParseIndex(IndexOf({Sub(Plugin("home", Ver("1.0.0")), "https://example.com", "http://example.com")}), &idx, &err) &&
               idx.plugins.empty(), "a non-https homepage drops the plugin");
}

void TestUrls() {
    std::string why, out;
    Expect(st::CheckIndexUrl("https://raw.githubusercontent.com/JaminB/melange-plugins/main/index.json", &why), "https index allowed");
    Expect(st::CheckIndexUrl("file:///C:/fixtures/out/index.json", &why), "file:/// index allowed");
    Expect(!st::CheckIndexUrl("http://example.com/index.json", &why), "http:// refused");
    Expect(!st::CheckIndexUrl("ftp://example.com/index.json", &why), "ftp:// refused");
    Expect(!st::CheckIndexUrl("file://server/share/index.json", &why), "UNC file URL refused");
    Expect(!st::CheckIndexUrl("https:///index.json", &why), "https without host refused");
    Expect(st::ResolveUrl("https://raw.githubusercontent.com/o/r/main/index.json", "plugins/a/screenshots/1.png", &out, &why) &&
               out == "https://raw.githubusercontent.com/o/r/main/plugins/a/screenshots/1.png", "relative path against https");
    Expect(st::ResolveUrl("file:///C:/fx/out/index.json", "plugins/a/1.png", &out, &why) && out == "file:///C:/fx/out/plugins/a/1.png",
           "relative path against file:///");
    Expect(!st::ResolveUrl("https://h/x/index.json", "file:///C:/evil.zip", &out, &why) && why == "url scheme not allowed",
           "file:// url refused under an https index");
    Expect(st::ResolveUrl("file:///C:/fx/index.json", "file:///C:/fx/releases/a.zip", &out, &why), "file:// url under a file index");
    Expect(st::ResolveUrl("file:///C:/fx/index.json", "https://h/a.zip", &out, &why), "https url under a file index");
    Expect(!st::ResolveUrl("https://h/x/index.json", "../secret", &out, &why), "'..' refused");
    Expect(!st::ResolveUrl("https://h/x/index.json", "http://h/a.zip", &out, &why), "http:// entry refused");
    Expect(!st::ResolveUrl("https://h/x/index.json", "/abs/a.png", &out, &why), "rooted path refused");
    std::wstring p;
    Expect(st::FileUrlToPath("file:///C:/a%20b/c.zip", &p) && p == L"C:\\a b\\c.zip", "file URL decoded");
    Expect(!st::FileUrlToPath("file:///C:/a/../b", &p), "file URL with '..' refused");
}

st::Plugin MakePlugin(std::vector<std::pair<std::string, bool>> versions, const std::string& range = ">=0.1.0") {
    st::Plugin p;
    p.id = "p";
    p.name = "P";
    p.gameBuilds = {"1077"};
    for (auto& [v, yanked] : versions) {
        st::Version x;
        x.version = v;
        x.melange = range;
        x.yanked = yanked;
        x.kind = "client-only";
        p.versions.push_back(x);
    }
    return p;
}

void TestChoose() {
    const st::Env env{"0.2.0", "1077", false};
    st::Plugin p = MakePlugin({{"1.2.0", false}, {"1.1.0", false}, {"1.0.0", false}});
    st::Choice c = st::Choose(p, {}, env);
    Expect(c.action == st::Action::Install && c.compatible && c.compatible->version == "1.2.0", "not installed: install the newest");
    st::Plugin old = MakePlugin({{"1.0.0", false}}, ">=0.9.0");
    c = st::Choose(old, {}, env);
    Expect(c.action == st::Action::None && c.state == "incompatible" && c.reason.find("needs Melange >=0.9.0") != std::string::npos,
           "incompatible Melange range");
    st::Plugin other = p;
    other.gameBuilds = {"9999"};
    c = st::Choose(other, {}, env);
    Expect(c.action == st::Action::None && c.reason.find("not tested on this game build") != std::string::npos, "other game build");
    c = st::Choose(p, {}, st::Env{"0.2.0", "", false});
    Expect(c.action == st::Action::None && c.reason.find("not recognised") != std::string::npos, "unknown exe matches nothing");
    c = st::Choose(p, {true, true, false, "1.1.0"}, env);
    Expect(c.action == st::Action::Update && c.canRemove && c.compatible->version == "1.2.0", "managed older: update");
    c = st::Choose(p, {true, true, false, "1.2.0"}, env);
    Expect(c.action == st::Action::Remove && c.state == "installed", "managed equal: remove");
    c = st::Choose(p, {true, true, false, "2.0.0"}, env);
    Expect(c.action == st::Action::Remove, "managed newer (pinned old index): no downgrade");
    st::Plugin y = MakePlugin({{"1.2.0", true}, {"1.1.0", false}});
    c = st::Choose(y, {}, env);
    Expect(c.compatible && c.compatible->version == "1.1.0", "a yanked version is never offered");
    c = st::Choose(y, {true, true, false, "1.2.0"}, env);
    Expect(c.state == "yanked" && c.reason.find("withdrawn") != std::string::npos && c.action == st::Action::Remove,
           "installed yanked: warning, no downgrade");
    st::Plugin y2 = MakePlugin({{"1.3.0", false}, {"1.2.0", true}});
    c = st::Choose(y2, {true, true, false, "1.2.0"}, env);
    Expect(c.state == "yanked" && c.action == st::Action::Update, "installed yanked: update when possible");
    c = st::Choose(p, {true, false, false, "1.0.0"}, env);
    Expect(c.state == "manual" && c.action == st::Action::Update && !c.canRemove, "manual older: update after a confirm");
    c = st::Choose(p, {true, false, false, "1.2.0"}, env);
    Expect(c.state == "manual" && c.action == st::Action::Install, "manual equal: replace after a confirm");
    c = st::Choose(p, {true, true, true, "1.1.0"}, env);
    Expect(c.state == "pending" && c.action == st::Action::None, "pending: applies at next launch");
    c = st::Choose(p, {}, st::Env{"0.2.0", "1077", true});
    Expect(c.action == st::Action::None && c.reason == st::kRollbackText, "rollback: no install");
    c = st::Choose(p, {true, true, false, "1.0.0"}, st::Env{"0.2.0", "1077", true});
    Expect(c.action == st::Action::Remove && c.reason == st::kRollbackText, "rollback: no update, remove still offered");

    st::Index idx;
    std::string err;
    Expect(st::ParseIndex(IndexOf({Plugin("pre", Ver("1.1.0-rc.1") + "," + Ver("1.0.0") + "," + Ver("1.1.0-beta"))}), &idx, &err) &&
               idx.plugins[0].versions[0].version == "1.1.0-rc.1" && idx.plugins[0].versions[2].version == "1.0.0",
           "prerelease ordering");
}

void TestPlan() {
    st::Index idx;
    std::string err;
    const std::string depA = Sub(Ver("1.0.0"), "\"dependencies\":[]", "\"dependencies\":[\"b ^1.0.0\",\"c\"]");
    const std::string depB = Sub(Ver("1.2.0"), "\"dependencies\":[]", "\"dependencies\":[{\"id\":\"c\",\"range\":\">=0.3.0\"}]");
    Expect(st::ParseIndex(IndexOf({Plugin("a", depA), Plugin("b", depB), Plugin("c", Ver("0.3.1"))}), &idx, &err), "plan fixture: " + err);
    const st::Env env{"0.2.0", "1077", false};
    std::vector<st::Step> steps;
    Expect(st::PlanInstall(idx, "a", "1.0.0", {}, env, &steps, &err) && steps.size() == 3 && steps[0].id == "c" && steps[1].id == "b" &&
               steps[2].id == "a", "dependencies install first, in order");
    Expect(st::PlanInstall(idx, "a", "1.0.0", {{"c", "0.4.0"}}, env, &steps, &err) && steps.size() == 2, "installed dependency skipped");
    Expect(st::PlanInstall(idx, "a", "1.0.0", {{"c", "0.2.0"}}, env, &steps, &err) && steps.size() == 3,
           "an installed dependency too old is updated");
    st::Index miss;
    Expect(st::ParseIndex(IndexOf({Plugin("a", depA)}), &miss, &err), "missing-dep fixture");
    Expect(!st::PlanInstall(miss, "a", "1.0.0", {}, env, &steps, &err) && err.find("needs b") != std::string::npos,
           "a missing dependency blocks with its name");
}

// --- zips -------------------------------------------------------------------------------------------------
std::wstring g_tmp;

std::wstring Tmp(const std::wstring& name) { return g_tmp + L"\\" + name; }

struct ZipFile { std::string name, data; };

// miniz refuses some hostile names, so each entry is written under a same-length placeholder that is patched afterwards.
std::string MakeZip(const std::vector<ZipFile>& files) {
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    std::vector<std::string> holders;
    for (size_t i = 0; i < files.size(); ++i) {
        std::string h = "Q" + std::to_string(i) + "Z";
        h.resize(files[i].name.size() < h.size() ? h.size() : files[i].name.size(), 'Q');
        holders.push_back(h);
        mz_zip_writer_add_mem(&z, h.c_str(), files[i].data.data(), files[i].data.size(), MZ_BEST_COMPRESSION);
    }
    void* buf = nullptr;
    size_t n = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &n);
    std::string out(static_cast<const char*>(buf), n);
    mz_zip_writer_end(&z);
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].name.size() != holders[i].size()) continue;
        for (size_t at = out.find(holders[i]); at != std::string::npos; at = out.find(holders[i], at + 1))
            out.replace(at, holders[i].size(), files[i].name);
    }
    return out;
}

void WriteFile(const std::wstring& path, const std::string& data) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

std::string ReadFile(const std::wstring& path) {
    std::string s;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

std::string Spice(const std::string& id, const std::string& version, const std::string& extra = "") {
    return "{\"spiceVersion\":1,\"id\":\"" + id + "\",\"version\":\"" + version + "\",\"name\":\"Hello\",\"authors\":[\"me\"],"
           "\"melange\":{\"range\":\">=0.1.0\"},\"kind\":\"client-only\",\"entry\":{\"client\":\"client/init.lua\"}" + extra + "}";
}

std::vector<ZipFile> HelloFiles(const std::string& version, const std::string& extra = "") {
    return {{"hello/spice.json", Spice("hello", version, extra)}, {"hello/LICENSE", "MIT"},
            {"hello/client/init.lua", "wum.log.info('hello " + version + "')"}};
}

bool InspectZip(const std::string& zip, const std::string& id = "hello", std::string* why = nullptr) {
    const std::wstring path = Tmp(L"inspect.zip");
    WriteFile(path, zip);
    std::vector<zc::Entry> e;
    std::string w;
    const bool ok = in::Inspect(path, id, 1 << 20, &e, &w);
    if (why) *why = w;
    return ok;
}

void TestZipRules() {
    std::string why;
    Expect(InspectZip(MakeZip(HelloFiles("1.0.0")), "hello", &why), "a good zip passes: " + why);
    auto bad = [&](const char* name, const char* what) {
        std::vector<ZipFile> f = HelloFiles("1.0.0");
        f.push_back({name, "x"});
        std::string w;
        Expect(!InspectZip(MakeZip(f), "hello", &w) && w.rfind(name, 0) == 0, std::string(what) + " (" + w + ")");
    };
    bad("hello/../x", "../x refused");
    bad("../x", "leading .. refused");
    bad("C:/x", "drive letter refused");
    bad("hello/a/./b", "./ segment refused");
    bad("hello/CON.txt", "device name refused");
    bad("hello/lpt1", "device name without extension refused");
    bad("hello/trailing.", "trailing dot refused");
    bad("hello/trailing ", "trailing space refused");
    bad("hello/SPICE.json", "case-duplicate refused");
    bad("other/x.lua", "two top folders refused");
    bad("hello/.hidden", "hidden file refused");
    bad("hello/user/save.txt", "user/ refused");
    bad("hello/x/storage.json", "reserved file name refused");
    bad("hello/tool.exe", "executable extension refused");
    bad("hello/a/b/c/d/e/f/g/h/i.txt", "more than 8 segments refused");
    bad("hello/caf\xc3\xa9.txt", "non-ASCII name refused");
    Expect(!InspectZip(MakeZip(HelloFiles("1.0.0")), "other"), "wrong top folder refused");
    Expect(!InspectZip(MakeZip({{"hello/LICENSE", "x"}})), "zip without spice.json refused");
    Expect(!InspectZip("not a zip at all"), "garbage refused");

    std::vector<zc::Entry> list = {{"hello/spice.json", 1, 1, 8, 0, 0x0314, 0}, {"hello/link", 1, 1, 0, 0, 0x0314, 0120777u << 16}};
    Expect(!zc::Check(list, "hello", 1 << 20, &why) && why.find("link") != std::string::npos, "symlink mode refused");
    list[1] = {"hello/a.txt", 1, 1, 8, 0, 0x0314, 0644u << 16};
    Expect(zc::Check(list, "hello", 1 << 20, &why), "unix permission bits without a file type accepted");
    list[1] = {"hello/a.txt", 1, 1, 0, 0, 0x0014, 0x400};
    Expect(!zc::Check(list, "hello", 1 << 20, &why), "reparse attribute refused");
    list[1] = {"hello/a.txt", 1, 1, 8, 1, 0x0014, 0};
    Expect(!zc::Check(list, "hello", 1 << 20, &why) && why.find("encrypted") != std::string::npos, "encrypted flag refused");
    list[1] = {"hello/a.txt", 1, 1, 14, 0, 0x0014, 0};
    Expect(!zc::Check(list, "hello", 1 << 20, &why) && why.find("method") != std::string::npos, "method 14 refused");
    list[1] = {"hello/a.txt", 1, zc::kMaxEntryBytes + 1, 8, 0, 0x0014, 0};
    Expect(!zc::Check(list, "hello", 1ull << 30, &why), "a file over 32 MiB refused");
    list[1] = {"hello/a.txt", 1, 2000, 8, 0, 0x0014, 0};
    Expect(!zc::Check(list, "hello", 1000, &why), "total over the record refused");
    list.resize(1);
    for (int i = 0; i < 2000; ++i) list.push_back({"hello/f" + std::to_string(i), 1, 1, 0, 0, 0x0014, 0});
    Expect(!zc::Check(list, "hello", 1 << 20, &why) && why.find("2000") != std::string::npos, "2001 entries refused");
    Expect(zc::ExecutableMagic("MZ\x90", 3) && zc::ExecutableMagic("\x7f" "ELF", 4) && !zc::ExecutableMagic("PK", 2), "magic numbers");
}

in::Expect HelloExpect(const std::string& version, const std::string& zip) {
    in::Expect e;
    e.id = "hello";
    e.version = version;
    e.sha256 = melange::hashutil::Sha256Hex(zip.data(), zip.size());
    e.size = zip.size();
    e.unpackedSize = 1 << 20;
    return e;
}

std::string FileUrl(const std::wstring& path) {
    std::string s = "file:///";
    for (wchar_t c : path) s.push_back(c == L'\\' ? '/' : static_cast<char>(c));
    return s;
}

void TestEngine() {
    const std::wstring mods = Tmp(L"Mods");
    CreateDirectoryW(mods.c_str(), nullptr);
    const in::Paths p = in::MakePaths(mods);
    Expect(in::EnsureDirs(p), "store folders created");
    std::string err;

    const std::string z1 = MakeZip(HelloFiles("1.0.0"));
    const std::wstring z1p = Tmp(L"hello-1.0.0.zip");
    WriteFile(z1p, z1);
    in::Expect e1 = HelloExpect("1.0.0", z1);

    const std::wstring part = p.Dl() + L"\\hello-1.0.0.zip.part";
    melange::store::fetch::Options fo;
    Expect(in::FetchVerified(FileUrl(z1p), part, e1, 1 << 20, fo, &err), "file:// download verified: " + err);
    in::Expect wrong = e1;
    wrong.sha256 = std::string(64, '0');
    Expect(!in::FetchVerified(FileUrl(z1p), part, wrong, 1 << 20, fo, &err) && err.find("sha256") != std::string::npos &&
               err.find(wrong.sha256) != std::string::npos && !in::Exists(part), "hash mismatch: both hashes named, part file gone");
    wrong = e1;
    wrong.size += 1;
    Expect(!in::FetchVerified(FileUrl(z1p), part, wrong, 1 << 20, fo, &err) && err.find("size") != std::string::npos && !in::Exists(part),
           "length mismatch: part file gone");
    Expect(!in::FetchVerified(FileUrl(z1p), part, e1, 16, fo, &err) && !in::Exists(part), "over the cap: refused");
    Expect(in::VerifyFile(z1p, e1.size, e1.sha256, &err) && !in::VerifyFile(z1p, e1.size, std::string(64, '1'), &err), "VerifyFile");

    in::Staged s;
    Expect(in::Stage(p, z1p, e1, "0.2.0", &s, &err), "stage 1.0.0: " + err);
    Expect(in::PlaceNew(p, "hello", s, &err) == in::Result::Done && in::Exists(mods + L"\\hello\\spice.json") &&
               in::Exists(mods + L"\\hello\\LICENSE") && !in::Exists(s.root), "install places Mods\\hello and drops staging");

    CreateDirectoryW((mods + L"\\hello\\user").c_str(), nullptr);
    WriteFile(mods + L"\\hello\\user\\data.txt", "keep me");
    const std::string z2 = MakeZip(HelloFiles("1.1.0"));
    const std::wstring z2p = Tmp(L"hello-1.1.0.zip");
    WriteFile(z2p, z2);
    in::Expect e2 = HelloExpect("1.1.0", z2);
    Expect(in::Stage(p, z2p, e2, "0.2.0", &s, &err), "stage 1.1.0");
    Expect(in::Replace(p, "hello", s, &err) == in::Result::Done && ReadFile(mods + L"\\hello\\spice.json").find("1.1.0") != std::string::npos &&
               ReadFile(mods + L"\\hello\\user\\data.txt") == "keep me", "update keeps user\\");

    const std::string z3 = MakeZip(HelloFiles("1.2.0"));
    const std::wstring z3p = Tmp(L"hello-1.2.0.zip");
    WriteFile(z3p, z3);
    in::Expect e3 = HelloExpect("1.2.0", z3);
    Expect(in::Stage(p, z3p, e3, "0.2.0", &s, &err), "stage 1.2.0");
    int calls = 0;
    auto failSecond = [&](const std::wstring& from, const std::wstring& to) -> unsigned long {
        ++calls;
        if (from == s.dir && to == mods + L"\\hello") return ERROR_ACCESS_DENIED + 1000;
        return in::DefaultMove(from, to);
    };
    Expect(in::Replace(p, "hello", s, &err, failSecond) == in::Result::Failed &&
               ReadFile(mods + L"\\hello\\spice.json").find("1.1.0") != std::string::npos &&
               ReadFile(mods + L"\\hello\\user\\data.txt") == "keep me" && !in::Exists(s.root), "rollback when the second rename fails");

    Expect(in::Stage(p, z3p, e3, "0.2.0", &s, &err), "stage 1.2.0 again");
    auto busy = [&](const std::wstring& from, const std::wstring& to) -> unsigned long {
        if (from == mods + L"\\hello") return ERROR_SHARING_VIOLATION;
        return in::DefaultMove(from, to);
    };
    Expect(in::Replace(p, "hello", s, &err, busy) == in::Result::Pending && in::Exists(s.dir), "sharing violation: pending, staging kept");
    in::Pending op{"update", "hello", "1.2.0", e3.sha256, s.rel, 4, false};
    Expect(in::SavePending(p, {op}), "pending.json written");
    in::CleanLeftovers(p);
    Expect(in::Exists(s.dir), "leftover cleanup keeps the staging pending.json needs");
    in::Db db;
    db.serialSeen = 4;
    db.mods["hello"] = {"1.1.0", e2.sha256, "t", 3};
    std::vector<in::Applied> applied = in::ApplyPending(p, &db);
    Expect(applied.size() == 1 && applied[0].ok && ReadFile(mods + L"\\hello\\spice.json").find("1.2.0") != std::string::npos &&
               ReadFile(mods + L"\\hello\\user\\data.txt") == "keep me" && db.mods["hello"].version == "1.2.0" &&
               !in::Exists(p.root + L"\\pending.json"), "pending update applied at the next start");
    in::Db loaded;
    Expect(in::LoadDb(p, &loaded) && loaded.serialSeen == 4 && loaded.mods["hello"].version == "1.2.0" &&
               loaded.mods["hello"].sha256 == e3.sha256, "installed.json round trip");

    Expect(in::SavePending(p, {{"update", "hello", "9.9.9", e3.sha256, "stage\\gone-1", 4, false}}), "stale pending written");
    applied = in::ApplyPending(p, &db);
    Expect(applied.size() == 1 && !applied[0].ok && ReadFile(mods + L"\\hello\\spice.json").find("1.2.0") != std::string::npos,
           "a pending op whose staging vanished fails safely");

    const std::string zu = MakeZip(HelloFiles("1.3.0", ",\"permissions\":{\"unsafe\":true}"));
    const std::wstring zup = Tmp(L"hello-unsafe.zip");
    WriteFile(zup, zu);
    in::Expect eu = HelloExpect("1.3.0", zu);
    Expect(!in::Stage(p, zup, eu, "0.2.0", &s, &err) && err.find("declares more") != std::string::npos,
           "staged spice.json declaring more permissions than the index: refused");
    eu.unsafe = true;
    eu.kind = "content";
    Expect(in::Stage(p, zup, eu, "0.2.0", &s, &err), "matching permissions stage: " + err);
    in::DeleteTree(s.root);
    in::Expect ev = e3;
    ev.version = "1.2.1";
    Expect(!in::Stage(p, z3p, ev, "0.2.0", &s, &err) && err.find("1.2.0") != std::string::npos, "version mismatch refused");
    Expect(!in::Stage(p, z3p, e3, "0.0.1", &s, &err) && err.find("needs Melange") != std::string::npos, "melange range enforced");

    std::vector<ZipFile> mz = HelloFiles("1.2.0");
    mz.push_back({"hello/assets/x.png", std::string("MZ\x90\0", 4) + "payload"});
    const std::string zm = MakeZip(mz);
    const std::wstring zmp = Tmp(L"hello-mz.zip");
    WriteFile(zmp, zm);
    Expect(!in::Stage(p, zmp, HelloExpect("1.2.0", zm), "0.2.0", &s, &err) && err.find("executable") != std::string::npos,
           "MZ payload refused");

    std::vector<ZipFile> bomb = HelloFiles("1.2.0");
    bomb.push_back({"hello/big.txt", std::string(64 * 1024, 'z')});
    std::string zb = MakeZip(bomb);
    for (size_t i = 0; i + 46 < zb.size(); ++i) {
        const bool central = !memcmp(&zb[i], "PK\x01\x02", 4), local = !memcmp(&zb[i], "PK\x03\x04", 4);
        if (!central && !local) continue;
        const size_t nameOff = central ? 46 : 30, lenOff = central ? 28 : 26, sizeOff = central ? 24 : 22;
        uint16_t nl;
        memcpy(&nl, &zb[i + lenOff], 2);
        if (std::string(&zb[i + nameOff], nl) != "hello/big.txt") continue;
        const uint32_t small = 1024;
        memcpy(&zb[i + sizeOff], &small, 4);
    }
    const std::wstring zbp = Tmp(L"hello-bomb.zip");
    WriteFile(zbp, zb);
    Expect(!in::Stage(p, zbp, HelloExpect("1.2.0", zb), "0.2.0", &s, &err) && err.find("inflates") != std::string::npos,
           "an entry inflating past its declared size is stopped: " + err);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((p.Stage() + L"\\*").c_str(), &fd);
    int left = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do left += wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..");
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    Expect(left == 0, "failed stagings leave nothing behind");

    Expect(in::PlaceNew(p, "hello", s, &err) == in::Result::Failed, "a new install never overwrites an existing folder");
    Expect(in::Remove(p, "hello", &err, busy) == in::Result::Pending && in::Exists(mods + L"\\hello"), "remove in use: pending");
    Expect(in::SavePending(p, {{"remove", "hello", "", "", "", 0, true}}), "pending remove written");
    applied = in::ApplyPending(p, &db);
    Expect(applied.size() == 1 && applied[0].ok && applied[0].op.deleteData && !in::Exists(mods + L"\\hello") && !db.mods.count("hello"),
           "pending remove applied at the next start");
    Expect(in::Remove(p, "hello", &err) == in::Result::Done, "removing a missing folder is a no-op");
}

void TestFetch() {
    namespace fe = melange::store::fetch;
    const std::wstring f = Tmp(L"fetch.bin");
    WriteFile(f, std::string(5000, 'x'));
    fe::StringSink sink;
    fe::Options o;
    o.cap = 10000;
    std::string err;
    uint64_t seen = 0;
    o.progress = [&](uint64_t got, uint64_t) { seen = got; };
    Expect(fe::Get(FileUrl(f), sink, o, &err) && sink.data.size() == 5000 && seen == 5000, "file:// read with progress");
    fe::StringSink small;
    o.cap = 100;
    Expect(!fe::Get(FileUrl(f), small, o, &err) && err.find("limit") != std::string::npos, "file:// read capped");
    Expect(!fe::Get(FileUrl(Tmp(L"missing.bin")), small, o, &err), "missing file fails");
    Expect(!fe::Get("http://example.com/x", small, o, &err) && err == "url scheme not allowed", "http:// never fetched");
    std::atomic<bool> cancel{true};
    o.cap = 10000;
    o.cancel = &cancel;
    fe::StringSink c;
    Expect(!fe::Get(FileUrl(f), c, o, &err) && err == "cancelled", "cancel");
}

void TestGate() {
    namespace se = melange::session;
    se::State s;
    s.atFrontend = true;
    Expect(se::ChangeRefusal(s, se::For::Plugins).empty(), "plugins: allowed offline at the menu");
    auto refused = [&](bool se::State::*f, const char* needle, const char* what) {
        se::State t = s;
        t.*f = !(t.*f);
        const std::string why = se::ChangeRefusal(t, se::For::Plugins);
        Expect(why.find(needle) != std::string::npos, what);
    };
    refused(&se::State::inLobby, "leave the lobby to install", "plugins: a lobby refuses");
    refused(&se::State::netSession, "network", "plugins: a network session refuses");
    refused(&se::State::atFrontend, "main menu", "plugins: a match refuses");
    refused(&se::State::attract, "attract demo", "plugins: the attract demo refuses");
    refused(&se::State::loading, "loading", "plugins: a level set-up refuses");
    refused(&se::State::testBusy, "Test", "plugins: an Erg Test refuses");
    se::State lobby = s;
    lobby.inLobby = true;
    Expect(se::ChangeRefusal(lobby, se::For::Packs) == "packs cannot change in a lobby or a network game", "packs keep their message");
}
}  // namespace

int main() {
    wchar_t base[MAX_PATH];
    GetTempPathW(MAX_PATH, base);
    g_tmp = std::wstring(base) + L"melange_store_selftest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_tmp.c_str(), nullptr);
    TestIndex();
    TestUrls();
    TestChoose();
    TestPlan();
    TestZipRules();
    TestEngine();
    TestFetch();
    TestGate();
    in::DeleteTree(g_tmp);
    printf("store_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
