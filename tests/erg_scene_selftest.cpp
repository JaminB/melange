// Offline self-test for the Erg scene model (no game, no game files): stem rules, erg-scene/1 and /2 and erg-patch/1 and
// /2 round trips and refusals over synthetic scenes, patch application, frame transforms, the generated chunk's grammar
// (every branch verifies, mutated chunks are refused) and the spice.json "levels" array.
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

#include "erg/luagen.h"
#include "erg/names.h"
#include "erg/patch.h"
#include "erg/scene.h"
#include "levels/manifest.h"
#include "mods/spice.h"

namespace erg = melange::erg;
namespace names = melange::erg::names;
namespace luagen = melange::erg::luagen;
namespace lm = melange::levels::manifest;

// A JSON array of n copies of v.
static std::string Corners(int n, double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    std::string out = "[";
    for (int i = 0; i < n; ++i) out += (i ? std::string(",") : std::string()) + buf;
    return out + "]";
}

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
        {Mutate(good, "\"erg-scene/1\"", "\"erg-scene/3\""), "another format"},
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

    // Diner Might's dance floor (frame 321, the M6.2 probe): voxels are centred on the frame's position.
    erg::Frame floor;
    floor.id = 321;
    floor.parent = 1;
    floor.pos = {-33.70, 3.45, -9.61};
    floor.rot = {0, 1.49, 0};
    floor.scale = {1.31, 0.64, 1.40};
    floor.size = {11, 1, 11};
    s.frames.push_back(floor);
    erg::Mat3x4 g;
    Expect(erg::VoxelWorld(s, 321, &g), "the dance floor has a voxel grid");
    erg::Vec3 lo{1e9, 1e9, 1e9}, hi{-1e9, -1e9, -1e9};
    for (int k = 0; k < 8; ++k) {
        const erg::Vec3 c = erg::Apply(g, {k & 1 ? 11.0 : 0.0, k & 2 ? 1.0 : 0.0, k & 4 ? 11.0 : 0.0});
        for (int ax = 0; ax < 3; ++ax) {
            lo[ax] = std::min(lo[ax], c[ax]);
            hi[ax] = std::max(hi[ax], c[ax]);
        }
    }
    auto near = [](double a, double b) { return std::fabs(a - b) < 0.02; };
    Expect(near(lo[0], -41.95) && near(hi[0], -25.45) && near(lo[2], -17.43) && near(hi[2], -1.79) && near(hi[1], 3.77),
           "the dance floor's box is its position +- size/2, as the engine's BVH leaf");
    const erg::Vec3 mid = erg::Apply(g, {5.5, 0.5, 5.5});
    d.frame = 321;
    d.pos = {0, 0, 0};
    Expect(near(mid[0], -33.70) && near(mid[1], 3.45) && near(mid[2], -9.61) && erg::DetailWorld(s, d, &w) && near(w[0], mid[0]) &&
               near(w[2], mid[2]),
           "the grid's middle is the frame position, where a detail at 0 sits");
    Expect(!erg::VoxelWorld(s, 42, &g), "a missing frame has no voxel grid");
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
    for (int m = 0; m < 9; ++m) {
        std::vector<L> ls;
        for (int i = 0; i < (m == 8 ? 1 : 32); ++i) ls.push_back(L{"l" + std::to_string(i), "L", "multi", "", false, 0});
        five.push_back(lm::Parse(Mod("pack" + std::to_string(m), ls), nullptr));
    }
    refused.clear();
    all = lm::Assign(five, &refused);
    Expect(all.size() == 256 && refused.size() == 1 && refused[0].mod == "pack8", "256 levels are accepted, the 257th refuses the last mod");
}

// ---------------------------------------------------------------- erg-scene/2, erg-patch/2 and the v2 chunk
const std::string kShaD(64, 'd');

// The v1 synthetic level plus a "Scene" group frame, the only place new frames may go.
erg::Scene BaseV2() {
    erg::Scene s = Synthetic(12, 7);
    erg::Frame scene;
    scene.id = 1000;
    scene.parent = s.frames[0].id;
    scene.name = "Scene";
    scene.size = {0, 0, 0};
    s.frames.push_back(scene);
    return s;
}

int64_t ObjectsFolder(const erg::Scene& s) { return s.details[8].frame; }

erg::ObjectSpec Crate(const std::string& knot, erg::CrateKind kind, const std::string& contents, int n) {
    erg::ObjectSpec o;
    o.knot = knot;
    o.type = erg::ObjectType::Crate;
    o.crate.kind = kind;
    if (kind == erg::CrateKind::Health) o.crate.amount = n;
    else o.crate.contents = contents, o.crate.count = n;
    return o;
}

erg::ObjectSpec Telepad(const std::string& knot, int group) {
    erg::ObjectSpec o;
    o.knot = knot;
    o.type = erg::ObjectType::Telepad;
    o.group = group;
    return o;
}

erg::ObjectSpec Trigger(const std::string& knot, int index, double radius) {
    erg::ObjectSpec o;
    o.knot = knot;
    o.type = erg::ObjectType::Trigger;
    o.trigger.index = index;
    o.trigger.radius = radius;
    return o;
}

std::vector<erg::ObjectSpec> SampleObjects() {
    return {Crate("CRATE_0", erg::CrateKind::Weapon, "kWeaponHolyHandGrenade", 1), Crate("CRATE_1", erg::CrateKind::Health, "", 50),
            Crate("CRATE_2", erg::CrateKind::Utility, "kUtilityJetpack", 2), Telepad("TP_1_0", 1), Telepad("TP_1_1", 1),
            Trigger("TRIG_0", 1, 60.0)};
}

erg::Patch SamplePatchV2(const erg::Scene& base) {
    erg::Patch p = SamplePatch(base);
    p.stem = "mymaps_objects";
    p.objects = SampleObjects();
    p.survivor = true;
    p.script = {true, kShaD};
    p.hmp = erg::HmpMode::Paint;
    int k = 0;
    for (auto& o : p.objects) {
        erg::Op add;
        add.kind = erg::Op::Kind::Add;
        add.frame = ObjectsFolder(base);
        add.fields.name = o.knot;
        add.fields.resource = "Unit";
        add.fields.pos = erg::Vec3{static_cast<double>(k++), 1, 2};
        p.ops.push_back(add);
    }
    erg::Op frame;
    frame.kind = erg::Op::Kind::AddFrame;
    frame.newFrame = {-1, 1000, "ergframe_0", {4, 2, -3}, {6, 31, 6}};
    p.ops.push_back(frame);
    erg::Op vox;
    vox.kind = erg::Op::Kind::Voxels;
    vox.frame = -1;
    vox.runs = {{0, 64, 3 | (5 << 2)}};
    p.ops.push_back(vox);
    erg::Op hmp;
    hmp.kind = erg::Op::Kind::Hmp;
    hmp.heights = {{0, 100, 1.0}, {9900, 100, 0.25}};
    hmp.blend = {{0, 10000, 255}};
    p.ops.push_back(hmp);
    return p;
}

erg::PatchRules AllRules() {
    erg::PatchRules r;
    r.voxels = r.objects = r.survivor = r.script = r.newFrames = r.hmpPaint = r.blend = true;
    return r;
}

// The scene the server would send for SamplePatchV2 applied to BaseV2 (with the blobs of the new frame and the .hmp).
erg::Scene SceneV2() {
    const erg::Scene base = BaseV2();
    erg::Scene s = base;
    std::string err;
    erg::ApplyPatch(s, SamplePatchV2(base), AllRules(), &err);
    int64_t ref = 0;
    for (auto& b : s.blobs) ref = std::max(ref, b.ref + 1);
    for (auto& f : s.frames)
        if (f.isNew) {
            f.voxels = ref;
            s.blobs.push_back({ref++, "voxels", f.id, static_cast<uint64_t>(f.size[0]) * f.size[1] * f.size[2] * 4});
        }
    s.hmpRef = ref;
    s.blobs.push_back({ref, "hmp", 0, erg::kHmpBytes});
    return s;
}

void TestV2Model() {
    std::string err;
    // A v1 document reads as a scene without /2 features and is written back as the same /1 bytes.
    const erg::Scene v1 = Synthetic(12, 7);
    const std::string v1Text = erg::WriteScene(v1);
    erg::Scene up;
    Expect(erg::ParseScene(v1Text, &up, &err) && !up.UsesV2() && erg::WriteScene(up) == v1Text && v1Text.find("erg-scene/1") != std::string::npos,
           "a v1 scene upgrades in memory and is written back byte for byte: " + err);
    const std::string v1Patch = erg::WritePatch(SamplePatch(v1));
    erg::Patch upP;
    Expect(erg::ParsePatch(v1Patch, &upP, &err) && !upP.UsesV2() && erg::WritePatch(upP) == v1Patch,
           "a v1 patch is written back byte for byte: " + err);

    const erg::Scene s2 = SceneV2();
    Expect(erg::ValidateScene(s2, &err), "the v2 scene validates: " + err);
    const std::string text = erg::WriteScene(s2);
    erg::Scene back;
    Expect(text.find("\"format\":\"erg-scene/2\"") != std::string::npos && erg::ParseScene(text, &back, &err) &&
               erg::WriteScene(back) == text,
           "the v2 scene round-trips byte for byte: " + err);
    Expect(back.objects.size() == 6 && back.survivor && back.script.present && back.hmp == erg::HmpMode::Paint &&
               back.FindFrame(-1) && back.FindFrame(-1)->isNew,
           "the v2 scene keeps objects, kind, script, the painted surround and the new frame");

    const erg::Scene base = BaseV2();
    const erg::Patch p2 = SamplePatchV2(base);
    const std::string ptext = erg::WritePatch(p2);
    erg::Patch pback;
    Expect(ptext.find("\"format\":\"erg-patch/2\"") != std::string::npos && erg::ParsePatch(ptext, &pback, &err) &&
               erg::WritePatch(pback) == ptext,
           "the v2 patch round-trips byte for byte: " + err);
    Expect(erg::ValidatePatch(pback, base, AllRules(), &err), "the v2 patch validates with every rule on: " + err);
    const std::pair<void (*)(erg::PatchRules&), const char*> off[] = {
        {[](erg::PatchRules& r) { r.objects = false; }, "objects"},
        {[](erg::PatchRules& r) { r.survivor = false; }, "kind.survivor"},
        {[](erg::PatchRules& r) { r.script = false; }, "script"},
        {[](erg::PatchRules& r) { r.hmpPaint = false; }, "hmp"},
        {[](erg::PatchRules& r) { r.newFrames = false; }, "new frames"},
    };
    for (auto& [fn, what] : off) {
        erg::PatchRules r = AllRules();
        fn(r);
        Expect(!erg::ValidatePatch(pback, base, r, &err) && err.find(what) != std::string::npos,
               std::string("a v2 feature is refused while its rule is off: ") + what + " (" + err + ")");
    }
    erg::Scene applied = base;
    Expect(erg::ApplyPatch(applied, pback, AllRules(), &err) && applied.objects.size() == 6 && applied.FindFrame(-1) &&
               applied.FindFrame(-1)->parent == 1000,
           "the v2 patch applies: " + err);

    struct Case { std::string json, what, needle; };
    const Case sceneCases[] = {
        {Mutate(v1Text, "\"blobs\":[", "\"objects\":[],\"blobs\":["), "a v1 scene with v2 keys", "unknown key"},
        {Mutate(v1Text, "erg-scene/1", "erg-scene/3"), "a newer scene format", "newer"},
        {Mutate(text, "\"knot\":\"CRATE_0\"", "\"knot\":\"CRATE_00\""), "a knot with a leading zero", "knot"},
        {Mutate(text, "\"knot\":\"TP_1_0\"", "\"knot\":\"TP_2_0\""), "a telepad knot of another group", "knot"},
        {Mutate(text, "\"kind\":\"weapon\"", "\"kind\":\"target\""), "a target crate", "kind"},
        {Mutate(text, "\"amount\":50", "\"amount\":0"), "a health crate of 0", "amount"},
        {Mutate(text, "\"count\":2", "\"count\":100"), "100 utilities in a crate", "count"},
        {Mutate(text, "\"contents\":\"kWeaponHolyHandGrenade\"", "\"contents\":\"k\\\")os.exit()--\""), "contents with a quote", "contents"},
        {Mutate(text, "\"radius\":60", "\"radius\":0.5"), "a trigger radius below 1", "radius"},
        {Mutate(text, "\"name\":\"TRIG_0\"", "\"name\":\"telepad\""), "an added detail named telepad", "telepad"},
        {Mutate(text, "\"name\":\"CRATE_1\"", "\"name\":\"CRATE_9\""), "a knot without its detail", "CRATE_1"},
        {Mutate(text, "\"parent\":1000", "\"parent\":1"), "a new frame under the root", "Scene"},
        {Mutate(text, "\"size\":[6,31,6]", "\"size\":[33,31,6]"), "a new frame of 33 voxels", "size"},
        {Mutate(text, "\"scale\":[1,1,1],\"size\":[6,31,6]", "\"scale\":[2,2,1],\"size\":[6,31,6]"), "a new frame scaled unevenly", "scale"},
        {Mutate(text, "\"scale\":[1,1,1],\"size\":[6,31,6]", "\"scale\":[5,5,5],\"size\":[6,31,6]"), "a new frame scaled past 4", "scale"},
    };
    for (auto& c : sceneCases) {
        erg::Scene out;
        Expect(c.json != text && c.json != v1Text && !erg::ParseScene(c.json, &out, &err) && err.find(c.needle) != std::string::npos,
               "scene refused: " + c.what + " (" + err + ")");
    }
    const Case patchCases[] = {
        {Mutate(v1Patch, "\"ops\":[", "\"objects\":[],\"ops\":["), "a v1 patch with v2 keys", "unknown key"},
        {Mutate(v1Patch, "erg-patch/1", "erg-patch/2x"), "a newer patch format", "newer"},
        {Mutate(ptext, "\"mode\":\"paint\"", "\"mode\":\"copy\""), "hmp ops without paint", "paint"},
        {Mutate(ptext, "{\"op\":\"voxels\",\"frame\":-1", "{\"op\":\"voxels\",\"frame\":-2"), "voxels on an unknown new frame", "ops["},
        {Mutate(ptext, "\"tmp\":-1", "\"tmp\":-65"), "a tmp id past 64", "tmp"},
        {Mutate(ptext, "\"blend\":[[0,10000,255]]", "\"blend\":[[0,10000,256]]"), "a blend value of 256", "blend"},
        {Mutate(ptext, "[9900,100,0.25]", "[9900,101,0.25]"), "a run past the 10000 cells", "10000"},
        {Mutate(ptext, "[9900,100,0.25]", "[9900,100,1.5]"), "a height above 1", "heights"},
        {Mutate(ptext, "\"name\":\"ergframe_0\"", "\"name\":\"a b\""), "a frame name with a space", "name"},
        {Mutate(ptext, "\"name\":\"ergframe_0\"", "\"name\":\"rock_slippy\""), "a frame name with an engine tag", "SLIPPY"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,0,6]"), "a new frame side of 0", "size"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31,33]"), "a new frame side of 33", "size"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31]"), "a new frame size of two sides", "size"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31,6],\"scale\":0.49"), "a new frame scale under 0.5", "scale"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31,6],\"corners\":[0,1]"), "corners of another count", "corners"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31,6],\"corners\":" + Corners(49, 64.5)), "a corner offset past 64", "corners"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31,6],\"scale\":4.01"), "a new frame scale over 4", "scale"},
        {Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31,6],\"scale\":[2,2,2]"), "a new frame scale as a vector", "scale"},
        {Mutate(ptext, "[0,64,23]", "[0,64,16777239]"), "a voxel value with bits 24-31 set", "bits 24-31"},
    };
    for (auto& c : patchCases) {
        erg::Patch out;
        Expect(c.json != ptext && c.json != v1Patch && !erg::ParsePatch(c.json, &out, &err) && err.find(c.needle) != std::string::npos,
               "patch refused: " + c.what + " (" + err + ")");
    }

    // Kept readable for projects made before the engine's 31-voxel limit was known, and an optional uniform scale.
    for (const auto& [from, to, what] : {std::tuple{"\"size\":[6,31,6]", "\"size\":[32,31,6]", "a legacy side of 32"},
                                         std::tuple{"\"size\":[6,31,6]", "\"size\":[6,31,6],\"scale\":2.5", "a scale of 2.5"},
                                         std::tuple{"\"size\":[6,31,6]", "\"size\":[6,31,6],\"scale\":0.5", "a scale of 0.5"}}) {
        erg::Patch ok;
        erg::Scene app = base;
        Expect(erg::ParsePatch(Mutate(ptext, from, to), &ok, &err) && erg::ApplyPatch(app, ok, AllRules(), &err),
               std::string("patch accepted: ") + what + " " + err);
    }
    // Corner offsets on a new frame: parsed, applied to the frame, written back and carried by the scene.
    {
        erg::Patch cp;
        erg::Scene app = base;
        const std::string ctext = Mutate(ptext, "\"size\":[6,31,6]", "\"size\":[6,31,6],\"corners\":" + Corners(49, -2.5));
        Expect(erg::ParsePatch(ctext, &cp, &err) && erg::WritePatch(cp) == ctext && erg::ApplyPatch(app, cp, AllRules(), &err) &&
                   app.FindFrame(-1)->corners.size() == 49 && app.FindFrame(-1)->corners[48] == -2.5,
               "patch accepted: corners on a new frame " + err);
        erg::Scene back, again;
        Expect(erg::ParseScene(Mutate(text, "\"parent\":1000", "\"parent\":1000,\"corners\":" + Corners(49, -2.5)), &back, &err) &&
                   back.FindFrame(-1)->corners.size() == 49 && erg::ParseScene(erg::WriteScene(back), &again, &err) &&
                   again.FindFrame(-1)->corners == back.FindFrame(-1)->corners,
               "the scene carries a new frame's corners: " + err);
        erg::Scene onBase;
        Expect(!erg::ParseScene(Mutate(text, "\"parent\":1000", "\"parent\":1000,\"corners\":[0]"), &onBase, &err) && err.find("corners") != std::string::npos,
               "corners of the wrong count on a scene frame are refused (" + err + ")");
    }
    erg::Scene scaled;
    Expect(erg::ParseScene(Mutate(text, "\"scale\":[1,1,1],\"size\":[6,31,6]", "\"scale\":[2.5,2.5,2.5],\"size\":[6,31,6]"), &scaled, &err) &&
               scaled.FindFrame(-1)->scale[1] == 2.5,
           "scene accepted: a new frame of scale 2.5 " + err);

    erg::Patch bad = pback;
    for (auto& op : bad.ops)
        if (op.kind == erg::Op::Kind::AddFrame) op.newFrame.parent = 2;
    Expect(!erg::ValidatePatch(bad, base, AllRules(), &err) && err.find("ops[") != std::string::npos && err.find("Scene") != std::string::npos,
           "a new frame outside the Scene frame names its op (" + err + ")");
    bad = pback;
    bad.objects.push_back(bad.objects[0]);
    Expect(!erg::ValidatePatch(bad, base, AllRules(), &err) && err.find("another object") != std::string::npos, "two objects on one knot");
    bad = pback;
    bad.objects[0].knot = "WORM0";
    Expect(!erg::ValidatePatch(bad, base, AllRules(), &err), "an object on a base detail's name is refused");
    bad = pback;
    bad.ops[0].fields.name = "telepad";
    Expect(!erg::ValidatePatch(bad, base, AllRules(), &err) && err.find("telepad") != std::string::npos, "a set naming a detail telepad");
    bad = pback;
    bad.ops.erase(std::remove_if(bad.ops.begin(), bad.ops.end(),
                                 [](const erg::Op& o) { return o.kind == erg::Op::Kind::Add && o.fields.name == std::string("TRIG_0"); }),
                  bad.ops.end());
    Expect(!erg::ValidatePatch(bad, base, AllRules(), &err) && err.find("TRIG_0") != std::string::npos, "deleting a knot's detail orphans its object");
    erg::Scene many = s2;
    many.objects.clear();
    for (int i = 0; i < 257; ++i) many.objects.push_back(Crate("CRATE_" + std::to_string(i % 256), erg::CrateKind::Health, "", 1));
    Expect(!erg::ValidateObjects(many, &err) && err.find("256") != std::string::npos, "257 objects are refused");
}

// ---------------------------------------------------------------- the v2 chunk grammar
std::vector<luagen::ChunkSpec> ChunkBranches() {
    std::vector<luagen::ChunkSpec> out;
    for (int k = 0; k < 2; ++k)
        for (int o = 0; o < 2; ++o)
            for (int w = 0; w < 2; ++w) {
                if (!k && !o && !w) continue;
                luagen::ChunkSpec c;
                c.knots = k != 0;
                c.placeObjects = o != 0;
                if (w) c.water = -12.5;
                out.push_back(c);
            }
    const auto objs = SampleObjects();
    for (auto& ob : objs) {
        luagen::ChunkSpec c;
        c.objects = {ob};
        out.push_back(c);
    }
    luagen::ChunkSpec all;
    all.knots = all.placeObjects = true;
    all.water = 40;
    all.objects = objs;
    all.objects[1].crate.parachute = true;
    all.objects[5].trigger.wormCollect = true;
    all.objects[5].trigger.teamCollect = 2;
    erg::ObjectSpec factory;
    factory.type = erg::ObjectType::MineFactory;
    factory.knot = "minefactory";
    luagen::ChunkSpec lone;
    lone.objects = {factory};
    out.push_back(lone);
    luagen::ChunkSpec placed = lone;
    placed.placeObjects = true;
    placed.knots = true;
    out.push_back(placed);
    luagen::ChunkSpec tuned = placed;
    tuned.objects[0].factory.activation = 40;
    tuned.objects[0].factory.inactiveTurns = 2;
    tuned.objects[0].factory.schemeMines = false;
    out.push_back(tuned);
    luagen::ChunkSpec noMines = lone;
    noMines.objects[0].factory.schemeMines = false;
    out.push_back(noMines);
    luagen::ChunkSpec teams;   // spawns.mode teams: each worm on its team's knot
    teams.teams = true;
    out.push_back(teams);
    luagen::ChunkSpec teamsAll = noMines;
    teamsAll.teams = true;
    teamsAll.placeObjects = true;
    teamsAll.water = 30;
    out.push_back(teamsAll);
    luagen::ChunkSpec areas = noMines;   // spawns.mode areas: each team on the area the match deals it
    areas.areas = erg::SpawnAreas{};
    areas.areas->areas = {{"", "sand", 4}, {"", "sand", 3}, {"", "keep", 2}};
    areas.areas->pattern = {{"sand", false}, {"sand", true}, {"keep", false}};
    areas.areas->opposite = {{0, 1}};
    areas.placeObjects = true;
    areas.water = 30;
    out.push_back(areas);
    all.objects.insert(all.objects.begin() + 2, factory);
    out.push_back(all);
    luagen::ChunkSpec max;
    for (int i = 0; i < 256; ++i) {
        if (i % 3 == 0) max.objects.push_back(Crate("CRATE_" + std::to_string(i), erg::CrateKind::Weapon, "kWeaponBazooka", 1 + i % 99));
        else if (i % 3 == 1) max.objects.push_back(Telepad("TP_" + std::to_string(1 + i % 8) + "_" + std::to_string(i), 1 + i % 8));
        else max.objects.push_back(Trigger("TRIG_" + std::to_string(i), i, 1 + i));
    }
    out.push_back(max);
    return out;
}

// Positions whose bytes are author values (numbers and crate contents): changing them gives another valid chunk.
std::vector<bool> ValueBytes(const std::string& t) {
    std::vector<bool> v(t.size(), false);
    for (size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-') v[i] = true;
    }
    for (const char* key : {"\"kWeapon", "\"kUtility"}) {
        size_t p = 0;
        while ((p = t.find(key, p)) != std::string::npos) {
            const size_t e = t.find('"', p + 1);
            for (size_t i = p + 1; i < e; ++i) v[i] = true;
            p = e;
        }
    }
    return v;
}

void TestChunkGrammar() {
    std::string why;
    const std::string stem = "mymaps_objects";
    int branch = 0;
    for (const auto& c : ChunkBranches()) {
        const std::string t = luagen::Text(stem, c);
        luagen::ChunkSpec back;
        Expect(!t.empty() && luagen::IsGenerated(stem, t, &why) && luagen::Parse(stem, t, &back) && luagen::Text(stem, back) == t,
               "chunk branch " + std::to_string(branch) + " is generated and verifies: " + why);
        ++branch;
    }
    for (int k = 0; k < 2; ++k)
        for (int o = 0; o < 2; ++o)
            for (int w = 0; w < 2; ++w) {
                const std::string t = luagen::Text(stem, k != 0, o != 0, w ? std::optional<double>(25.0) : std::nullopt);
                if (t.empty()) continue;
                Expect(luagen::IsGenerated(stem, t, &why) && t.find("ergCrate") == std::string::npos, "the v1 settings verify");
            }
    branch = 0;
    for (const auto& c : ChunkBranches()) {
        const std::string t = luagen::Text(stem, c), old = luagen::LegacyText(stem, c);
        std::string run;
        const bool needsWrap = c.knots || c.teams || c.areas || c.placeObjects || !c.objects.empty();
        // Nothing at top level touches a library function: Survivor's strict check runs before Initialise.
        const size_t init = t.find("function Initialise()\n");
        const std::string top = t.substr(0, init);
        Expect(!needsWrap || (init != std::string::npos && top.find("lib_") == std::string::npos &&
                              t.ends_with("    ergInit()\n    lib_SetupMultiplayerWormsAndTeams = ergSetup\nend\n")),
               "chunk branch " + std::to_string(branch) + " wraps the setup only inside Initialise");
        Expect(needsWrap ? old != t : old == t, "chunk branch " + std::to_string(branch) + " differs from the legacy form only by the wrap");
        Expect(luagen::Upgrade(stem, old, &run, &why) && run == t, "legacy chunk branch " + std::to_string(branch) + " is upgraded: " + why);
        Expect(!needsWrap || !luagen::IsGenerated(stem, old, &why), "legacy chunk branch " + std::to_string(branch) + " is not the current form");
        ++branch;
    }
    {
        std::string run;
        Expect(luagen::Upgrade(stem, luagen::Stub(stem), &run, &why) && run == luagen::Stub(stem), "the stub is run as is");
        const std::string t = luagen::Text(stem, ChunkBranches().back());
        std::string flat = t;
        for (size_t p; (p = flat.find("\n        ")) != std::string::npos;) flat.replace(p, 9, "\n    ");
        Expect(!luagen::Upgrade(stem, flat, &run, &why), "a deferred chunk with legacy indentation is refused");
        std::string hybrid = luagen::LegacyText(stem, ChunkBranches().back());
        hybrid += "local ergInit = Initialise\n";
        Expect(!luagen::Upgrade(stem, hybrid, &run, &why), "a legacy chunk with extra lines is refused");
    }
    Expect(!luagen::IsGenerated("other_stem", luagen::Text(stem, ChunkBranches().back()), &why), "a chunk of another stem is refused");
    {
        luagen::ChunkSpec t;
        t.teams = true;
        const std::string text = luagen::Text(stem, t);
        Expect(text.find("for i = 0, 15 do\n") != std::string::npos &&
                   text.find("if p < 4 then worm.Spawn = \"WORM\" .. (worm.TeamIndex * 4 + p) end\n") != std::string::npos &&
                   text.find("worm.Spawn = \"WORM\" .. i\n") == std::string::npos,
               "team spawns put each worm on WORM<4 x team + place>, not WORMi");
        t.knots = true;
        Expect(luagen::Text(stem, t).empty(), "knot and team spawns together are refused");
        erg::SpawnMode m = erg::SpawnMode::Random;
        Expect(erg::ParseSpawnMode("teams", &m) && m == erg::SpawnMode::Teams && std::string(erg::SpawnModeName(m)) == "teams",
               "spawns.mode teams parses and names itself");
    }
    luagen::ChunkSpec mf;
    erg::ObjectSpec f;
    f.knot = "minefactory";
    f.type = erg::ObjectType::MineFactory;
    mf.objects = {f};
    const std::string factory = luagen::Text(stem, mf);
    const std::string edit =
        "        local lock, scheme = EditContainer(\"GM.SchemeData\")\n"
        "        scheme.MineFactoryOn = false\n"
        "        CloseContainer(lock)\n";
    const std::string place = "        SendMessage(\"GameLogic.PlaceObjects\")\n";
    Expect(factory.find(edit + place) != std::string::npos && factory.find("~= true") == std::string::npos,
           "a level factory turns the scheme's off, then places the level's objects");
    Expect(factory.find("\"minefactory\"") == std::string::npos && factory.find("telepad") == std::string::npos,
           "the factory chunk names no knot");
    mf.placeObjects = true;
    Expect(luagen::Text(stem, mf) == factory, "with mines or drums too, PlaceObjects is sent once");
    mf.objects.push_back(f);
    Expect(luagen::Text(stem, mf).empty(), "two mine factories are refused");
    for (const char* k : {"CRATE_0", "TP_8_255", "TRIG_12", "minefactory"})
        Expect(erg::DeriveRole(k, erg::kKnotResource) == erg::Role::Object, std::string("an object knot is an object: ") + k);
    for (const char* k : {"WORM0", "TP_9_0", "CRATE_007", "TRIG_256", "MINEFACTORY_1"})
        Expect(erg::DeriveRole(k, erg::kKnotResource) == erg::Role::Spawn, std::string("not an object knot: ") + k);
    {
        std::string twice = factory, flipped = factory, without = factory;
        twice.insert(twice.find(place), place);
        Expect(!luagen::IsGenerated(stem, twice, &why), "a doubled PlaceObjects line is refused");
        flipped.replace(flipped.find("MineFactoryOn = false"), 21, "MineFactoryOn = true ");
        Expect(!luagen::IsGenerated(stem, flipped, &why), "a changed scheme edit is refused");
        without.erase(without.find(edit), edit.size());
        Expect(luagen::IsGenerated(stem, without, &why), "without the scheme edit it is the mines-and-drums chunk");
        luagen::ChunkSpec back;
        Expect(luagen::Parse(stem, without, &back) && back.objects.empty() && back.placeObjects, "and parses without a factory");
    }

    for (const bool legacy : {false, true}) {
        const luagen::ChunkSpec spec = ChunkBranches()[ChunkBranches().size() - 2];
        const std::string sample = legacy ? luagen::LegacyText(stem, spec) : luagen::Text(stem, spec);
        const std::vector<bool> values = ValueBytes(sample);
        const char* inserts[] = {"\nos.exit()\n", "\nSendMessage(\"GameLogic.PauseGame\")\n", " ", "\t", "--", "\"", "\\", ")", "(",
                                 "\nend\n", ";", "x", "\x1b", "\n    lib_CreateTelepad(\"TP_1_0\", 1) os.exit()\n",
                                 "\nlib_SetupMultiplayerWormsAndTeams = ergSetup\n", "\n    ergInit()\n", "    "};
        Rng r{legacy ? 2025u : 2024u};
        int refused = 0, tried = 0;
        while (tried < 1000) {
            std::string m = sample;
            const size_t pos = r.Next() % m.size();
            const int kind = r.Range(0, 2);
            if (kind == 0) {
                if (values[pos]) continue;
                char c = static_cast<char>(r.Range(32, 126));
                if (c == m[pos]) continue;
                m[pos] = c;
            } else if (kind == 1) {
                if (values[pos]) continue;
                m.erase(pos, 1);
            } else {
                if (values[pos] || (pos > 0 && values[pos - 1])) continue;   // text next to a value may extend it validly
                m.insert(pos, inserts[r.Next() % (sizeof inserts / sizeof inserts[0])]);
            }
            ++tried;
            std::string run;
            if (!luagen::Upgrade(stem, m, &run, &why)) ++refused;
            else printf("  accepted mutation at %zu: %.60s\n", pos, m.substr(pos > 20 ? pos - 20 : 0, 60).c_str());
        }
        Expect(refused == 1000, std::string("1000 mutated ") + (legacy ? "legacy" : "deferred") + " chunks are refused (" +
                                    std::to_string(refused) + ")");
    }
}

// The mine factory's settings: written only when set, through the patch and the scene, and into the chunk.
void TestFactorySettings() {
    std::string err, why;
    const erg::Scene base = BaseV2();
    erg::Patch p = SamplePatchV2(base);
    erg::ObjectSpec f;
    f.type = erg::ObjectType::MineFactory;
    f.knot = "minefactory";
    p.objects.push_back(f);
    erg::Op add;
    add.kind = erg::Op::Kind::Add;
    add.frame = ObjectsFolder(base);
    add.fields.name = "minefactory";
    add.fields.resource = erg::kKnotResource;
    add.fields.pos = erg::Vec3{9, 1, 2};
    p.ops.insert(p.ops.begin(), add);
    const std::string plain = erg::WritePatch(p);
    Expect(plain.find("\"type\":\"minefactory\"}") != std::string::npos && plain.find("\"factory\"") == std::string::npos,
           "a factory without settings is written as before");
    p.objects.back().factory.activation = 30;
    p.objects.back().factory.schemeMines = false;
    const std::string text = erg::WritePatch(p);
    erg::Patch back;
    Expect(text.find("\"factory\":{\"activation\":30,\"schemeMines\":false}") != std::string::npos &&
               erg::ParsePatch(text, &back, &err) && erg::WritePatch(back) == text,
           "factory settings round-trip in the patch: " + err);
    const erg::FactorySpec& bf = back.objects.back().factory;
    Expect(bf.activation == 30 && !bf.inactiveTurns && !bf.schemeMines, "the settings parse back as set");
    erg::Scene applied = base;
    Expect(erg::ApplyPatch(applied, back, AllRules(), &err), "the patch with a tuned factory applies: " + err);
    int64_t ref = 0;   // the blobs a served scene carries (as SceneV2 adds them)
    for (auto& b : applied.blobs) ref = std::max(ref, b.ref + 1);
    for (auto& fr : applied.frames)
        if (fr.isNew) {
            fr.voxels = ref;
            applied.blobs.push_back({ref++, "voxels", fr.id, static_cast<uint64_t>(fr.size[0]) * fr.size[1] * fr.size[2] * 4});
        }
    applied.hmpRef = ref;
    applied.blobs.push_back({ref, "hmp", 0, erg::kHmpBytes});
    const std::string scene = erg::WriteScene(applied);
    erg::Scene sback;
    const bool parsed0 = erg::ParseScene(scene, &sback, &err);
    Expect(parsed0 && erg::WriteScene(sback) == scene && !sback.objects.empty() && sback.objects.back().factory.activation == 30,
           "and round-trips in the scene: " + err + (parsed0 ? "" : " (parse)") +
               (scene.find("\"activation\":30") == std::string::npos ? " (not written)" : ""));
    struct Case { std::string from, to, needle; };
    for (const Case& c : {Case{"\"activation\":30", "\"activation\":256", "activation"},
                          Case{"\"activation\":30", "\"activation\":-1", "activation"},
                          Case{"\"activation\":30", "\"activation\":2.5", "activation"},
                          Case{"\"schemeMines\":false", "\"schemeMines\":0", "schemeMines"},
                          Case{"\"schemeMines\":false", "\"speed\":3", "unknown key"},
                          Case{"\"factory\":{\"activation\":30,\"schemeMines\":false}", "\"factory\":7", "factory"}}) {
        erg::Patch out;
        const std::string bad = Mutate(text, c.from, c.to);
        Expect(bad != text && !erg::ParsePatch(bad, &out, &err) && err.find(c.needle) != std::string::npos,
               "factory refused: " + c.to + " (" + err + ")");
    }

    const std::string stem = "mymaps_objects";
    luagen::ChunkSpec spec;
    spec.objects = {f};
    const std::string untuned = luagen::Text(stem, spec);
    spec.objects[0].factory.activation = 15;
    spec.objects[0].factory.inactiveTurns = 0;
    spec.objects[0].factory.schemeMines = false;
    const std::string tuned = luagen::Text(stem, spec);
    luagen::ChunkSpec parsed;
    Expect(tuned.starts_with("-- generated by Erg for mymaps_objects; edits are overwritten on export\n"
                             "-- minefactory: activation=15 inactiveTurns=0\n") &&
               tuned.find("        scheme.MineFactoryOn = false\n"
                          "        if scheme.Objects == 1 then scheme.Objects = 0 elseif scheme.Objects == 3 then scheme.Objects = 2 end\n"
                          "        CloseContainer(lock)\n") != std::string::npos,
           "the chunk carries the settings line and drops the scheme's start mines");
    Expect(luagen::IsGenerated(stem, tuned, &why) && luagen::Parse(stem, tuned, &parsed) && parsed.objects.size() == 1 &&
               parsed.objects[0].factory.activation == 15 && parsed.objects[0].factory.inactiveTurns == 0 &&
               !parsed.objects[0].factory.schemeMines,
           "a tuned factory chunk verifies and parses back: " + why);
    Expect(untuned.find("-- minefactory:") == std::string::npos && untuned.find("scheme.Objects") == std::string::npos,
           "an untuned factory's chunk is the one older Melange wrote");
    spec.objects[0].factory = {};
    spec.objects[0].factory.inactiveTurns = 3;
    const std::string idle = luagen::Text(stem, spec);
    Expect(idle.find("\n-- minefactory: inactiveTurns=3\n") != std::string::npos && idle.find("scheme.Objects") == std::string::npos &&
               luagen::IsGenerated(stem, idle, &why),
           "only the set fields are written: " + why);
    std::string run;
    for (const auto& [from, to] : {std::pair{"activation=15", "activation=300"}, std::pair{"activation=15", "activation=015"},
                                   std::pair{"activation=15 ", "activation=15  "}, std::pair{"activation=15", "speed=15"},
                                   std::pair{"inactiveTurns=0", "inactiveTurns=0 activation=1"},
                                   std::pair{"activation=15 inactiveTurns=0", "inactiveTurns=0 activation=15"}}) {
        const std::string m = Mutate(tuned, from, to);
        Expect(m != tuned && !luagen::Upgrade(stem, m, &run, &why), std::string("a hand-edited settings line is refused: ") + to);
    }
    // keepKnot: in the patch (written only when set), and on the settings line, alone or after the others
    {
        erg::Patch kp;
        const std::string withKeep = Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"keepKnot\":true}");
        Expect(erg::ParsePatch(withKeep, &kp, &err) && kp.objects.back().factory.keepKnot && erg::WritePatch(kp) == withKeep,
               "keepKnot round-trips in the patch: " + err);
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"keepKnot\":1}"), &kp, &err),
               "keepKnot must be a boolean");
        spec.objects[0].factory = {};
        spec.objects[0].factory.keepKnot = true;
        const std::string keep = luagen::Text(stem, spec);
        luagen::ChunkSpec kparsed;
        Expect(keep.find("\n-- minefactory: keepKnot=1\n") != std::string::npos && luagen::IsGenerated(stem, keep, &why) &&
                   luagen::Parse(stem, keep, &kparsed) && kparsed.objects[0].factory.keepKnot && !kparsed.objects[0].factory.Tweaks(),
               "keepKnot alone writes the settings line and parses back: " + why);
        spec.objects[0].factory.activation = 9;
        const std::string both = luagen::Text(stem, spec);
        Expect(both.find("\n-- minefactory: activation=9 keepKnot=1\n") != std::string::npos && luagen::IsGenerated(stem, both, &why),
               "keepKnot follows the other settings: " + why);
        Expect(!luagen::Upgrade(stem, Mutate(keep, "keepKnot=1", "keepKnot=2"), &run, &why), "keepKnot is 1 or absent");
    }
    // launchHeight / launchRadius: in the patch (written only when set, 1-255 and 0-255), and on the settings line
    {
        erg::Patch lp;
        const std::string withLaunch = Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchHeight\":20,\"launchRadius\":12}");
        Expect(erg::ParsePatch(withLaunch, &lp, &err) && lp.objects.back().factory.launchHeight == 20 && lp.objects.back().factory.launchRadius == 12 &&
                   erg::WritePatch(lp) == withLaunch,
               "launchHeight and launchRadius round-trip in the patch: " + err);
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchHeight\":0}"), &lp, &err), "launchHeight is 1 or more");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchRadius\":256}"), &lp, &err), "launchRadius is at most 255");
        spec.objects[0].factory = {};
        spec.objects[0].factory.launchRadius = 12;
        const std::string launch = luagen::Text(stem, spec);
        luagen::ChunkSpec lparsed;
        Expect(launch.find("\n-- minefactory: launchRadius=12\n") != std::string::npos && luagen::IsGenerated(stem, launch, &why) &&
                   luagen::Parse(stem, launch, &lparsed) && lparsed.objects[0].factory.launchRadius == 12 && !lparsed.objects[0].factory.launchHeight &&
                   lparsed.objects[0].factory.Launch() && !lparsed.objects[0].factory.Tweaks(),
               "launchRadius alone writes the settings line and parses back: " + why);
        spec.objects[0].factory.keepKnot = true;
        spec.objects[0].factory.launchHeight = 30;
        const std::string all = luagen::Text(stem, spec);
        Expect(all.find("\n-- minefactory: keepKnot=1 launchHeight=30 launchRadius=12\n") != std::string::npos && luagen::IsGenerated(stem, all, &why),
               "the launch follows keepKnot: " + why);
        Expect(!luagen::Upgrade(stem, Mutate(launch, "launchRadius=12", "launchHeight=0"), &run, &why), "launchHeight is 1 or more on the line");
    }
    // launchers: [[x, y, z], ...] world units in the patch (1-8, whole numbers), x:y:z/... on the settings line
    {
        erg::Patch lp;
        const std::string withL = Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchRadius\":10,\"launchers\":[[-300,900,40],[260,880,-120]]}");
        Expect(erg::ParsePatch(withL, &lp, &err) && lp.objects.back().factory.launchers.size() == 2 &&
                   lp.objects.back().factory.launchers[0] == std::array<int, 3>{-300, 900, 40} && erg::WritePatch(lp) == withL,
               "launchers round-trip in the patch: " + err);
        Expect(lp.objects.back().factory.Launch() && !lp.objects.back().factory.Default(), "launchers are a launch");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchers\":[]}"), &lp, &err), "launchers is not empty");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchers\":[[1,2]]}"), &lp, &err), "a launcher is [x, y, z]");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchers\":[[1.5,2,3]]}"), &lp, &err), "whole world units");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"launchers\":[[100001,2,3]]}"), &lp, &err), "within 100000");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}",
                                       "\"schemeMines\":false,\"launchers\":[[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0]]}"),
                                &lp, &err),
               "at most 8 launchers");
        spec.objects[0].factory = {};
        spec.objects[0].factory.launchRadius = 10;
        spec.objects[0].factory.launchers = {{-300, 900, 40}, {260, 880, -120}};
        const std::string ltext = luagen::Text(stem, spec);
        luagen::ChunkSpec lparsed;
        Expect(ltext.find("\n-- minefactory: launchRadius=10 launchers=-300:900:40/260:880:-120\n") != std::string::npos &&
                   luagen::IsGenerated(stem, ltext, &why) && luagen::Parse(stem, ltext, &lparsed) &&
                   lparsed.objects[0].factory.launchers == spec.objects[0].factory.launchers,
               "launchers go on the settings line and parse back: " + why);
        Expect(!luagen::Upgrade(stem, Mutate(ltext, "launchers=-300:900:40/260:880:-120", "launchers=-300:900"), &run, &why), "a launcher on the line is x:y:z");
        Expect(!luagen::Upgrade(stem, Mutate(ltext, "launchers=-300:900:40/260:880:-120", "launchers=-300:900:40/"), &run, &why), "no empty launcher");
        spec.objects[0].factory.launchers.clear();
        Expect(luagen::Text(stem, spec).find("launchers") == std::string::npos, "no launchers, no launchers key");
    }
    luagen::ChunkSpec noFactory;
    noFactory.placeObjects = true;
    std::string orphan2 = luagen::Text(stem, noFactory);
    orphan2.insert(orphan2.find('\n') + 1, "-- minefactory: activation=5\n");
    Expect(!luagen::IsGenerated(stem, orphan2, &why), "settings without a factory are refused");
    // throw: {maxRange, minRange?, apex?, pattern?, avoidWater?, targets?, scatter?, targetShare?} in the patch (the defaults
    // left out when written), "throw=min:max:apex:pattern:water:scatter:share targets=x:z/..." on the settings line
    {
        erg::Patch lp;
        const std::string least = Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40}}");
        Expect(erg::ParsePatch(least, &lp, &err) && lp.objects.back().factory.throwing && lp.objects.back().factory.throwing->maxRange == 40 &&
                   lp.objects.back().factory.throwing->minRange == 0 && lp.objects.back().factory.throwing->apex == 15 &&
                   lp.objects.back().factory.throwing->pattern == erg::ThrowPattern::Random && lp.objects.back().factory.throwing->avoidWater &&
                   lp.objects.back().factory.throwing->scatter == 10 && lp.objects.back().factory.throwing->targetShare == 50 && erg::WritePatch(lp) == least,
               "a throw of maxRange alone round-trips with its defaults: " + err);
        Expect(lp.objects.back().factory.Launch() && !lp.objects.back().factory.Default(), "a throw is a launch");
        const std::string full = Mutate(text, "\"schemeMines\":false}",
                                        "\"schemeMines\":false,\"throw\":{\"minRange\":20,\"maxRange\":110,\"apex\":20,\"pattern\":\"targets\",\"avoidWater\":false,"
                                        "\"targets\":[[-1500,300],[1200,-800]],\"scatter\":12,\"targetShare\":40}}");
        Expect(erg::ParsePatch(full, &lp, &err) && lp.objects.back().factory.throwing->pattern == erg::ThrowPattern::Targets &&
                   lp.objects.back().factory.throwing->targets.size() == 2 && lp.objects.back().factory.throwing->targets[1] == std::array<int, 2>{1200, -800} &&
                   !lp.objects.back().factory.throwing->avoidWater && erg::WritePatch(lp) == full,
               "a full throw round-trips in the patch: " + err);
        const std::string dflt = Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"minRange\":0,\"maxRange\":40,\"apex\":15,\"pattern\":\"random\"}}");
        Expect(erg::ParsePatch(dflt, &lp, &err) && erg::WritePatch(lp) == least, "defaults given are left out when written");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"minRange\":10}}"), &lp, &err), "a throw needs maxRange");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"minRange\":50,\"maxRange\":40}}"), &lp, &err), "maxRange is not under minRange");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":501}}"), &lp, &err), "maxRange is at most 500");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"pattern\":\"spiral\"}}"), &lp, &err), "the pattern is named");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"pattern\":\"targets\"}}"), &lp, &err), "pattern targets needs targets");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"targets\":[[1.5,2]]}}"), &lp, &err), "targets are whole world units");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"range\":1}}"), &lp, &err), "no unknown throw key");
        // the settings line
        luagen::ChunkSpec tspec = spec;
        tspec.objects[0].factory = {};
        erg::ThrowSpec th;
        th.minRange = 20, th.maxRange = 110, th.apex = 20, th.pattern = erg::ThrowPattern::Targets, th.avoidWater = false, th.scatter = 12, th.targetShare = 40;
        th.targets = {{-1500, 300}, {1200, -800}};
        tspec.objects[0].factory.throwing = th;
        tspec.objects[0].factory.launchers = {{-300, 900, 40}};
        const std::string ttext = luagen::Text(stem, tspec);
        luagen::ChunkSpec tparsed;
        Expect(ttext.find("\n-- minefactory: launchers=-300:900:40 throw=20:110:20:targets:0:12:40 targets=-1500:300/1200:-800\n") != std::string::npos &&
                   luagen::IsGenerated(stem, ttext, &why) && luagen::Parse(stem, ttext, &tparsed) && tparsed.objects[0].factory.throwing &&
                   *tparsed.objects[0].factory.throwing == th && tparsed.objects[0].factory.launchers.size() == 1,
               "the throw goes on the settings line after the launchers and parses back: " + why);
        tspec.objects[0].factory.throwing->targets.clear();
        tspec.objects[0].factory.throwing->pattern = erg::ThrowPattern::Ring;
        tspec.objects[0].factory.launchers.clear();
        const std::string ring = luagen::Text(stem, tspec);
        Expect(ring.find("\n-- minefactory: throw=20:110:20:ring:0:12:40\n") != std::string::npos && ring.find("targets=") == std::string::npos &&
                   luagen::IsGenerated(stem, ring, &why),
               "a throw without targets writes no targets key: " + why);
        Expect(!luagen::Upgrade(stem, Mutate(ring, "throw=20:110:20:ring:0:12:40", "throw=20:110:20:spiral:0:12:40"), &run, &why), "the pattern on the line is named");
        Expect(!luagen::Upgrade(stem, Mutate(ring, "throw=20:110:20:ring:0:12:40", "throw=20:110:20:ring:2:12:40"), &run, &why), "avoidWater on the line is 0 or 1");
        Expect(!luagen::Upgrade(stem, Mutate(ring, "throw=20:110:20:ring:0:12:40", "throw=20:110:20:ring:0:12"), &run, &why), "the throw on the line has every field");
        Expect(!luagen::Upgrade(stem, Mutate(ring, "throw=20:110:20:ring:0:12:40", "throw=20:110:20:ring:0:12:40 targets=1:2 targets=3:4"), &run, &why), "targets once");
        Expect(!luagen::Upgrade(stem, Mutate(ring, " throw=20:110:20:ring:0:12:40", " targets=1:2"), &run, &why), "targets need a throw");
        // the volley cap and the avoid corridors (mines round 4): in the patch only when set, on the settings line as two more
        // fields after targetShare (a line of the first seven is an older project's) and an avoid key of x:z/x:z lines split by '|'
        const std::string corr = Mutate(text, "\"schemeMines\":false}",
                                        "\"schemeMines\":false,\"throw\":{\"maxRange\":110,\"volley\":4,\"avoidRadius\":5,\"avoid\":[[[-1600,700],[-900,900],[-400,600]],[[600,-1500],[1000,-900]]]}}");
        Expect(erg::ParsePatch(corr, &lp, &err) && lp.objects.back().factory.throwing->volley == 4 && lp.objects.back().factory.throwing->avoidRadius == 5 &&
                   lp.objects.back().factory.throwing->avoid.size() == 2 && lp.objects.back().factory.throwing->avoid[0].size() == 3 &&
                   lp.objects.back().factory.throwing->avoid[1][1] == std::array<int, 2>{1000, -900} && erg::WritePatch(lp) == corr,
               "volley, avoidRadius and avoid round-trip in the patch: " + err);
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"volley\":11}}"), &lp, &err), "volley is at most 10");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"avoidRadius\":51}}"), &lp, &err), "avoidRadius is at most 50");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"avoid\":[[[1,2],[3,4]]]}}"), &lp, &err), "avoid needs avoidRadius");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"avoidRadius\":5,\"avoid\":[[[1,2]]]}}"), &lp, &err), "an avoid line has 2 points or more");
        Expect(!erg::ParsePatch(Mutate(text, "\"schemeMines\":false}", "\"schemeMines\":false,\"throw\":{\"maxRange\":40,\"avoidRadius\":5,\"avoid\":[[[1.5,2],[3,4]]]}}"), &lp, &err), "avoid points are whole world units");
        tspec.objects[0].factory.throwing->volley = 4;
        tspec.objects[0].factory.throwing->avoidRadius = 5;
        tspec.objects[0].factory.throwing->avoid = {{{-1600, 700}, {-900, 900}, {-400, 600}}, {{600, -1500}, {1000, -900}}};
        const std::string corrLine = luagen::Text(stem, tspec);
        luagen::ChunkSpec cparsed;
        Expect(corrLine.find("\n-- minefactory: throw=20:110:20:ring:0:12:40:4:5 avoid=-1600:700/-900:900/-400:600|600:-1500/1000:-900\n") != std::string::npos &&
                   luagen::IsGenerated(stem, corrLine, &why) && luagen::Parse(stem, corrLine, &cparsed) && cparsed.objects[0].factory.throwing &&
                   *cparsed.objects[0].factory.throwing == *tspec.objects[0].factory.throwing,
               "the volley, avoid radius and avoid lines go on the settings line and parse back: " + why);
        Expect(!luagen::Upgrade(stem, Mutate(corrLine, "throw=20:110:20:ring:0:12:40:4:5", "throw=20:110:20:ring:0:12:40:4"), &run, &why), "the volley and avoid radius come together");
        Expect(!luagen::Upgrade(stem, Mutate(corrLine, "throw=20:110:20:ring:0:12:40:4:5", "throw=20:110:20:ring:0:12:40:0:0"), &run, &why), "both at their defaults are left off the line");
        Expect(!luagen::Upgrade(stem, Mutate(corrLine, "throw=20:110:20:ring:0:12:40:4:5", "throw=20:110:20:ring:0:12:40:4:0"), &run, &why), "avoid lines need the radius");
        Expect(!luagen::Upgrade(stem, Mutate(corrLine, " avoid=-1600:700/-900:900/-400:600|600:-1500/1000:-900", " avoid=-1600:700|600:-1500/1000:-900"), &run, &why), "an avoid line on the settings line has 2 points or more");
        luagen::ChunkSpec oparsed;
        Expect(luagen::Parse(stem, ring, &oparsed) && oparsed.objects[0].factory.throwing && oparsed.objects[0].factory.throwing->volley == 0 &&
                   oparsed.objects[0].factory.throwing->avoidRadius == 0 && oparsed.objects[0].factory.throwing->avoid.empty(),
               "an older line of seven throw fields parses with no volley and no avoid");
    }
}

void TestManifestV2() {
    using L = melange::spice::Level;
    std::vector<lm::Error> errs;
    L surv{"a", "A", "multi", "", false, 0};
    surv.survivor = true;
    Expect(lm::Parse(Mod("maps", {surv}), &errs).empty() == !lm::kSurvivorTwins, "survivor follows the build flag");
    {
        surv.chunk = true;
        errs.clear();
        const auto twins = lm::Parse(Mod("maps", {surv}), &errs);
        Expect(twins.size() == 1 && twins[0].survivor && lm::SurvivorScripts(twins[0]) == std::vector<std::string>{"Survivor", "maps_a"},
               "a Survivor copy with a chunk runs Survivor then the level's chunk");
        surv.chunk = false;
    }
    for (const char* t : {"challenge", "deathmatch", "fort", "story"}) {
        errs.clear();
        Expect(lm::Parse(Mod("maps", {L{"a", "A", t, "", false, 0}}), &errs).empty() && !errs.empty() &&
                   errs[0].text.find("not supported in this version") != std::string::npos,
               std::string("type ") + t + " is not supported in this version");
    }
    L sim{"harbour", "Harbour", "multi", "", true, 0};
    sim.sim = "sim/harbour.lua";
    errs.clear();
    auto d = lm::Parse(Mod("maps", {sim}), &errs);
    Expect(d.size() == 1 && d[0].sim == "sim/harbour.lua", "a level sim script path is accepted");
    for (const char* bad : {"sim/../x.lua", "scripts/x.lua", "sim/x.txt", "/sim/x.lua", "sim\\x.lua", "sim/.lua", "sim/a b.lua"}) {
        sim.sim = bad;
        errs.clear();
        Expect(lm::Parse(Mod("maps", {sim}), &errs).empty(), std::string("a bad sim path is refused: ") + bad);
    }
    std::string why;
    Expect(lm::CheckSimText("wum.log('hi')\n-- \xc3\xa9\n", &why), "a UTF-8 script passes: " + why);
    Expect(!lm::CheckSimText("\xef\xbb\xbfwum.log(1)", &why), "a BOM is refused");
    Expect(!lm::CheckSimText("\x1bLua", &why), "bytecode is refused");
    Expect(!lm::CheckSimText("x = '\xc3'", &why), "invalid UTF-8 is refused");
    Expect(!lm::CheckSimText(std::string(lm::kMaxSimBytes + 1, 'a'), &why), "a script over 256 KB is refused");
    Expect(lm::TwinKey("maps_a") == "Multi.maps_a.S" && lm::SurvivorScripts(d[0]).front() == "Survivor", "twin key and scripts");
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
    const std::pair<const char*, std::string> docs[] = {
        {"synthetic-12.ergpatch.json", erg::WritePatch(SamplePatch(s)) + "\n"},
        {"synthetic-v2.json", erg::WriteScene(SceneV2()) + "\n"},
        {"synthetic-v2-base.json", erg::WriteScene(BaseV2()) + "\n"},
        {"synthetic-v2.ergpatch.json", erg::WritePatch(SamplePatchV2(BaseV2())) + "\n"},
    };
    for (auto& [file, text] : docs) {
        if (write) std::ofstream(dir + "/" + file, std::ios::binary) << text;
        else Expect(ReadFile(dir + "/" + file) == text, std::string("fixture ") + file + " matches the generator (run with --write)");
    }
}
}  // namespace

// spawns.mode areas in erg-scene and erg-patch: the block round-trips, its keys are refused in other modes, and the
// areas are validated (groups, knots, pattern, opposite pairs).
void TestSpawnAreasJson(const erg::Scene& small) {
    const std::string text = erg::WriteScene(small), plain = "\"spawns\":{\"mode\":\"" + std::string(erg::SpawnModeName(small.spawns)) + "\"}";
    Expect(text.find(plain) != std::string::npos, "the synthetic scene's spawns block is the plain mode");
    const std::string block = "\"spawns\":{\"mode\":\"areas\",\"areas\":[{\"name\":\"SAND W\",\"group\":\"sand\",\"knots\":4},"
                              "{\"name\":\"SAND E\",\"group\":\"sand\",\"knots\":4},{\"name\":\"PALACE\",\"group\":\"palace\",\"knots\":2}],"
                              "\"pattern\":[\"sand\",\"sand:opposite\",\"palace\"],\"opposite\":[[0,1]]}";
    auto with = [&](const std::string& b) {
        std::string t = text;
        t.replace(t.find(plain), plain.size(), b);
        return t;
    };
    erg::Scene s;
    std::string err;
    Expect(erg::ParseScene(with(block), &s, &err) && s.spawns == erg::SpawnMode::Areas && s.spawnAreas.areas.size() == 3 &&
               s.spawnAreas.pattern[1].opposite && s.spawnAreas.opposite.size() == 1 && s.spawnAreas.Knots() == 10,
           "a scene with spawn areas parses: " + err);
    Expect(erg::WriteScene(s) == with(block), "and writes back byte for byte");
    Expect(luagen::Needed(s) && luagen::Chunk(s).find("ergSpawnDeal(\"sand:4/sand:4/palace:2;sand/sand:opposite/palace;0-1\", n)") != std::string::npos,
           "and its chunk carries the areas");
    const std::pair<const char*, const char*> bad[] = {
        {"\"spawns\":{\"mode\":\"teams\",\"pattern\":[]}", "only for spawns.mode"},
        {"\"spawns\":{\"mode\":\"areas\"}", "spawns.areas"},
        {"\"spawns\":{\"mode\":\"areas\",\"areas\":[]}", "1-8 areas"},
        {"\"spawns\":{\"mode\":\"areas\",\"areas\":[{\"name\":\"A\",\"group\":\"Sand\",\"knots\":4}]}", "group"},
        {"\"spawns\":{\"mode\":\"areas\",\"areas\":[{\"name\":\"A\",\"group\":\"a\",\"knots\":9}]}", "knots"},
        {"\"spawns\":{\"mode\":\"areas\",\"areas\":[{\"name\":\"A\",\"group\":\"a\",\"knots\":4}],\"pattern\":[\"b\"]}", "no area is in group 'b'"},
        {"\"spawns\":{\"mode\":\"areas\",\"areas\":[{\"name\":\"A\",\"group\":\"a\",\"knots\":4}],\"opposite\":[[0,0]]}", "two different area indexes"},
        {"\"spawns\":{\"mode\":\"areas\",\"areas\":[{\"name\":\"A\",\"group\":\"a\",\"knots\":4},{\"name\":\"A\",\"group\":\"a\",\"knots\":4}]}", "used twice"},
        {"\"spawns\":{\"mode\":\"areas\",\"areas\":[{\"name\":\"A\",\"group\":\"a\",\"knots\":4,\"x\":1}]}", "unknown key"},
        {"\"spawns\":{\"mode\":\"dunes\"}", "\"random\", \"knots\", \"teams\" or \"areas\""},
    };
    for (const auto& [b, why] : bad)
        Expect(!erg::ParseScene(with(b), &s, &err) && err.find(why) != std::string::npos, std::string("refused: ") + b + " -> " + err);
    // erg-patch: the same block
    erg::Patch p;
    std::string ptext;
    {
        erg::Scene edited;
        Expect(erg::ParseScene(with(block), &edited, &err), "the edited scene parses");
        p.stem = small.stem;
        p.title = small.title;
        p.base = small.base;
        p.spawns = edited.spawns;
        p.spawnAreas = edited.spawnAreas;
        ptext = erg::WritePatch(p);
    }
    erg::Patch back;
    Expect(erg::ParsePatch(ptext, &back, &err) && back.spawns == erg::SpawnMode::Areas && back.spawnAreas == p.spawnAreas,
           "a patch with spawn areas round-trips: " + err);
    Expect(erg::WritePatch(back) == ptext, "and writes back byte for byte");
}

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
    TestV2Model();
    TestChunkGrammar();
    TestSpawnAreasJson(small);
    TestFactorySettings();
    TestManifestV2();
    Fixtures(std::string(MELANGE_SOURCE_DIR) + "/tests/fixtures/erg", write);
    printf("erg_scene_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
