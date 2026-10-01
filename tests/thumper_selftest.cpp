// Offline self-test for src/mods/spice (Thumper's manifest parser and resolver). No game needed.
// Exit code 0 = all passed.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "mods/spice.h"

using melange::mods::State;
using melange::spice::Dep;
using melange::spice::Error;
using melange::spice::Manifest;
using melange::spice::Parse;
using melange::spice::Resolve;
using melange::spice::Resolved;
using melange::spice::SemverSatisfies;

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

// ---------------------------------------------------------------------------------------------
// Temp-directory fixtures for Parse(), which reads a real <dir>\spice.json.
// ---------------------------------------------------------------------------------------------
std::wstring g_root;

std::wstring MakeDir(const std::wstring& name) {
    std::wstring dir = g_root + L"\\" + name;
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void WriteFile(const std::wstring& dir, const std::string& content) {
    std::wstring path = dir + L"\\spice.json";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
}

std::wstring Fixture(const char* folder, const std::string& json) {
    std::wstring w(folder, folder + strlen(folder));
    std::wstring dir = MakeDir(w);
    if (!json.empty()) WriteFile(dir, json);
    return dir;
}

// ---------------------------------------------------------------------------------------------
// Semver
// ---------------------------------------------------------------------------------------------
void TestSemver() {
    Expect(SemverSatisfies("1.2.3", ""), "semver: empty range matches anything");
    Expect(SemverSatisfies("1.2.3", "1.2.3"), "semver: exact match");
    Expect(!SemverSatisfies("1.2.4", "1.2.3"), "semver: exact mismatch");
    Expect(SemverSatisfies("1.2.3", ">=1.0.0 <2.0.0"), "semver: range AND, inside");
    Expect(!SemverSatisfies("2.0.0", ">=1.0.0 <2.0.0"), "semver: range AND, at the upper bound");
    Expect(SemverSatisfies("1.9.9", "^1.2.0"), "semver: caret allows minor/patch bump");
    Expect(!SemverSatisfies("2.0.0", "^1.2.0"), "semver: caret blocks a major bump");
    Expect(SemverSatisfies("0.5.9", "^0.5.0"), "semver: caret on 0.x allows patch bump only across minor 0");
    Expect(!SemverSatisfies("0.6.0", "^0.5.0"), "semver: caret on 0.x blocks a minor bump");
    Expect(SemverSatisfies("1.2.9", "~1.2.0"), "semver: tilde allows patch bump");
    Expect(!SemverSatisfies("1.3.0", "~1.2.0"), "semver: tilde blocks a minor bump");
    Expect(!SemverSatisfies("1.0.0-rc.1", ">=1.0.0"), "semver: a prerelease is invisible to a plain range");
    Expect(SemverSatisfies("1.0.0-rc.1", ">=1.0.0-rc.0 <1.0.0"), "semver: a prerelease matches a range that names one");
    Expect(!SemverSatisfies("bad", "1.0.0"), "semver: an unparsable version never satisfies");
}

// ---------------------------------------------------------------------------------------------
// Parse()
// ---------------------------------------------------------------------------------------------
void TestParse() {
    {
        std::wstring dir = Fixture("m1-legacy", "");
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(ok && m.implicit && m.id == "m1-legacy" && m.version == "0.0.0" && !m.content, "parse: implicit manifest for an M1 folder");
    }
    {
        // A folder name is never validated the way a manifest's own "id" is (ValidId rejects '.'). A dot in an
        // implicit id would make "mod.<id>." ambiguous with another mod's own "mod.<id>." prefix over Oasis
        // (web/src/shell/ext/host.tsx's extAllowed), so it must come out sanitized.
        std::wstring dir = Fixture("foo.bar", "");
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(ok && m.implicit && m.id == "foo-bar", "parse: a dot in an implicit id is sanitized (got '" + m.id + "')");
    }
    {
        const char* json = R"({
            "spiceVersion": 1, "id": "hello-spice", "version": "1.0.0", "name": "Hello Spice",
            "authors": ["a", "b"], "melange": {"range": ">=0.1.0 <1.0.0"}, "kind": "client-only",
            "entry": {"client": "client/init.lua"},
            "settings": [{"key": "greeting", "type": "string", "default": "hi", "label": "Greeting"}],
            "defaultEnabled": false
        })";
        std::wstring dir = Fixture("hello-spice", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(ok, "parse: a full valid manifest");
        Expect(ok && m.id == "hello-spice" && m.version == "1.0.0" && m.authors.size() == 2, "parse: scalar and array fields");
        Expect(ok && !m.content && m.entryClient == "client/init.lua" && m.entrySim.empty(), "parse: kind and entry");
        Expect(ok && m.settings.size() == 1 && m.settings[0].key == "greeting" && m.settings[0].type == "string",
               "parse: settings");
        Expect(ok && !m.defaultEnabled, "parse: defaultEnabled honoured");
    }
    {
        const char* json = R"({
            "spiceVersion": 1, "id": "gfx-mod", "version": "1.0.0", "name": "Gfx Mod",
            "melange": {"range": ">=0.1.0"}, "kind": "client-only", "entry": {"client": "client/init.lua"},
            "graphics": {"anisotropy": 16, "trilinearFilter": true, "lodBias": -0.5}
        })";
        std::wstring dir = Fixture("gfx-mod", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(ok, "parse: a valid graphics block");
        Expect(ok && m.graphicsPresent && m.graphicsAnisotropy == 16 && m.graphicsTrilinear && m.graphicsLodBiasSet &&
                   m.graphicsLodBias == -0.5,
               "parse: graphics fields read through");
    }
    {
        const char* json = R"({"spiceVersion": 1, "id": "no-gfx", "version": "1.0.0", "name": "X",
            "melange": {"range": ">=0.1.0"}, "kind": "client-only", "entry": {}})";
        std::wstring dir = Fixture("no-gfx", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(ok && !m.graphicsPresent && m.graphicsAnisotropy == 0 && !m.graphicsTrilinear,
               "parse: no graphics block means no request (vanilla)");
    }
    {
        const char* json = R"({"spiceVersion": 1, "id": "bad-gfx", "version": "1.0.0", "name": "X",
            "melange": {"range": ">=0.1.0"}, "kind": "client-only", "entry": {}, "graphics": {"anisotropy": 17}})";
        std::wstring dir = Fixture("bad-gfx", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok && !errs.empty() && errs[0].field == "graphics.anisotropy", "parse: anisotropy above 16 is rejected");
    }
    {
        const char* json = R"({"spiceVersion": 1, "id": "bad-gfx2", "version": "1.0.0", "name": "X",
            "melange": {"range": ">=0.1.0"}, "kind": "client-only", "entry": {}, "graphics": {"lodBias": -9}})";
        std::wstring dir = Fixture("bad-gfx2", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok && !errs.empty() && errs[0].field == "graphics.lodBias", "parse: lodBias out of range is rejected");
    }
    {
        const char* json = R"({"spiceVersion": 1, "id": "wrong-id", "version": "1.0.0", "name": "X",
            "melange": {"range": ">=0.1.0"}, "kind": "client-only", "entry": {}})";
        std::wstring dir = Fixture("right-folder", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok, "parse: id/folder mismatch is rejected");
        Expect(!errs.empty() && errs[0].field == "id", "parse: id/folder mismatch names the field");
    }
    {
        const char* json = "{\"spiceVersion\": 1, \"id\": \"broken\"\n  \"version\": \"1.0.0\"}";  // missing comma
        std::wstring dir = Fixture("broken", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok && !errs.empty() && errs[0].line == 2, "parse: bad JSON reports a line number");
    }
    {
        const char* json = R"({"spiceVersion": 2, "id": "futuristic", "version": "1.0.0", "name": "X",
            "melange": {"range": ">=0.1.0"}, "kind": "client-only", "entry": {}})";
        std::wstring dir = Fixture("futuristic", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok, "parse: unknown spiceVersion is rejected");
    }
    {
        const char* json = R"({"spiceVersion": 1, "id": "client-sim", "version": "1.0.0", "name": "X",
            "melange": {"range": ">=0.1.0"}, "kind": "client-only", "entry": {"sim": "sim/x.lua"}})";
        std::wstring dir = Fixture("client-sim", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok, "parse: entry.sim on a client-only mod is rejected");
    }
    {
        std::string msgs = "\"messages\": [";
        for (int i = 0; i < 17; ++i) msgs += (i ? "," : "") + std::string("\"Mod.M") + std::to_string(i) + "\"";
        msgs += "]";
        std::string json = "{\"spiceVersion\": 1, \"id\": \"chatty\", \"version\": \"1.0.0\", \"name\": \"X\","
                            "\"melange\": {\"range\": \">=0.1.0\"}, \"kind\": \"content\", \"entry\": {}, " +
                            msgs + "}";
        std::wstring dir = Fixture("chatty", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok, "parse: more than 16 message names is rejected");
    }
    {
        const char* json = R"({"spiceVersion": 1, "id": "shouty", "version": "1.0.0", "name": "X",
            "melange": {"range": ">=0.1.0"}, "kind": "content", "entry": {}, "messages": ["notCapitalized.ok"]})";
        std::wstring dir = Fixture("shouty", json);
        Manifest m;
        std::vector<Error> errs;
        bool ok = Parse(dir, &m, &errs);
        Expect(!ok, "parse: a lowercase-first message name is rejected");
    }
}

// ---------------------------------------------------------------------------------------------
// Resolve()
// ---------------------------------------------------------------------------------------------
Manifest M(std::string id, std::string version = "1.0.0") {
    Manifest m;
    m.id = std::move(id);
    m.version = std::move(version);
    m.name = m.id;
    m.melangeRange = "";
    return m;
}

const Resolved* Find(const std::vector<Resolved>& r, const std::string& id) {
    for (const Resolved& x : r)
        if (x.id == id) return &x;
    return nullptr;
}

std::set<std::string> All(std::initializer_list<const char*> ids) { return std::set<std::string>(ids.begin(), ids.end()); }

void TestResolveBasics() {
    {  // missing required dependency
        Manifest a = M("a");
        a.dependencies.push_back({"missing-dep", ""});
        auto r = Resolve({a}, All({"a"}), "1.0.0", {});
        const Resolved* ra = Find(r, "a");
        Expect(ra && ra->state == State::Blocked && ra->reason.find("not installed") != std::string::npos,
               "resolve: missing required dependency blocks the mod");
    }
    {  // version mismatch
        Manifest a = M("a"), b = M("b", "1.4.0");
        a.dependencies.push_back({"b", ">=2.0.0 <3.0.0"});
        auto r = Resolve({a, b}, All({"a", "b"}), "1.0.0", {});
        const Resolved* ra = Find(r, "a");
        Expect(ra && ra->state == State::Blocked && ra->reason.find("have 1.4.0") != std::string::npos,
               "resolve: version mismatch names what's installed");
    }
    {  // optional absent: no error, no edge requirement
        Manifest a = M("a");
        a.optional.push_back({"nope", ""});
        auto r = Resolve({a}, All({"a"}), "1.0.0", {});
        const Resolved* ra = Find(r, "a");
        Expect(ra && ra->state == State::Enabled, "resolve: an absent optional dependency is not an error");
    }
    {  // conflict blocks both sides, even declared one-sided
        Manifest a = M("a"), b = M("b");
        a.conflicts.push_back({"b", ""});
        auto r = Resolve({a, b}, All({"a", "b"}), "1.0.0", {});
        const Resolved *ra = Find(r, "a"), *rb = Find(r, "b");
        Expect(ra && ra->state == State::Blocked && rb && rb->state == State::Blocked,
               "resolve: a one-sided conflicts declaration blocks both mods");
    }
    {  // 3-cycle: all three blocked, one shared reason
        Manifest a = M("a"), b = M("b"), c = M("c");
        a.dependencies.push_back({"b", ""});
        b.dependencies.push_back({"c", ""});
        c.dependencies.push_back({"a", ""});
        auto r = Resolve({a, b, c}, All({"a", "b", "c"}), "1.0.0", {});
        const Resolved *ra = Find(r, "a"), *rb = Find(r, "b"), *rc = Find(r, "c");
        bool allBlocked = ra && rb && rc && ra->state == State::Blocked && rb->state == State::Blocked && rc->state == State::Blocked;
        Expect(allBlocked, "resolve: a 3-cycle blocks every member");
        Expect(allBlocked && ra->reason == rb->reason && rb->reason == rc->reason && ra->reason.find("cycle") != std::string::npos,
               "resolve: the cycle gets one shared reason");
    }
    {  // diamond: no false cycle, D loads before B and C, both before A
        Manifest a = M("a"), b = M("b"), c = M("c"), d = M("d");
        a.dependencies.push_back({"b", ""});
        a.dependencies.push_back({"c", ""});
        b.dependencies.push_back({"d", ""});
        c.dependencies.push_back({"d", ""});
        auto r = Resolve({a, b, c, d}, All({"a", "b", "c", "d"}), "1.0.0", {});
        const Resolved *ra = Find(r, "a"), *rb = Find(r, "b"), *rc = Find(r, "c"), *rd = Find(r, "d");
        bool allEnabled = ra && rb && rc && rd && ra->state == State::Enabled && rb->state == State::Enabled &&
                          rc->state == State::Enabled && rd->state == State::Enabled;
        Expect(allEnabled, "resolve: a diamond is not mistaken for a cycle");
        Expect(allEnabled && rd->order < rb->order && rd->order < rc->order && rb->order < ra->order && rc->order < ra->order,
               "resolve: a diamond orders D before B/C before A");
    }
    {  // pins: an explicit pin overrides the natural (id-ascending) tie order
        Manifest a = M("aaa"), b = M("zzz");
        auto r = Resolve({a, b}, All({"aaa", "zzz"}), "1.0.0", {{"zzz", "aaa"}});
        const Resolved *ra = Find(r, "aaa"), *rb = Find(r, "zzz");
        Expect(ra && rb && rb->order < ra->order, "resolve: a pin can load a later id before an earlier one");
    }
    {  // loadAfter of an absent id: ignored, no error
        Manifest a = M("a");
        a.loadAfter.push_back("nonexistent");
        auto r = Resolve({a}, All({"a"}), "1.0.0", {});
        const Resolved* ra = Find(r, "a");
        Expect(ra && ra->state == State::Enabled, "resolve: loadAfter of an absent id is silently ignored");
    }
    {  // unsafe mod: pending consent instead of a hard block
        Manifest a = M("a");
        a.unsafe = true;
        auto r = Resolve({a}, All({"a"}), "1.0.0", {});
        const Resolved* ra = Find(r, "a");
        Expect(ra && ra->state == State::PendingConsent, "resolve: an unsafe mod is pending consent, not blocked");
    }
    {  // engine version range excludes the running build
        Manifest a = M("a");
        a.melangeRange = ">=9.0.0";
        auto r = Resolve({a}, All({"a"}), "1.0.0", {});
        const Resolved* ra = Find(r, "a");
        Expect(ra && ra->state == State::Incompatible, "resolve: a melange.range that excludes the running build is incompatible");
    }
    {  // not user-enabled: disabled, not an error
        Manifest a = M("a");
        auto r = Resolve({a}, {}, "1.0.0", {});
        const Resolved* ra = Find(r, "a");
        Expect(ra && ra->state == State::Disabled, "resolve: a mod the user hasn't enabled is Disabled");
    }
}

void TestResolveStableOrder() {
    std::vector<Manifest> base;
    base.push_back(M("a"));
    base.push_back(M("b"));
    base.push_back(M("c"));
    base.push_back(M("d"));
    base.push_back(M("e"));
    base.push_back(M("f"));
    base[0].dependencies.push_back({"c", ""});  // a needs c
    base[1].dependencies.push_back({"c", ""});  // b needs c
    base[3].loadAfter.push_back("e");           // d after e
    std::set<std::string> enabled = All({"a", "b", "c", "d", "e", "f"});

    std::vector<Resolved> first = Resolve(base, enabled, "1.0.0", {});
    std::sort(first.begin(), first.end(), [](const Resolved& x, const Resolved& y) { return x.id < y.id; });

    std::mt19937 rng(12345);
    bool stable = true;
    for (int i = 0; i < 100; ++i) {
        std::vector<Manifest> shuffled = base;
        std::shuffle(shuffled.begin(), shuffled.end(), rng);
        std::vector<Resolved> r = Resolve(shuffled, enabled, "1.0.0", {});
        std::sort(r.begin(), r.end(), [](const Resolved& x, const Resolved& y) { return x.id < y.id; });
        if (r.size() != first.size()) {
            stable = false;
            break;
        }
        for (size_t k = 0; k < r.size(); ++k)
            if (r[k].id != first[k].id || r[k].state != first[k].state || r[k].order != first[k].order) {
                stable = false;
                break;
            }
        if (!stable) break;
    }
    Expect(stable, "resolve: output order is identical across 100 shuffled input orders");
}
}  // namespace

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t unique[MAX_PATH];
    swprintf(unique, MAX_PATH, L"%sthumper_selftest_%lu", tmp, GetCurrentProcessId());
    g_root = unique;
    CreateDirectoryW(g_root.c_str(), nullptr);

    TestSemver();
    TestParse();
    TestResolveBasics();
    TestResolveStableOrder();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
