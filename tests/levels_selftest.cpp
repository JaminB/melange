// Offline self-test for level registration (no game, no game files): the level-root naming rules, the pack checks in
// load order, root order, the .csh guard over a temporary folder, the online map gate, the one-shot override and the
// "erg" channel payloads. Exit code 0 = all passed.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "levels/csh.h"
#include "levels/gate.h"
#include "levels/roots.h"
#include "levels/test.h"
#include "mods/spice.h"

namespace fs = std::filesystem;
namespace roots = melange::levels::roots;
namespace csh = melange::levels::csh;
namespace gate = melange::levels::gate;
namespace lt = melange::levels::test;
using melange::levels::Online;
using melange::levels::Source;
using melange::levels::TestState;
using melange::mods::PeerStatus;

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

const std::vector<melange::assets::crcsafe::Entry> kCrc = {{"Data/Tweak/SCRIPTS.XOM", 1}, {"Data/Maps/mymaps_crc.xan", 2}};

roots::Listing L(std::vector<std::string> files, std::vector<std::string> dirs = {"Maps"}) {
    roots::Listing l;
    l.exists = true;
    l.files = std::move(files);
    l.dirs = std::move(dirs);
    return l;
}

bool Refused(const roots::Listing& l, const std::string& needle, const std::string& prefix = "mymaps") {
    std::string err;
    const bool ok = roots::CheckLevelRoot(prefix, l, kCrc, &err);
    if (!ok && err.find(needle) == std::string::npos) printf("  (reason was: %s)\n", err.c_str());
    return !ok && err.find(needle) != std::string::npos;
}

void TestRootRules() {
    std::string err;
    const auto good = L({"mymaps_harbour.XOM", "mymaps_harbour.lub", "Maps/mymaps_harbour.xan", "Maps/mymaps_harbour.hmp",
                         "Maps/mymaps_harbour.txt", "mymaps_REG.XOM"});
    Expect(roots::CheckLevelRoot("mymaps", good, kCrc, &err), "a well-formed level root passes: " + err);
    Expect(roots::CheckLevelRoot("mymaps", L({"MYMAPS_Harbour.XOM", "maps/MyMaps_harbour.xan"}, {"maps"}), kCrc, &err),
           "names and the Maps folder match case-insensitively");
    Expect(roots::CheckLevelRoot("mymaps", L({}, {}), kCrc, &err), "an empty level root passes the naming rules");
    Expect(Refused(L({"Maps/other.xan"}), "is not named 'Maps/mymaps_*'"), "Maps/other.xan is refused");
    Expect(Refused(L({"other.XOM"}), "is not named 'mymaps_*'"), "a top-level file without the prefix is refused");
    Expect(Refused(L({"mymapsx_a.XOM"}), "is not named"), "the prefix must be followed by '_'");
    Expect(Refused(L({"Maps/mymaps_aDAY.csh"}), ".csh"), "a .csh is refused");
    Expect(Refused(L({"mymaps_a.CSH"}), ".csh"), "a .CSH at the top is refused");
    Expect(Refused(L({"Maps/mymaps_a.b.xan"}), "'.' in its stem"), "a stem with a dot is refused");
    Expect(Refused(L({"Maps/mymaps_" + std::string(42, 'a') + ".xan"}), "longer than 48"), "a 49-character stem is refused");
    Expect(roots::CheckLevelRoot("mymaps", L({"Maps/mymaps_" + std::string(41, 'a') + ".xan"}), kCrc, &err),
           "a 48-character stem passes");
    Expect(Refused(L({"Maps/multi_dinermight.xan"}), "vanilla level name", "multi"), "a vanilla stem is refused");
    Expect(Refused(L({"Maps/Multi_DinerMight.hmp"}), "vanilla level name", "multi"), "vanilla stems match case-insensitively");
    Expect(Refused(L({"Maps/MyMaps_CRC.xan"}), "protected file"), "a CRC-listed name is refused");
    Expect(Refused(L({"mymaps_a.XOM"}, {"Maps", "Textures"}), "folder other than Maps"), "another folder is refused");
    Expect(Refused(L({"Maps/sub/mymaps_a.xan"}, {"Maps", "Maps/sub"}), "folder other than Maps"),
           "a folder inside Maps is refused");
    Expect(Refused(L({"Maps/sub/mymaps_a.xan"}, {"Maps"}), "folder other than Maps"), "a file below Maps/x is refused");
    auto link = L({"mymaps_a.XOM"});
    link.other = {"Maps/mymaps_b.xan"};
    Expect(Refused(link, "not a plain file"), "a link or special file is refused");
}

melange::levels::manifest::LevelDecl Decl(const std::string& mod, const std::string& slug, bool chunk) {
    melange::levels::manifest::LevelDecl d;
    d.mod = mod;
    d.slug = slug;
    d.stem = mod + "_" + slug;
    d.title = slug;
    d.type = "multi";
    d.chunk = chunk;
    return d;
}

void TestBuilt() {
    std::string err;
    const auto d1 = Decl("mymaps", "a", false), d2 = Decl("mymaps", "b", true);
    Expect(roots::CheckBuilt({d1}, L({"mymaps_a.XOM", "Maps/mymaps_a.xan"}), &err), "built files present");
    Expect(roots::CheckBuilt({d1}, L({"MYMAPS_A.xom", "maps/mymaps_a.XAN"}), &err), "built files match case-insensitively");
    Expect(!roots::CheckBuilt({d2}, L({"mymaps_b.XOM", "Maps/mymaps_b.xan"}), &err) && err.find("mymaps_b.lub") != std::string::npos,
           "a chunk level needs its .lub");
    Expect(!roots::CheckBuilt({d1}, L({"mymaps_a.XOM"}), &err) && err.rfind("not built", 0) == 0,
           "a missing .xan reads as not built: " + err);
}

melange::spice::Manifest M(const std::string& id, std::vector<std::string> slugs, bool content = true) {
    melange::spice::Manifest m;
    m.id = id;
    m.version = "1.0.0";
    m.content = content;
    for (auto& s : slugs) {
        melange::spice::Level l;
        l.slug = s;
        l.title = "Title " + s;
        m.levels.push_back(l);
    }
    return m;
}

void TestPacks() {
    std::map<std::string, roots::Listing> disk;
    const auto lister = [&](const fs::path& p) {
        const std::string key = p.generic_string();
        auto it = disk.find(key);
        return it == disk.end() ? roots::Listing{} : it->second;
    };
    const auto a = M("pack-a", {"one", "two"}), b = M("pack_a", {"three"}), c = M("pack-c", {"x"}),
               d = M("pack-d", {"y"}), e = M("clientonly", {"z"}, false), f = M("multi", {"dinermight"}), g = M("nolevels", {});
    disk["Mods/pack-a/assets/levels"] = L({"pack_a_one.XOM", "pack_a_two.XOM", "Maps/pack_a_one.xan", "Maps/pack_a_two.xan"});
    disk["Mods/pack_a/assets/levels"] = L({"pack_a_three.XOM", "Maps/pack_a_three.xan"});
    disk["Mods/pack-c/assets/levels"] = L({"pack_c_x.XOM", "Maps/pack_c_x.xan", "Maps/pack_c_xDAY.csh"});
    const std::vector<roots::PackInput> in = {{&a, "Mods/pack-a"}, {&b, "Mods/pack_a"}, {&c, "Mods/pack-c"},
                                              {&d, "Mods/pack-d"}, {&e, "Mods/clientonly"}, {&f, "Mods/multi"},
                                              {&g, "Mods/nolevels"}};
    const auto v = roots::CheckPacks(in, kCrc, true, lister);
    Expect(v.size() == 6, "every mod with levels gets a verdict (" + std::to_string(v.size()) + ")");
    auto find = [&](const std::string& id) {
        for (auto& x : v)
            if (x.mod == id) return x;
        return roots::PackVerdict{"?", false, "missing", {}};
    };
    const auto va = find("pack-a");
    Expect(va.ok && va.levels.size() == 2 && va.levels[0].stem == "pack_a_one", "pack-a is accepted with its stems");
    const auto vb = find("pack_a");
    Expect(!vb.ok && vb.reason.find("prefix") != std::string::npos, "a second mod with the same prefix is refused: " + vb.reason);
    const auto vc = find("pack-c");
    Expect(!vc.ok && vc.reason.find(".csh") != std::string::npos && vc.levels.empty(), "a pack shipping a .csh is refused");
    const auto vd = find("pack-d");
    Expect(!vd.ok && vd.reason.rfind("not built", 0) == 0, "a pack without assets/levels is not built: " + vd.reason);
    Expect(!find("clientonly").ok, "levels in a client-only mod are refused");
    const auto vf = find("multi");
    Expect(!vf.ok && vf.reason.find("vanilla") != std::string::npos, "a stem equal to a vanilla stem is refused: " + vf.reason);
    const auto none = roots::CheckPacks(in, {}, false, lister);
    bool allRefused = true;
    for (auto& x : none) allRefused &= !x.ok;
    Expect(allRefused, "an unverifiable CRC table refuses every pack");

    std::vector<melange::spice::Manifest> many;
    std::vector<roots::PackInput> big;
    many.reserve(5);
    for (int i = 0; i < 5; ++i) {
        std::vector<std::string> slugs;
        for (int k = 0; k < 30; ++k) slugs.push_back("s" + std::to_string(k));
        many.push_back(M("big" + std::to_string(i), slugs));
        roots::Listing l = L({});
        for (auto& s : slugs) {
            l.files.push_back("big" + std::to_string(i) + "_" + s + ".XOM");
            l.files.push_back("Maps/big" + std::to_string(i) + "_" + s + ".xan");
        }
        disk["Mods/big" + std::to_string(i) + "/assets/levels"] = l;
    }
    for (auto& m : many) big.push_back({&m, fs::path("Mods") / m.id});
    const auto vbig = roots::CheckPacks(big, kCrc, true, lister);
    int accepted = 0;
    for (auto& x : vbig) accepted += x.ok;
    Expect(accepted == 4 && !vbig[4].ok && vbig[4].reason.find("128") != std::string::npos,
           "the fifth 30-level pack passes the 128-level limit and is refused");
}

std::string Tmp() {
    wchar_t buf[MAX_PATH];
    GetTempPathW(MAX_PATH, buf);
    const fs::path p = fs::path(buf) / (L"melange_levels_selftest_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p.string();
}

void Write(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

void TestListing(const fs::path& base) {
    const fs::path root = base / "Mods" / "listing" / "assets" / "levels";
    Write(root / "listing_a.XOM", "x");
    Write(root / "Maps" / "listing_a.xan", "x");
    Write(root / "Maps" / "deep" / "listing_b.xan", "x");
    Write(root / "Other" / "x.txt", "x");
    const auto l = roots::ListLevelRoot(root);
    Expect(l.exists, "the listing sees the folder");
    Expect(l.files == std::vector<std::string>{"Maps/listing_a.xan", "Other/x.txt", "listing_a.XOM"},
           "files are listed relative, one folder down");
    Expect(l.dirs == std::vector<std::string>{"Maps", "Maps/deep", "Other"}, "folders are listed (Maps/deep included)");
    std::string err;
    Expect(!roots::CheckLevelRoot("listing", l, kCrc, &err), "the listed extra folders are refused");
    Expect(!roots::ListLevelRoot(root / "nothing").exists, "a missing folder reads as missing");

    Expect(roots::GameRelative(base, root) == "Mods/listing/assets/levels", "the level root is game-relative");
    Expect(roots::GameRelative(base / "Mods", base / "Other").empty(), "a folder outside the game folder is refused");
    Expect(roots::GameRelative(base, base / "Mods" / "my.mod" / "assets" / "levels").empty(), "a '.' in the root path is refused");
}

void TestOrder() {
    const auto o = roots::AddOrder({"Mods/a/assets/levels", "Mods/b/assets/levels"}, true, true);
    Expect(o.size() == 4 && o[0] == "Mods/a/assets/levels" && o[2] == roots::kTestRel && o.back() == roots::kCacheRel,
           "pack roots in load order, then the Test workspace, then the cache last (searched first)");
    Expect(roots::AddOrder({}, false, true) == std::vector<std::string>{roots::kCacheRel}, "the cache alone");
    Expect(roots::IsShadowOf("mymaps_aDAY.csh", "mymaps_a") && roots::IsShadowOf("MYMAPS_Anight.CSH", "mymaps_a") &&
               roots::IsShadowOf("mymaps_aEVENING.csh", "mymaps_a"),
           "the three times of day match");
    Expect(!roots::IsShadowOf("mymaps_abDAY.csh", "mymaps_a") && !roots::IsShadowOf("mymaps_a.csh", "mymaps_a") &&
               !roots::IsShadowOf("mymaps_aDAY.xan", "mymaps_a") && !roots::IsShadowOf("mymaps_DAY.csh", "mymaps_a"),
           "another stem's shadows and other files do not match");
}

void Age(const fs::path& p, int seconds) {
    std::error_code ec;
    fs::last_write_time(p, fs::last_write_time(p, ec) - std::chrono::seconds(seconds), ec);
}

void TestCsh(const fs::path& base) {
    const fs::path pack = base / "pack" / "Maps", cache = base / "cache" / "Maps", side = base / "cache" / "levels";
    const fs::path xan = pack / "mymaps_a.xan";
    Write(xan, "voxels v1");
    Write(pack / "mymaps_aDAY.csh", "old");
    Write(cache / "mymaps_aNIGHT.csh", "old");
    Write(cache / "mymaps_abDAY.csh", "other level");
    Write(cache / "mymaps_bDAY.csh", "other level");
    auto r = csh::Guard(xan, "mymaps_a", side, {pack, cache});
    Expect(r.ok && r.changed && r.deleted == 2 && r.sha.size() == 64, "the first start deletes the level's shadows");
    Expect(fs::exists(cache / "mymaps_abDAY.csh") && fs::exists(cache / "mymaps_bDAY.csh"), "other levels' shadows stay");
    Expect(fs::exists(side / "mymaps_a.xan.sha"), "the sidecar is written");
    Write(cache / "mymaps_aDAY.csh", "generated");
    r = csh::Guard(xan, "mymaps_a", side, {pack, cache});
    Expect(r.ok && !r.changed && r.deleted == 0 && fs::exists(cache / "mymaps_aDAY.csh"),
           "an unchanged .xan keeps the shadow (idempotent, the host runs set-up twice)");
    Age(cache / "mymaps_aDAY.csh", 3600);
    r = csh::Guard(xan, "mymaps_a", side, {pack, cache});
    Expect(r.ok && !r.changed && r.deleted == 1 && !fs::exists(cache / "mymaps_aDAY.csh"), "a shadow older than its .xan goes");
    Write(cache / "mymaps_aDAY.csh", "generated");
    Write(xan, "voxels v2");
    r = csh::Guard(xan, "mymaps_a", side, {pack, cache});
    Expect(r.ok && r.changed && r.deleted == 1 && !fs::exists(cache / "mymaps_aDAY.csh"), "an edited .xan deletes the shadow");
    r = csh::Guard(pack / "missing.xan", "missing", side, {pack, cache});
    Expect(!r.ok && !r.error.empty(), "a missing .xan is reported");
    Write(cache / "mymaps_aEVENING.csh", "x");
    Expect(csh::Purge("mymaps_a", {pack, cache}) == 1, "Purge removes the stem's shadows");
}

gate::Member Mem(const std::string& name, PeerStatus s, const std::string& mods = "") { return {name, s, mods}; }

void TestGate() {
    gate::Input in;
    Expect(!gate::Evaluate(in).hold && gate::Evaluate(in).status == Online::NotInLobby, "offline: allowed, not in a lobby");
    in.inLobby = true;
    in.owner = true;
    in.key = "Multi.DinerMight";
    in.members = {Mem("Vera", PeerStatus::Vanilla)};
    auto v = gate::Evaluate(in);
    Expect(!v.hold && v.status == Online::Allowed, "a vanilla level plays with a vanilla member");
    in.key = "Multi.mymaps_a";
    in.source = Source::Pack;
    in.title = "Harbour Brawl";
    in.mod = "mymaps";
    in.modVersion = "1.0.0";
    in.members = {Mem("Mia", PeerStatus::Match, "mymaps@1.0.0"), Mem("Max", PeerStatus::Match, "mymaps@1.0.0")};
    v = gate::Evaluate(in);
    Expect(!v.hold && v.status == Online::Allowed, "a pack level with every member matching is allowed");
    in.members.push_back(Mem("Vera", PeerStatus::Vanilla));
    v = gate::Evaluate(in);
    Expect(v.hold && v.status == Online::NotAllMatch && v.why == "Harbour Brawl is a mod map; Vera doesn't have mymaps",
           "a vanilla member holds the start: " + v.why);
    in.members = {Mem("Otto", PeerStatus::Mismatch, "other@2.0.0"), Mem("Pia", PeerStatus::Mismatch, "mymaps@1.0.0,x@1"),
                  Mem("Ned", PeerStatus::MelangeVanilla), Mem("Una", PeerStatus::Unknown), Mem("Vic", PeerStatus::Mismatch, "mymaps@0.9.0")};
    v = gate::Evaluate(in);
    Expect(v.hold && v.members.size() == 5 && v.members[0] == "Otto doesn't have mymaps" &&
               v.members[1] == "Pia's mods differ from ours" && v.members[2] == "Ned doesn't have mymaps" &&
               v.members[4] == "Vic has mymaps 0.9.0, not 1.0.0",
           "each offending member is named with its reason");
    in.owner = false;
    v = gate::Evaluate(in);
    Expect(!v.hold && v.status == Online::NotAllMatch, "a joiner never holds, but sees the status");
    in.owner = true;
    in.members = {Mem("Mia", PeerStatus::Match, "mymaps@1.0.0")};
    in.online = false;
    v = gate::Evaluate(in);
    Expect(v.hold && v.status == Online::NotAllMatch && v.why.find("Online=0") != std::string::npos, "Online=0 holds every pack level");
    in.online = true;
    in.source = Source::Test;
    in.title = "Erg test";
    v = gate::Evaluate(in);
    Expect(v.hold && v.status == Online::TestLevel, "a Test level always holds");
    in.source = Source::Vanilla;
    in.key = "Multi.oldpack_a";
    in.known = false;
    in.title.clear();
    v = gate::Evaluate(in);
    Expect(v.hold && v.why.find("Multi.oldpack_a") != std::string::npos, "an unregistered lobby level holds (would crash both)");
    in.key.clear();
    Expect(!gate::Evaluate(in).hold, "no level key: nothing to hold");

    Expect(gate::KeepInList(Source::Pack, false, false, false) && gate::KeepInList(Source::Test, false, true, true),
           "offline lists keep every mod level");
    Expect(gate::KeepInList(Source::Pack, true, true, true) && !gate::KeepInList(Source::Pack, true, true, false) &&
               !gate::KeepInList(Source::Pack, true, false, true) && !gate::KeepInList(Source::Test, true, true, true) &&
               gate::KeepInList(Source::Vanilla, true, false, false),
           "network lists: pack levels only when all match and Online=1; Test never; vanilla always");
    Expect(gate::KeepInPool(Source::Vanilla, false) && !gate::KeepInPool(Source::Pack, false) &&
               gate::KeepInPool(Source::Pack, true) && !gate::KeepInPool(Source::Test, true),
           "random pools: RandomPool decides for packs, Test levels never");
    Expect(gate::AllMatch({}) && !gate::AllMatch({Mem("a", PeerStatus::Unknown)}), "AllMatch");
    Expect(gate::HasMod("a@1,mymaps@1.0.0", "mymaps", "1.0.0") && !gate::HasMod("a@1,mymaps@1.0.0", "mymaps", "1.0") &&
               !gate::HasMod("", "mymaps", "1.0.0") && !gate::HasMod("xmymaps@1.0.0", "mymaps", "1.0.0"),
           "HasMod matches whole entries");
    std::string m, ver, t;
    Expect(gate::LevelValue("", "1", "x").empty(), "no member value for a vanilla level");
    Expect(gate::ParseLevelValue(gate::LevelValue("mymaps", "1.0.0", "Harbour; Brawl"), &m, &ver, &t) && m == "mymaps" &&
               ver == "1.0.0" && t == "Harbour; Brawl",
           "the mlg.lvl value round-trips");
    Expect(!gate::ParseLevelValue("2;a;b;c", &m, &ver, &t) && !gate::ParseLevelValue("1;;b;c", &m, &ver, &t) &&
               !gate::ParseLevelValue("1;a", &m, &ver, &t),
           "malformed mlg.lvl values are ignored");
}

void TestOverride() {
    using O = lt::Override;
    O o;
    O::Event ev;
    Expect(o.Take("Multi.DinerMight", 0, &ev).empty() && ev == O::Event::None, "nothing armed: the frontend's choice loads");
    Expect(!o.Arm("", 10, 0), "an empty key is refused");
    Expect(o.Arm("Multi.ergtest_p1", 120, 1000) && o.armed(), "armed");
    Expect(o.Update(false, 60000) == O::Event::None, "still armed before the timeout");
    Expect(o.Take("Multi.DinerMight", 61000, &ev) == "Multi.ergtest_p1" && ev == O::Event::Started, "the first set-up loads the override");
    Expect(!o.armed() && o.phase() == O::Phase::Starting, "used once: no longer armed");
    Expect(!o.Arm("Multi.other", 10, 61500), "re-arming while a Test start is under way is refused");
    Expect(o.Take("Multi.DinerMight", 61500, &ev) == "Multi.ergtest_p1" && ev == O::Event::None,
           "the second set-up of the same start gets the same override");
    Expect(o.Take("Multi.Other", 61500, &ev).empty(), "a different frontend choice is not overridden");
    Expect(o.Update(true, 62000) == O::Event::Playing && o.phase() == O::Phase::Playing, "the match started");
    Expect(o.Update(true, 70000) == O::Event::None, "playing");
    Expect(o.Update(false, 90000) == O::Event::Ended && o.phase() == O::Phase::Idle, "back at the menu: ended and idle");
    Expect(o.Take("Multi.DinerMight", 91000, &ev).empty(), "the next start is the frontend's again");

    Expect(o.Arm("Multi.ergtest_p1", 5, 0), "armed with a 5 s timeout");
    Expect(o.Update(false, 5001) == O::Event::Expired && !o.armed(), "the timeout disarms");
    Expect(o.Arm("Multi.ergtest_p1", 5, 0) && o.Take("x", 6000, &ev).empty() && ev == O::Event::Expired,
           "a late set-up after the timeout is not overridden");
    Expect(o.Arm("Multi.ergtest_p1", 120, 0) && !o.Take("x", 1, &ev).empty(), "armed and started");
    Expect(o.Update(false, 1 + O::kStartWindowMs + 1) == O::Event::Abandoned && o.phase() == O::Phase::Idle,
           "a start that never reaches a match is abandoned");
    Expect(o.Arm("Multi.a", 120, 0), "armed");
    o.Disarm();
    Expect(!o.armed() && o.Take("x", 1, &ev).empty(), "Disarm");
    Expect(o.Arm("Multi.a", 999999, 0) && o.Update(false, static_cast<uint64_t>(O::kMaxTimeoutS) * 1000 + 1) == O::Event::Expired,
           "the timeout is capped");
}

void TestWire() {
    Expect(lt::StateJson(TestState::Armed, "Multi.ergtest_p1", "") == R"({"state":"armed","key":"Multi.ergtest_p1","detail":""})",
           "state payload");
    Expect(lt::StateJson(TestState::Failed, "k", "a \"b\"\n") == R"({"state":"failed","key":"k","detail":"a \"b\"\u000a"})",
           "state payload escapes");
    float w = 40.f;
    Expect(lt::LevelJson("Multi.mymaps_a", "mymaps_a", Source::Pack, false, &w) ==
               R"({"level":"Multi.mymaps_a","stem":"mymaps_a","source":"pack","online":false,"water":40.0000})",
           "level payload with water");
    Expect(lt::LevelJson("Multi.DinerMight", "", Source::Vanilla, true, nullptr) ==
               R"({"level":"Multi.DinerMight","stem":"","source":"vanilla","online":true,"water":null})",
           "level payload without water");
    const char* names[] = {"idle", "registering", "registered", "armed", "starting", "playing", "ended", "failed"};
    bool all = true;
    for (int i = 0; i < 8; ++i) all &= std::string(lt::StateName(static_cast<TestState>(i))) == names[i];
    Expect(all, "state names follow TestState");
}
}  // namespace

int main() {
    TestRootRules();
    TestBuilt();
    TestPacks();
    const fs::path base = Tmp();
    TestListing(base);
    TestOrder();
    TestCsh(base);
    std::error_code ec;
    fs::remove_all(base, ec);
    TestGate();
    TestOverride();
    TestWire();
    printf("levels_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
