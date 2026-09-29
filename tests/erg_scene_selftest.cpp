// Offline self-test for the Erg scene model (no game, no game files): stem rules, erg-scene/1 and erg-patch/1 round
// trips and refusals over synthetic scenes, patch application, frame transforms and the spice.json "levels" array.
// The synthetic scenes are written to tests/fixtures/erg with --write and compared with the committed files otherwise.
// Exit code 0 = all passed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "erg/names.h"
#include "erg/patch.h"
#include "erg/scene.h"
#include "levels/manifest.h"
#include "mods/spice.h"

namespace erg = melange::erg;
namespace names = melange::erg::names;
namespace lm = melange::levels::manifest;

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

const std::string kSha(64, 'a'), kShaB(64, 'b'), kShaC(64, 'c');

// Deterministic LCG so the fixtures never change between runs or machines.
struct Rng {
    uint32_t s;
    uint32_t Next() { return s = s * 1664525u + 1013904223u; }
    double Unit() { return (Next() >> 8) / 16777216.0; }
    int Range(int lo, int hi) { return lo + static_cast<int>(Next() % static_cast<uint32_t>(hi - lo + 1)); }
};

double Round(double v, double step) { return std::round(v / step) * step; }

// A synthetic level: a root, folder frames ("Worms", "Objects") and terrain frames with rotations and scales.
erg::Scene Synthetic(int terrainFrames, uint32_t seed) {
    Rng r{seed};
    erg::Scene s;
    s.stem = "synthetic_s" + std::to_string(terrainFrames);
    s.title = "Synthetic " + std::to_string(terrainFrames);
    s.base = {"Multi.Synthetic", "game", "Synthetic", {kSha, kShaB, kShaC}};
    s.databank = {"BUILDING", "DAY", "ThemeBuilding\\ThemeBuilding.txt", "", ""};
    int64_t id = 1, ref = 1;
    erg::Frame root;
    root.id = id++;
    root.name = "root";
    root.size = {0, 0, 0};
    s.frames.push_back(root);
    auto folder = [&](const char* name, erg::Vec3 pos) {
        erg::Frame f;
        f.id = id++;
        f.parent = root.id;
        f.name = name;
        f.pos = pos;
        f.size = {1, 1, 1};
        f.folder = true;
        s.frames.push_back(f);
        return f.id;
    };
    const int64_t worms = folder("Worms", {0, 0, 0}), objects = folder("Objects", {2, 0, -2});
    for (int i = 0; i < terrainFrames; ++i) {
        erg::Frame f;
        f.id = id++;
        f.parent = i % 5 == 4 ? s.frames.back().id : root.id;
        f.name = "land" + std::to_string(i);
        f.pos = {Round(r.Unit() * 400 - 200, 0.25), Round(r.Unit() * 20, 0.25), Round(r.Unit() * 400 - 200, 0.25)};
        f.rot = {0, Round(r.Unit() * 6.2831853, 0.001), i % 7 == 0 ? 0.25 : 0};
        f.scale = {1, 1, 1};
        if (i % 11 == 0) f.scale = {1.5, 1.5, 1.5};
        f.size = {r.Range(2, 12), r.Range(2, 10), r.Range(2, 12)};
        const uint64_t cells = static_cast<uint64_t>(f.size[0]) * f.size[1] * f.size[2];
        f.voxels = ref;
        s.blobs.push_back({ref++, "voxels", f.id, cells * 4});
        f.heightMap = ref;
        s.blobs.push_back({ref++, "heightMap", f.id, static_cast<uint64_t>(f.size[0] + 1) * (f.size[2] + 1) * 4});
        s.frames.push_back(f);
    }
    int64_t src = id;
    for (int i = 0; i < 8; ++i) {
        erg::Detail d;
        d.id = d.src.emplace(src++);
        d.frame = worms;
        d.name = "WORM" + std::to_string(i);
        d.resource = "CheesyGrinWorm";
        d.pos = {Round(r.Unit() * 200 - 100, 0.5), 10, Round(r.Unit() * 200 - 100, 0.5)};
        d.role = erg::DeriveRole(d.name, d.resource);
        s.details.push_back(d);
    }
    const char* objs[][2] = {{"oildrum", "OilDrum"}, {"mine", "Mine"}, {"Camera1", "Camera"}, {"LIGHT PNTLGHT 1 1 1 30", "LIGHT"}};
    for (auto& o : objs) {
        erg::Detail d;
        d.id = d.src.emplace(src++);
        d.frame = objects;
        d.name = o[0];
        d.resource = o[1];
        d.pos = {Round(r.Unit() * 50, 0.5), 2, Round(r.Unit() * 50, 0.5)};
        d.role = erg::DeriveRole(d.name, d.resource);
        s.details.push_back(d);
    }
    for (int i = 0; i < terrainFrames / 4; ++i) {
        erg::Detail d;
        d.id = d.src.emplace(src++);
        d.frame = s.frames[4 + i * 3 % terrainFrames].id;
        d.name = "VISIBLE_prop" + std::to_string(i);
        d.resource = "BUILDING" + std::to_string(1 + i % 20);
        d.pos = {Round(r.Unit() * 4, 0.5), 1, Round(r.Unit() * 4, 0.5)};
        d.rot = {0, Round(r.Unit() * 3.14159, 0.01), 0};
        d.voxelPos = {1, 1, 1};
        d.role = erg::DeriveRole(d.name, d.resource);
        s.details.push_back(d);
    }
    return s;
}

// Line endings are normalised: a checkout may turn the committed LF fixtures into CRLF.
std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

void TestNames() {
    Expect(names::Prefix("my-maps") == "my_maps", "Prefix replaces '-'");
    Expect(names::ValidSlug("harbour") && names::ValidSlug("a1") && !names::ValidSlug("") && !names::ValidSlug("Harbour") &&
               !names::ValidSlug("a_b") && !names::ValidSlug(std::string(25, 'a')),
           "ValidSlug");
    std::string err;
    Expect(!names::ValidPrefix("ergtest", &err) && err.find("reserved") != std::string::npos, "ergtest prefix is reserved");
    Expect(names::ValidStem("mymaps_harbour", "mymaps", &err), "a good stem passes");
    Expect(names::ValidStem("ergtest_p1", "ergtest", &err), "a Test stem passes with the Test prefix");
    Expect(!names::ValidStem("mymaps_har.bour", "mymaps", &err) && err.find("'.'") != std::string::npos, "a dot is refused");
    Expect(!names::ValidStem("other_harbour", "mymaps", &err), "another prefix is refused");
    Expect(!names::ValidStem("mymaps_", "mymaps", &err), "an empty slug is refused");
    Expect(!names::ValidStem(std::string(40, 'a') + "_abcdefghi", std::string(40, 'a'), &err), "a stem over 48 is refused");
    Expect(names::CollidesWithVanilla("DeathMatch10") && names::CollidesWithVanilla("deathmatch10") &&
               names::CollidesWithVanilla("Multi_DinerMight"),
           "vanilla stems match case-insensitively");
    Expect(!names::CollidesWithVanilla("mymaps_harbour") && !names::CollidesWithVanilla(""), "a new stem is not vanilla");
    Expect(!names::ValidStem("multi_dinermight", "multi", &err) && err.find("vanilla") != std::string::npos,
           "a pack stem equal to a vanilla stem is refused");
    Expect(names::Key("mymaps_harbour") == "Multi.mymaps_harbour", "Key");
}

void TestSceneRoundTrip(const erg::Scene& s, const std::string& what) {
    std::string err;
    Expect(erg::ValidateScene(s, &err), what + " validates: " + err);
    const std::string a = erg::WriteScene(s);
    erg::Scene back;
    Expect(erg::ParseScene(a, &back, &err), what + " parses: " + err);
    Expect(erg::WriteScene(back) == a, what + " round-trips byte for byte");
    Expect(back.frames.size() == s.frames.size() && back.details.size() == s.details.size() && back.blobs.size() == s.blobs.size(),
           what + " keeps every frame, detail and blob");
}

std::string Mutate(const std::string& json, const std::string& from, const std::string& to) {
    std::string s = json;
    const size_t p = s.find(from);
    if (p != std::string::npos) s.replace(p, from.size(), to);
    return s;
}

void TestSceneRefusals(const erg::Scene& s) {
    const std::string good = erg::WriteScene(s);
    erg::Scene out;
    std::string err;
    struct Case { std::string json, what; };
    const Case cases[] = {
        {Mutate(good, "\"erg-scene/1\"", "\"erg-scene/2\""), "another format"},
        {Mutate(good, "\"stem\":", "\"extra\":1,\"stem\":"), "an unknown key"},
        {Mutate(good, "\"title\":\"Synthetic 12\"", "\"title\":\"" + std::string(41, 'x') + "\""), "a long title"},
        {Mutate(good, "\"worldPerXan\":20", "\"worldPerXan\":10"), "another unit"},
        {Mutate(good, "\"kind\":\"voxels\"", "\"kind\":\"mesh\""), "an unknown blob kind"},
        {Mutate(good, "\"role\":\"spawn\"", "\"role\":\"weapon\""), "an unknown role"},
        {Mutate(good, "\"mode\":\"copy\"", "\"mode\":\"paint\""), "hmp paint"},
        {Mutate(good, "\"level\":null", "\"level\":5000"), "water out of range"},
        {Mutate(good, "\"theme\":\"BUILDING\"", "\"theme\":\"MOON\""), "an unknown theme"},
        {good.substr(0, good.size() / 2), "truncated JSON"},
        {"[]", "not an object"},
    };
    for (auto& c : cases) Expect(!erg::ParseScene(c.json, &out, &err), "scene refused: " + c.what);

    erg::Scene bad = s;
    bad.blobs[0].bytes += 4;
    Expect(!erg::ValidateScene(bad, &err) && err.find("voxels blob") != std::string::npos, "a blob of the wrong size is refused");
    bad = s;
    bad.frames[3].parent = 999999;
    Expect(!erg::ValidateScene(bad, &err), "a missing parent is refused");
    bad = s;
    bad.frames[3].parent = bad.frames[4].id;
    bad.frames[4].parent = bad.frames[3].id;
    Expect(!erg::ValidateScene(bad, &err) && err.find("cycle") != std::string::npos, "a parent cycle is refused");
    bad = s;
    bad.details[1].id = bad.details[0].id;
    Expect(!erg::ValidateScene(bad, &err), "a duplicate detail id is refused");
    bad = s;
    bad.details[0].frame = 777777;
    Expect(!erg::ValidateScene(bad, &err), "a detail on a missing frame is refused");
    bad = s;
    bad.frames[0].parent = -1;
    bad.frames[1].parent = -1;
    Expect(!erg::ValidateScene(bad, &err), "two roots are refused");
}

erg::Patch SamplePatch(const erg::Scene& s) {
    erg::Patch p;
    p.stem = "mymaps_synthetic";
    p.title = "Synthetic Brawl";
    p.base = {s.base.key, s.base.source, "", s.base.sha256};
    p.databank.theme = "CAMELOT";
    p.water = 40.0;
    p.spawns = erg::SpawnMode::Knots;
    erg::Op set;
    set.kind = erg::Op::Kind::Set;
    set.src = *s.details[0].src;
    set.fields.pos = erg::Vec3{12.5, 3.0, -4.0};
    p.ops.push_back(set);
    erg::Op add;
    add.kind = erg::Op::Kind::Add;
    add.frame = s.details[8].frame;
    add.fields.name = "oildrum";
    add.fields.resource = "OilDrum";
    add.fields.pos = erg::Vec3{1, 2, 3};
    p.ops.push_back(add);
    erg::Op rem;
    rem.kind = erg::Op::Kind::Remove;
    rem.src = *s.details[9].src;
    p.ops.push_back(rem);
    return p;
}

void TestPatch(const erg::Scene& s) {
    std::string err;
    erg::Patch p = SamplePatch(s);
    const std::string text = erg::WritePatch(p);
    erg::Patch back;
    Expect(erg::ParsePatch(text, &back, &err), "the sample patch parses: " + err);
    Expect(erg::WritePatch(back) == text, "the sample patch round-trips byte for byte");
    Expect(erg::ValidatePatch(back, s, {}, &err), "the sample patch validates: " + err);

    erg::Scene applied = s;
    Expect(erg::ApplyPatch(applied, back, {}, &err), "the sample patch applies: " + err);
    const erg::Detail* moved = applied.FindDetailBySrc(*s.details[0].src);
    Expect(moved && moved->pos == erg::Vec3{12.5, 3.0, -4.0} && moved->name == "WORM0", "set moves the detail and keeps its name");
    Expect(!applied.FindDetailBySrc(*s.details[9].src), "remove deletes the detail");
    Expect(applied.details.size() == s.details.size() && !applied.details.back().src && applied.details.back().name == "oildrum" &&
               applied.details.back().role == erg::Role::Object,
           "add appends a new object with no src");
    Expect(applied.databank.theme == "CAMELOT" && applied.water && *applied.water == 40.0 && applied.spawns == erg::SpawnMode::Knots,
           "databank, water and spawns apply");
    Expect(erg::ValidateScene(applied, &err), "the patched scene validates: " + err);

    // Refusals at parse time name the op index.
    struct Case { std::string json, what, needle; };
    const std::string good = text;
    const Case cases[] = {
        {Mutate(good, "\"op\":\"remove\"", "\"op\":\"explode\""), "an unknown op", "ops[2]"},
        {Mutate(good, "\"pos\":[12.5,3,-4]", "\"pos\":[12.5,3]"), "a short vector", "ops[0]"},
        {Mutate(good, "\"pos\":[12.5,3,-4]", "\"pos\":[12.5,3,\"x\"]"), "a string coordinate", "ops[0]"},
        {Mutate(good, "\"name\":\"oildrum\"", "\"name\":\"\""), "an empty name", "ops[1]"},
        {Mutate(good, "{\"op\":\"set\",\"src\":", "{\"op\":\"set\",\"bogus\":1,\"src\":"), "an unknown op key", "ops[0]"},
        {Mutate(good, "\"stem\":\"mymaps_synthetic\"", "\"stem\":\"mymaps.synthetic\""), "a stem with a dot", "stem"},
        {Mutate(good, "\"stem\":\"mymaps_synthetic\"", "\"stem\":\"deathmatch10_x\""), "a stem without a vanilla clash", ""},
        {Mutate(good, "\"stem\":\"mymaps_synthetic\"", "\"stem\":\"multi_dinermight\""), "a vanilla stem", "vanilla"},
        {Mutate(good, "\"theme\":\"CAMELOT\"", "\"theme\":\"camelot\""), "a lower-case theme", "theme"},
        {Mutate(good, "\"level\":40", "\"level\":1e9"), "water out of range", "water"},
        {Mutate(good, kSha, "ABC"), "a short hash", "sha256"},
    };
    for (auto& c : cases) {
        erg::Patch out;
        const bool ok = erg::ParsePatch(c.json, &out, &err);
        if (c.what == "a stem without a vanilla clash") {
            Expect(ok, "patch accepted: " + c.what + ": " + err);
            continue;
        }
        Expect(!ok && err.find(c.needle) != std::string::npos, "patch refused: " + c.what + " (" + err + ")");
    }

    // Refusals against the base.
    erg::Patch bad = back;
    bad.ops[0].src = 999999;
    Expect(!erg::ValidatePatch(bad, s, {}, &err) && err.find("ops[0].src") != std::string::npos, "an out-of-range src names its op");
    bad = back;
    bad.ops.push_back(bad.ops[2]);
    Expect(!erg::ValidatePatch(bad, s, {}, &err) && err.find("ops[3]") != std::string::npos, "a second remove of one src is refused");
    bad = back;
    bad.ops[1].frame = s.frames[5].id;
    Expect(!erg::ValidatePatch(bad, s, {}, &err) && err.find("folder") != std::string::npos, "add to a terrain frame is refused");
    Expect(erg::ValidatePatch(bad, s, {false, true}, &err), "add to a terrain frame passes with anyFrame");
    bad = back;
    bad.base.sha256.xan = kShaB;
    Expect(!erg::ValidatePatch(bad, s, {}, &err) && err.find("changed") != std::string::npos, "a changed base is refused");

    // Voxels: refused by default, bounds and value bits checked.
    const erg::Frame& land = s.frames[3];
    const uint32_t cells = static_cast<uint32_t>(land.size[0] * land.size[1] * land.size[2]);
    erg::Patch vx = back;
    erg::Op v;
    v.kind = erg::Op::Kind::Voxels;
    v.frame = land.id;
    v.runs = {{0, 2, 0}, {3, 1, 3 | (7 << 2)}};
    vx.ops.push_back(v);
    Expect(!erg::ValidatePatch(vx, s, {}, &err) && err.find("ops[3]") != std::string::npos, "voxels ops are refused by default");
    Expect(erg::ValidatePatch(vx, s, {true, false}, &err), "voxels ops pass when enabled: " + err);
    vx.ops.back().runs.push_back({cells - 1, 2, 0});
    Expect(!erg::ValidatePatch(vx, s, {true, false}, &err) && err.find("runs[2]") != std::string::npos, "a run past the frame is refused");
    std::string vtext = erg::WritePatch(vx);
    Expect(!erg::ParsePatch(Mutate(vtext, "[3,1,31]", "[3,1,16777247]"), &back, &err) && err.find("bits 24-31") != std::string::npos,
           "a value with high bits is refused");
    Expect(!erg::ParsePatch(Mutate(vtext, "[3,1,31]", "[3,1,29]"), &back, &err), "a value with solid bits 1 is refused");
    std::vector<uint32_t> vox(8, 3);
    Expect(erg::ApplyRuns(vox, {{2, 3, 0}}, &err) && vox[1] == 3 && vox[2] == 0 && vox[4] == 0 && vox[5] == 3, "ApplyRuns");
    Expect(!erg::ApplyRuns(vox, {{6, 3, 0}}, &err), "ApplyRuns refuses a run past the end");

    // Limits.
    std::string many = "{\"format\":\"erg-patch/1\",\"stem\":\"a_b\",\"title\":\"t\",\"base\":{\"key\":\"k\",\"source\":\"game\","
                       "\"sha256\":{\"xan\":\"" + kSha + "\",\"xom\":\"" + kSha + "\",\"hmp\":null}},\"ops\":[";
    for (int i = 0; i < 20001; ++i) many += std::string(i ? "," : "") + "{\"op\":\"remove\",\"src\":1}";
    many += "]}";
    Expect(!erg::ParsePatch(many, &back, &err) && err.find("20000") != std::string::npos, "more than 20000 ops are refused");
    std::string runs = "{\"format\":\"erg-patch/1\",\"stem\":\"a_b\",\"title\":\"t\",\"base\":{\"key\":\"k\",\"source\":\"game\","
                       "\"sha256\":{\"xan\":\"" + kSha + "\",\"xom\":\"" + kSha + "\"}},\"ops\":[{\"op\":\"voxels\",\"frame\":3,\"runs\":[";
    for (int i = 0; i < 2001; ++i) runs += std::string(i ? "," : "") + "[" + std::to_string(i) + ",1,0]";
    runs += "]}]}";
    Expect(!erg::ParsePatch(runs, &back, &err) && err.find("2000") != std::string::npos, "more than 2000 runs per frame are refused");
    Expect(!erg::ParsePatch(std::string(erg::kMaxPatchBytes + 1, ' '), &back, &err) && err.find("4 MB") != std::string::npos,
           "a patch over 4 MB is refused");
}

void TestTransforms() {
    erg::Scene s;
    s.frames.push_back({});
    s.frames[0].id = 1;
    s.frames[0].scale = {0, 0, 0};
    erg::Frame a;
    a.id = 2;
    a.parent = 1;
    a.pos = {10, 0, 0};
    erg::Frame b;
    b.id = 3;
    b.parent = 2;
    b.pos = {0, 5, 0};
    b.scale = {2, 2, 2};
    s.frames.push_back(a);
    s.frames.push_back(b);
    erg::Detail d;
    d.frame = 3;
    d.pos = {1, 1, 1};
    erg::Vec3 w{};
    Expect(erg::DetailWorld(s, d, &w) && std::fabs(w[0] - 12) < 1e-9 && std::fabs(w[1] - 7) < 1e-9 && std::fabs(w[2] - 2) < 1e-9,
           "translation and scale compose from the frame to the root");
    s.frames[1].rot = {0, 1.5707963267948966, 0};
    Expect(erg::DetailWorld(s, d, &w), "a rotated parent composes");
    const double len = std::sqrt((w[0] - 10) * (w[0] - 10) + w[1] * w[1] + w[2] * w[2]);
    Expect(std::fabs(len - std::sqrt(4 + 49 + 4)) < 1e-9 && std::fabs(w[1] - 7) < 1e-9, "a Y rotation keeps lengths and Y");
    erg::Frame f;
    f.rot = {0, 1.5707963267948966, 0};
    erg::Vec3 px = erg::Apply(erg::FrameLocal(f), {1, 0, 0});
    Expect(std::fabs(px[0]) < 1e-9 && std::fabs(px[2] + 1) < 1e-9, "Ry(+90) maps +x to -z");
    f.rot = {1.5707963267948966, 0, 1.5707963267948966};
    px = erg::Apply(erg::FrameLocal(f), {0, 1, 0});
    Expect(std::fabs(px[0]) < 1e-9 && std::fabs(px[1]) < 1e-9 && std::fabs(px[2] - 1) < 1e-9, "Rz * Rx: X is applied first");
    d.frame = 42;
    Expect(!erg::DetailWorld(s, d, &w), "a missing frame has no world position");
}

melange::spice::Manifest Mod(const std::string& id, std::vector<melange::spice::Level> levels, bool content = true) {
    melange::spice::Manifest m;
    m.id = id;
    m.content = content;
    m.levels = std::move(levels);
    return m;
}

void TestManifest() {
    using L = melange::spice::Level;
    std::vector<lm::Error> errs;
    auto a = lm::Parse(Mod("my-maps", {L{"harbour", "Harbour Brawl", "multi", "src/harbour.ergpatch.json", true, 0}}), &errs);
    Expect(a.size() == 1 && a[0].stem == "my_maps_harbour" && a[0].chunk && errs.empty(), "a good level parses");
    Expect(lm::RequiredFiles(a[0]).size() == 3 && lm::Scripts(a[0]).back() == "my_maps_harbour", "files and scripts of a chunk level");
    errs.clear();
    Expect(lm::Parse(Mod("maps", {L{"a", "A", "multi", "", false, 0}}, false), &errs).empty() && !errs.empty(), "client-only mods are refused");
    errs.clear();
    Expect(lm::Parse(Mod("maps", {L{"a", "A", "survivor", "", false, 0}}), &errs).empty(), "type survivor is refused");
    errs.clear();
    Expect(lm::Parse(Mod("maps", {L{"a", "A", "multi", "", false, 0}, L{"a", "B", "multi", "", false, 0}}), &errs).empty(),
           "a duplicate slug is refused");
    errs.clear();
    Expect(lm::Parse(Mod("maps", {L{"a", "Caf\xc3\xa9", "multi", "", false, 0}}), &errs).empty(), "a non-ASCII title is refused");
    errs.clear();
    Expect(lm::Parse(Mod("maps", {L{"a", "A", "multi", "../x.json", false, 0}}), &errs).empty(), "a source outside the mod is refused");
    errs.clear();
    Expect(lm::Parse(Mod("ergtest", {L{"a", "A", "multi", "", false, 0}}), &errs).empty(), "the ergtest prefix is refused");
    errs.clear();
    Expect(lm::Parse(Mod("multi", {L{"dinermight", "A", "multi", "", false, 0}}), &errs).empty(), "a vanilla stem is refused");
    std::vector<L> many;
    for (int i = 0; i < 33; ++i) many.push_back(L{"l" + std::to_string(i), "L", "multi", "", false, 0});
    errs.clear();
    Expect(lm::Parse(Mod("maps", many), &errs).empty(), "33 levels in one mod are refused");

    std::vector<lm::Error> refused;
    auto b = lm::Parse(Mod("my_maps", {L{"dock", "Dock", "multi", "", false, 0}}), nullptr);
    auto c = lm::Parse(Mod("others", {L{"dock", "Dock", "multi", "", false, 0}}), nullptr);
    auto all = lm::Assign({a, b, c}, &refused);
    Expect(all.size() == 2 && refused.size() == 1 && refused[0].mod == "my_maps", "the later mod with the same prefix is refused");
    std::vector<std::vector<lm::LevelDecl>> five;
    for (int m = 0; m < 5; ++m) {
        std::vector<L> ls;
        for (int i = 0; i < 30; ++i) ls.push_back(L{"l" + std::to_string(i), "L", "multi", "", false, 0});
        five.push_back(lm::Parse(Mod("pack" + std::to_string(m), ls), nullptr));
    }
    refused.clear();
    all = lm::Assign(five, &refused);
    Expect(all.size() == 120 && refused.size() == 1 && refused[0].mod == "pack4", "more than 128 levels in all refuse the last mod");
}

void Fixtures(const std::string& dir, bool write) {
    const std::pair<const char*, erg::Scene> scenes[] = {{"synthetic-12.json", Synthetic(12, 7)},
                                                         {"synthetic-400.json", Synthetic(400, 11)}};
    for (auto& [file, s] : scenes) {
        const std::string text = erg::WriteScene(s) + "\n";
        const std::string path = dir + "/" + file;
        if (write) {
            std::ofstream(path, std::ios::binary) << text;
            printf("wrote %s (%zu bytes)\n", path.c_str(), text.size());
        } else {
            const std::string have = ReadFile(path);
            Expect(have == text, std::string("fixture ") + file + " matches the generator (run with --write)");
        }
    }
    const erg::Scene s = Synthetic(12, 7);
    const std::string patch = erg::WritePatch(SamplePatch(s)) + "\n";
    if (write) std::ofstream(dir + "/synthetic-12.ergpatch.json", std::ios::binary) << patch;
    else Expect(ReadFile(dir + "/synthetic-12.ergpatch.json") == patch, "fixture synthetic-12.ergpatch.json matches the generator");
}
}  // namespace

int main(int argc, char** argv) {
    bool write = false;
    for (int i = 1; i < argc; ++i) write |= std::strcmp(argv[i], "--write") == 0;
    TestNames();
    const erg::Scene small = Synthetic(12, 7), big = Synthetic(400, 11);
    TestSceneRoundTrip(small, "synthetic-12");
    TestSceneRoundTrip(big, "synthetic-400");
    Expect(big.frames.size() == 403, "the 400-frame scene has 403 frames");
    TestSceneRefusals(small);
    TestPatch(small);
    TestTransforms();
    TestManifest();
    Fixtures(std::string(MELANGE_SOURCE_DIR) + "/tests/fixtures/erg", write);
    printf("erg_scene_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
