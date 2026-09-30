// Offline self-test for terrain sculpting on the server side (no game, no game files): the voxel word rules, applying
// and diffing voxels ops over synthetic frames, the editor's sculpt fixture against the same generated words, and the
// shadow-cache deletion. Exit code 0 = all passed.
#include <cstdio>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "erg/patch.h"
#include "erg/scene.h"
#include "erg/voxels.h"

namespace erg = melange::erg;
namespace vx = melange::erg::voxels;
namespace fs = std::filesystem;

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

std::string ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str(), out;
    for (char c : s)
        if (c != '\r') out += c;
    return out;
}

// The same words as web/test/unit/erg-terrain.test.ts.
uint32_t Word(int64_t fid, int x, int y, int z, int Y) {
    const int64_t h = 1 + ((x * 7 + z * 13 + fid) % Y), m = (x * 3 + y + z * 5 + fid) % 64;
    uint32_t w = static_cast<uint32_t>(m) << 2;
    if (y < h) w |= 3;
    if (((x ^ z) & 3) == 0) w |= static_cast<uint32_t>((x * 31 + z) & 0xff) << 16;
    if (y % 5 == 0) w |= (1u << 8) | (static_cast<uint32_t>((m + 1) % 64) << 10);
    return w;
}

vx::FrameWords Words(const erg::Scene& s) {
    vx::FrameWords out;
    for (auto& f : s.frames) {
        if (f.voxels < 0) continue;
        auto& w = out[f.id];
        w.resize(vx::Cells(f));
        for (int z = 0; z < f.size[2]; ++z)
            for (int x = 0; x < f.size[0]; ++x)
                for (int y = 0; y < f.size[1]; ++y) w[(static_cast<size_t>(z) * f.size[0] + x) * f.size[1] + y] = Word(f.id, x, y, z, f.size[1]);
    }
    return out;
}

const std::string kSha(64, 'a'), kShaB(64, 'b');

erg::Scene TwoFrames() {
    erg::Scene s;
    s.stem = "test_sculpt";
    s.title = "Sculpt";
    s.base = {"Multi.Test", "game", "Test", {kSha, kShaB, ""}};
    erg::Frame root;
    root.id = 1;
    s.frames.push_back(root);
    for (int i = 0; i < 2; ++i) {
        erg::Frame f;
        f.id = 2 + i;
        f.parent = 1;
        f.size = {4, 5, 3};
        f.voxels = 10 + i;
        s.blobs.push_back({f.voxels, "voxels", f.id, vx::Cells(f) * 4});
        s.frames.push_back(f);
    }
    erg::Frame folder;
    folder.id = 4;
    folder.parent = 1;
    folder.size = {1, 1, 1};
    folder.folder = true;
    s.frames.push_back(folder);
    return s;
}

erg::Patch PatchFor(const erg::Scene& s) {
    erg::Patch p;
    p.stem = "mymaps_sculpt";
    p.title = "Sculpt";
    p.base = s.base;
    p.base.file.clear();
    return p;
}

erg::Op VoxOp(int64_t frame, std::vector<erg::VoxelRun> runs) {
    erg::Op op;
    op.kind = erg::Op::Kind::Voxels;
    op.frame = frame;
    op.runs = std::move(runs);
    return op;
}

void TestWords() {
    const uint32_t v = 3 | (5 << 2) | (1 << 8) | (9 << 10) | (0x44 << 16);
    Expect(vx::Solid(v) && vx::Material(v) == 5, "solid and material bits");
    Expect(vx::Carved(v) == (v & ~3u), "carve clears the solid bits only");
    Expect(vx::Filled(7) == (3u | (7u << 2)), "fill is the material alone");
    Expect(vx::Painted(v, 12) == ((v & ~0xfcu) | (12u << 2)) && vx::Painted(vx::Carved(v), 12) == vx::Carved(v), "paint");
    const struct { uint32_t base, now; bool ok; } cases[] = {
        {v, vx::Carved(v), true}, {v, vx::Filled(9), true}, {v, vx::Painted(v, 1), true}, {v, vx::Carved(vx::Painted(v, 2)), true},
        {vx::Carved(v), v, true}, {v, 3u | (0x44u << 16), false}, {0, 3u | (0x10u << 16), false}, {v, 2, false},
        {v, 0x01000003u, false}, {0, 0, true},
    };
    for (auto& c : cases) Expect(vx::ValidEdit(c.base, c.now) == c.ok, "ValidEdit " + std::to_string(c.base) + " -> " + std::to_string(c.now));
    std::vector<uint32_t> w = {0, 1, 0xffffffffu, 0x12345678u};
    std::vector<uint32_t> back;
    std::string err;
    Expect(vx::Decode(vx::Encode(w), &back, &err) && back == w, "encode/decode round trip");
    Expect(vx::Encode({0x04030201u}) == std::vector<uint8_t>{1, 2, 3, 4}, "little-endian words");
    Expect(!vx::Decode({1, 2, 3}, &back, &err), "a partial word is refused");
}

void TestApply() {
    const erg::Scene s = TwoFrames();
    const vx::FrameWords base = Words(s);
    std::string err;
    erg::Patch p = PatchFor(s);
    const auto& f2 = base.at(2);
    // Carve voxel 0..3 (keeping their words but the solid bits), fill 4, paint 5, then carve 4 again in a later op.
    std::vector<erg::VoxelRun> runs;
    for (uint32_t j = 0; j < 4; ++j) runs.push_back({j, 1, vx::Carved(f2[j])});
    runs.push_back({4, 1, vx::Filled(7)});
    runs.push_back({5, 1, vx::Solid(f2[5]) ? vx::Painted(f2[5], 9) : f2[5]});
    p.ops.push_back(VoxOp(2, runs));
    p.ops.push_back(VoxOp(3, {{0, 60, vx::Filled(1)}}));
    p.ops.push_back(VoxOp(2, {{4, 1, vx::Carved(vx::Filled(7))}}));
    Expect(erg::ValidatePatch(p, s, {vx::kAccepted, false}, &err), "the patch validates with voxels accepted: " + err);
    const std::string text = erg::WritePatch(p);
    erg::Patch parsed;
    Expect(erg::ParsePatch(text, &parsed, &err) && erg::WritePatch(parsed) == text, "the voxels patch round-trips: " + err);

    vx::FrameWords w = base;
    vx::Stats st;
    Expect(vx::ApplyOps(p, s, w, &err, &st), "ApplyOps: " + err);
    Expect(w.at(2)[4] == vx::Carved(vx::Filled(7)) && w.at(3)[59] == vx::Filled(1), "later ops win");
    Expect(st.frames == 2 && st.runs == 8, "stats count frames and runs");
    size_t solidBefore = 0;
    for (auto v : base.at(3)) solidBefore += vx::Solid(v);
    Expect(st.filled >= 60 - solidBefore, "stats count filled voxels");
    for (uint32_t j = 0; j < 4; ++j) Expect(w.at(2)[j] == vx::Carved(f2[j]), "carved voxel " + std::to_string(j) + " keeps its material");

    std::vector<erg::Op> ops;
    Expect(vx::DiffOps(s, base, w, &ops, &err), "DiffOps: " + err);
    erg::Patch again = PatchFor(s);
    again.ops = ops;
    vx::FrameWords w2 = base;
    Expect(vx::ApplyOps(again, s, w2, &err) && w2 == w, "the diff rebuilds the edit");
    Expect(ops.size() == 2 && ops[0].frame == 2 && ops[1].frame == 3, "one op per changed frame");

    std::map<int64_t, std::vector<uint8_t>> blobs = {{10, vx::Encode(base.at(2))}, {11, vx::Encode(base.at(3))}};
    Expect(vx::ApplyToBlobs(p, s, blobs, &err) && blobs.at(10) == vx::Encode(w.at(2)) && blobs.at(11) == vx::Encode(w.at(3)),
           "ApplyToBlobs: " + err);

    // Refusals leave the words untouched and name the op and run.
    const auto refused = [&](erg::Patch bad, const std::string& needle, const std::string& what) {
        vx::FrameWords t = base;
        std::string e;
        const bool ok = vx::ApplyOps(bad, s, t, &e);
        Expect(!ok && e.find(needle) != std::string::npos && t == base, what + " (" + e + ")");
    };
    erg::Patch bad = PatchFor(s);
    bad.ops.push_back(VoxOp(2, {{0, 1, f2[0]}, {1, 1, 3u | (0x5u << 16)}}));
    refused(bad, "ops[0].runs[1]", "a fill with a foreign blend is refused");
    bad.ops = {VoxOp(3, {{0, 1, 3}}), VoxOp(2, {{59, 2, 0}})};
    refused(bad, "ops[1].runs[0]", "a run past the frame is refused");
    bad.ops = {VoxOp(4, {{0, 1, 3}})};
    refused(bad, "has no voxels", "a frame without voxels is refused");
    bad.ops = {VoxOp(99, {{0, 1, 3}})};
    refused(bad, "not a frame", "an unknown frame is refused");
    vx::FrameWords missing = base;
    missing.erase(3);
    bad.ops = {VoxOp(3, {{0, 1, 3}})};
    Expect(!vx::ApplyOps(bad, s, missing, &err) && err.find("not loaded") != std::string::npos, "unloaded voxels are refused");

    // A frame whose diff needs more than 2000 runs cannot be written as a patch.
    erg::Scene big = TwoFrames();
    big.frames[1].size = {40, 4, 40};
    big.blobs[0].bytes = vx::Cells(big.frames[1]) * 4;
    vx::FrameWords bw;
    bw[2].resize(vx::Cells(big.frames[1]));
    for (size_t j = 0; j < bw[2].size(); ++j) bw[2][j] = 3u | (static_cast<uint32_t>(j % 64) << 2);
    vx::FrameWords carvedAll = bw;
    for (auto& v : carvedAll[2]) v = vx::Carved(v);
    Expect(!vx::DiffOps(big, bw, carvedAll, &ops, &err) && err.find("2000") != std::string::npos, "more than 2000 runs is refused");
    Expect(vx::TerrainChanged(p) && !vx::TerrainChanged(PatchFor(s)), "TerrainChanged");
}

void TestEditorFixture() {
    const fs::path dir = fs::path(MELANGE_SOURCE_DIR) / "tests" / "fixtures" / "erg";
    erg::Scene s;
    erg::Patch p;
    std::string err;
    Expect(erg::ParseScene(ReadFile(dir / "synthetic-12.json"), &s, &err), "synthetic-12 parses: " + err);
    Expect(erg::ParsePatch(ReadFile(dir / "synthetic-12-sculpt.ergpatch.json"), &p, &err), "the editor's sculpt patch parses: " + err);
    Expect(erg::ValidatePatch(p, s, {vx::kAccepted, false}, &err), "the editor's sculpt patch validates: " + err);
    Expect(!erg::ValidatePatch(p, s, {}, &err), "without the voxels rule it is refused");
    const vx::FrameWords base = Words(s);
    vx::FrameWords w = base;
    vx::Stats st;
    Expect(vx::ApplyOps(p, s, w, &err, &st), "the editor's sculpt patch applies under the server's rules: " + err);
    Expect(st.frames >= 10 && st.carved > 0 && st.filled > 0 && st.painted > 0, "it carves, fills and paints");
    std::vector<erg::Op> ops;
    Expect(vx::DiffOps(s, base, w, &ops, &err), "DiffOps over the editor's edit: " + err);
    erg::Patch again = p;
    again.ops = ops;
    Expect(erg::WritePatch(again) == erg::WritePatch(p), "the server's diff equals the editor's runs");
}

void TestShadows() {
    Expect(vx::ShadowFiles("a_b") == std::vector<std::string>{"Maps/a_bDAY.csh", "Maps/a_bEVENING.csh", "Maps/a_bNIGHT.csh"}, "shadow names");
    const fs::path game = fs::temp_directory_path() / ("erg_voxels_selftest_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(game, ec);
    const auto touch = [&](const fs::path& p) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
    };
    touch(game / "Melange/cache/Maps/a_bDAY.csh");
    touch(game / "Melange/cache/Maps/a_bNIGHT.csh");
    touch(game / "Melange/cache/Maps/a_bcDAY.csh");
    touch(game / "Mods/m/assets/levels/Maps/a_bEVENING.csh");
    touch(game / "Mods/m/assets/levels/Maps/a_b.xan");
    touch(game / "Data/Maps/a_bDAY.csh");
    std::string err;
    Expect(vx::DeleteShadows(game, {"Melange/cache", "Mods/m/assets/levels"}, "a_b", &err) == 3 && err.empty(), "three shadows deleted: " + err);
    Expect(fs::exists(game / "Melange/cache/Maps/a_bcDAY.csh") && fs::exists(game / "Mods/m/assets/levels/Maps/a_b.xan"),
           "another stem's shadow and the level file stay");
    Expect(vx::DeleteShadows(game, {"Data", "data/x", "../x", "C:/x", "/x"}, "a_b", &err) == 0 && fs::exists(game / "Data/Maps/a_bDAY.csh") &&
               err.find("refused") != std::string::npos,
           "Data, parent and absolute roots are refused");
    Expect(vx::DeleteShadows(game, {"Melange/cache"}, "a.b", &err) == 0 && err == "not a level stem", "a stem with a dot is refused");
    fs::remove_all(game, ec);
}
}  // namespace

int main() {
    TestWords();
    TestApply();
    TestEditorFixture();
    TestShadows();
    printf("erg_voxels_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
