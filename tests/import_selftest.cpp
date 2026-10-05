// The local content importer without the game or the network. Every input is synthetic and built here: a small
// Worms-shaped zip (registry, language banks, descriptors, maps, a preview) with decoys around it, a vanilla map in a
// temp game folder, and a recipe pinned to the zip's hash.
#include <windows.h>

#include <miniz.h>

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "erg/xomutil.h"
#include "import/archive.h"
#include "import/generate.h"
#include "import/importer.h"
#include "import/plan.h"
#include "import/recipe.h"
#include "import/w4reader.h"
#include "levels/hidden.h"
#include "mods/spice.h"
#include "store/install.h"
#include "tools/hash.h"
#include "xom/xom.h"

namespace imp = melange::import;
namespace xom = melange::xom;
namespace xu = melange::erg::xomutil;
namespace inst = melange::store::install;
namespace hidden = melange::levels::hidden;

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

std::wstring W(const std::string& s) {
    std::wstring w;
    for (char c : s) w.push_back(static_cast<unsigned char>(c));
    return w;
}

std::string N(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c));
    return s;
}

std::vector<uint8_t> B(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& b) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    const bool ok = b.empty() || fwrite(b.data(), 1, b.size(), f) == b.size();
    fclose(f);
    return ok;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::vector<uint8_t> out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return out;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void MakeDir(const std::wstring& p) { CreateDirectoryW(p.c_str(), nullptr); }

// Every file under dir, relative with '/', and its SHA-256.
void Tree(const std::wstring& dir, const std::string& rel, std::map<std::string, std::string>* out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::string r = rel + N(name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) Tree(dir + L"\\" + name, r + "/", out);
        else (*out)[r] = melange::hashutil::Sha256HexFile(dir + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------- synthetic XOM documents
std::vector<uint8_t> Serialize(const xom::Document& d) {
    std::vector<uint8_t> out;
    std::string e;
    if (!xom::serialize(d, out, &e)) printf("serialize: %s\n", e.c_str());
    return out;
}

struct Reg {
    std::string key, file, name, image, scripts;
    int type = 3;
};

std::vector<uint8_t> Registry(const std::vector<Reg>& regs) {
    xom::Document d;
    d.schmRec = {1, 0, 0};
    d.strings = {""};
    for (const char* c : {"XContainer", "XResourceDetails", "XContainerResourceDetails", "XDataBank", "WXFE_LevelDetails"})
        xu::EnsureType(d, c, "");
    d.types.back().version = 1;
    const uint32_t n = static_cast<uint32_t>(regs.size());
    xom::Object bank;
    xu::NewObject(d, "XDataBank", &bank, nullptr);
    for (uint32_t i = 0; i < n; ++i) {
        xom::Object o;
        xu::NewObject(d, "XContainerResourceDetails", &o, nullptr);
        xu::SetStr(o, "Name", regs[i].key);
        *o.field("Value") = xu::RefValue(n + 2 + i);
        o.field("Flags")->setInt(80);
        d.objects.push_back(std::move(o));
        bank.field("ContainerResources")->items.push_back(xu::RefValue(i + 1));
    }
    d.objects.push_back(std::move(bank));
    for (const Reg& r : regs) {
        xom::Object o;
        xu::NewObject(d, "WXFE_LevelDetails", &o, nullptr);
        xu::SetStr(o, "Frontend_Name", r.name);
        xu::SetStr(o, "Frontend_Image", r.image);
        xu::SetStr(o, "Level_ScriptName", r.scripts);
        xu::SetStr(o, "Level_FileName", r.file);
        o.field("Level_Type")->setInt(r.type);
        o.field("Theme_Type")->setInt(5);
        d.objects.push_back(std::move(o));
    }
    d.root = n + 1;
    return Serialize(d);
}

std::vector<uint8_t> Strings(const std::vector<std::pair<std::string, std::string>>& kv) {
    xom::Document d;
    d.schmRec = {1, 0, 0};
    d.strings = {""};
    for (const char* c : {"XResourceDetails", "XStringResourceDetails", "XDataBank", "XContainer"}) xu::EnsureType(d, c, "");
    xom::Object bank;
    xu::NewObject(d, "XDataBank", &bank, nullptr);
    for (const auto& [k, v] : kv) {
        xom::Object o;
        xu::NewObject(d, "XStringResourceDetails", &o, nullptr);
        xu::SetStr(o, "Name", k);
        xu::SetStr(o, "Value", v);
        d.objects.push_back(std::move(o));
        bank.field("StringResources")->items.push_back(xu::RefValue(static_cast<uint32_t>(d.objects.size())));
    }
    d.objects.push_back(std::move(bank));
    d.root = static_cast<uint32_t>(d.objects.size());
    return Serialize(d);
}

std::vector<uint8_t> Desc(const std::string& theme, const std::string& tod, const std::string& mat, bool heightmap = true,
                          uint32_t ctb = 0) {
    imp::DescriptorOut d;
    d.theme = theme;
    d.timeOfDay = tod;
    d.materialFile = mat;
    if (heightmap) d.heightmapBase = "B01", d.heightmapSecond = "B13";
    d.customTextureBank = ctb;
    d.customDetailBank = ctb ? 1 : 0;
    std::vector<uint8_t> out;
    imp::BuildDescriptor(d, &out, nullptr);
    return out;
}

std::vector<uint8_t> Xan(int version, const std::string& tag) {
    std::vector<uint8_t> b = {'M', 'O', 'I', 'K', 0, 0, 0, static_cast<uint8_t>(version)};
    b.resize(64, 0);
    b.insert(b.end(), tag.begin(), tag.end());
    return b;
}

std::vector<uint8_t> Tga(int w, int h) {
    std::vector<uint8_t> b(18, 0);
    b[2] = 2;
    b[12] = static_cast<uint8_t>(w), b[13] = static_cast<uint8_t>(w >> 8), b[14] = static_cast<uint8_t>(h), b[15] = static_cast<uint8_t>(h >> 8);
    b[16] = 24;
    uint32_t x = 12345;
    for (int i = 0; i < w * h * 3; ++i) {
        x = x * 1103515245u + 12345u;
        b.push_back(static_cast<uint8_t>(x >> 16));
    }
    return b;
}

// ---------------------------------------------------------------- the fixture zip
struct ZipEntry {
    std::string name;
    std::vector<uint8_t> data;
    bool stored = false;
};

bool WriteZip(const std::wstring& path, const std::vector<ZipEntry>& entries) {
    mz_zip_archive z{};
    if (!mz_zip_writer_init_heap(&z, 0, 0)) return false;
    MZ_TIME_T t = 1700000000;
    for (const auto& e : entries)
        if (!mz_zip_writer_add_mem_ex_v2(&z, e.name.c_str(), e.data.data(), e.data.size(), nullptr, 0, e.stored ? 0 : 6, 0, 0, &t,
                                         nullptr, 0, nullptr, 0))
            return false;
    void* buf = nullptr;
    size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&z, &buf, &size)) return false;
    const bool ok = WriteFileBytes(path, std::vector<uint8_t>(static_cast<uint8_t*>(buf), static_cast<uint8_t*>(buf) + size));
    mz_zip_writer_end(&z);
    return ok;
}

const std::string kRoot = "Fix_0.1/Data/";
const std::string kLong = "AVeryLongFileNameThatNeedsHashing_42";

std::vector<ZipEntry> FixtureEntries() {
    std::vector<Reg> regs = {
        {"Re.Alpha_One", "Alpha_One", "txt.Alpha", "Alpha_One.tga", "stdvs,wormpot,DM"},
        {"Re.Alpha_One.Dup", "Alpha_One", "txt.Other", "", "stdvs,wormpot,DM"},
        {"Re.Alpha_One.S", "Alpha_One", "txt.Alpha", "", "Survivor,SV"},
        {"Re.W3D_Beta", "W3D_Beta", "txt.Beta", "", "DM, stdvs, wormpot"},
        {"Re.Gamma", "Gamma", "txt.Missing", "", "stdvs,wormpot,DM"},
        {"Re.Delta", "re_delta", "txt.Delta", "", "renew,renewWLoad,renew_stdvs,wormpot"},
        {"Re.Eps", "re_epsilon", "txt.Eps", "", "renew,RR,wormpot"},
        {"Re.Long", kLong, "txt.Long", "", "stdvs,wormpot,DM"},
        {"Re.User", "usermap", "txt.User", "", "stdvs,wormpot,DM"},
        {"Re.Ghost", "ghost", "txt.Ghost", "", "stdvs,wormpot,DM"},
        {"Re.Van", "vanmap", "txt.Van", "", "stdvs,wormpot,DM"},
        {"Vanilla.Multi", "Alpha_One", "txt.Alpha", "", "stdvs,wormpot", 0},
    };
    std::vector<ZipEntry> z;
    z.push_back({kRoot + "Tweak/SCRIPTS.XOM", Registry(regs)});
    z.push_back({kRoot + "Tweak/LOCAL.XOM", Strings({{"Tweak", "x"}})});
    z.push_back({kRoot + "Language/PC/English.xom",
                 Strings({{"txt.Alpha", "Alpha One"}, {"txt.Beta", "W3D Beta \x01Port"},
                          {"txt.Long", "A title that is much longer than forty characters in all"},
                          {"txt.Delta", "[DM] Delta"}, {"txt.Eps", "[RR] Epsilon"}, {"txt.Van", "Vanilla Map"}})});
    z.push_back({kRoot + "Language/PC/EngFE.xom", Strings({{"txt.Alpha", "Shadowed"}, {"txt.User", "User"}})});
    z.push_back({kRoot + "Alpha_One.XOM", Desc("PIRATE", "NIGHT", "Maps\\Alpha_One.txt", true, 6)});
    z.push_back({kRoot + "Maps/Alpha_One.xan", Xan(1, "alpha")});
    z.push_back({kRoot + "Maps/Alpha_One.txt", B("alpha textures\n")});
    z.push_back({kRoot + "Maps/Alpha_One.hmp", std::vector<uint8_t>(300, 7)});
    z.push_back({kRoot + "Frontend/Levels/Alpha_One.tga", Tga(512, 256)});
    z.push_back({kRoot + "W3D_Beta.XOM", Desc("LUNAR", "evening", "Maps\\Alpha_One.txt")});
    z.push_back({kRoot + "Maps/W3D_Beta.xan", Xan(2, "beta"), true});
    z.push_back({kRoot + "Gamma.XOM", Desc("ARCTIC", "Vera34", "Maps\\vanillatheme.txt", false)});
    z.push_back({kRoot + "Maps/Gamma.xan", Xan(1, "gamma")});
    z.push_back({kRoot + "re_delta.XOM", Desc("WAR", "DAY", "Maps\\re_delta.txt")});
    z.push_back({kRoot + "Maps/re_delta.xan", Xan(1, "delta")});
    z.push_back({kRoot + "Maps/re_delta.txt", B("delta textures\n")});
    z.push_back({kRoot + "re_epsilon.XOM", Desc("HORROR", "NIGHT", "Maps\\re_epsilon.txt")});
    z.push_back({kRoot + "Maps/re_epsilon.xan", Xan(1, "epsilon")});
    z.push_back({kRoot + kLong + ".XOM", Desc("ENGLAND", "DAY", "Maps\\" + kLong + ".txt")});
    z.push_back({kRoot + "Maps/" + kLong + ".xan", Xan(1, "long")});
    z.push_back({kRoot + "usermap.XOM", Desc("BUILDING", "DAY", "")});
    z.push_back({kRoot + "Maps/usermap.xan", Xan(1, "user")});
    z.push_back({kRoot + "Maps/Orphan.xan", Xan(1, "orphan")});
    // Decoys: never read, never written.
    z.push_back({"Fix_0.1/dinput8.dll", B("MZ decoy loader")});
    z.push_back({"Fix_0.1/plugins/patch.asi", B("MZ decoy patch")});
    z.push_back({kRoot + "Bundles/Bundl00.xom", B("bundle")});
    z.push_back({kRoot + "scripts/x.lub", B("\x1bLua")});
    z.push_back({"Fix_0.1/../escape.txt", B("x")});
    return z;
}

struct Fixture {
    std::wstring root, zip, game;
    std::string sha;
    uint64_t size = 0;
    std::string vanXom, vanXan;
};

std::string RecipeJson(const Fixture& f, const std::string& extra = "", int expectMaps = 7) {
    return std::string("{\n") + R"(  "importVersion": 1, "format": 1, "id": "fixture-0.1", "name": "Fixture maps",
  "content": {"title": "Fixture", "publisher": "mod.worms.pro", "termsUrl": "https://mod.worms.pro/", "credit": "Test credit."},
  "sources": [{"id": "mirror", "name": "mod.worms.pro", "urls": ["https://mod.worms.pro/resources/Fix.zip"],
               "fileName": "Fix_0.1.zip", "size": )" +
           std::to_string(f.size) + R"(, "sha256": ")" + f.sha + R"("}],
  "reader": {"type": "w4-registry", "root": "Fix_0.1/Data", "registry": "Tweak/SCRIPTS.XOM",
             "titles": ["Language/PC/English.xom", "Language/PC/EngFE.xom"], "descriptors": "*.XOM",
             "maps": ["Maps/*.xan", "Maps/*.txt", "Maps/*.hmp"], "previews": "Frontend/Levels/*.tga"},
  "select": {"levelType": 3, "skipKeySuffix": ".S", "require": ["descriptor", "xan"], "exclude": ["usermap"],
             "vanilla": [{"file": "vanmap", "sha256": {"vanmap.XOM": ")" +
           f.vanXom + R"(", "Maps/vanmap.xan": ")" + f.vanXan + R"("}}],
             "expect": {"maps": )" + std::to_string(expectMaps) + R"(, "fromArchive": )" + std::to_string(expectMaps - 1) +
           R"(, "fromGame": 1}},
  "categories": [{"id": "play", "label": "Plays as designed", "scriptsEqual": ["stdvs", "wormpot", "DM"]},
                 {"id": "dm", "label": "Deathmatch only", "scriptsWithin": ["renew*", "wormpot"]},
                 {"id": "mode", "label": "Mode not supported", "default": true, "hidden": true}],
  "modes": {"PRO": "Pro", "RR": "Rope race"},
  "groups": [{"id": "w3d", "label": "Worms 3D ports", "match": ["W3D_*", "*-w3d"]},
             {"id": "renewation", "label": "Renewation", "match": ["re_*"]},
             {"id": "vanilla", "label": "Standard maps", "vanilla": true},
             {"id": "mmp", "label": "Mega Map Pack", "default": true}],
  "transform": {"stem": {"slugMax": 24, "hashKeep": 18, "hashHex": 6}, "descriptor": "rebuild",
                "timeOfDay": {"allowed": ["DAY", "EVENING", "NIGHT"], "fallback": "DAY"}, "title": {"max": 40, "fallback": "fileName"},
                "author": "Databank.MapAuthor", "textures": "vanilla", "scripts": "drop", "survivor": true, "previews": "local"},
  "output": {"packPrefix": "caravan", "version": "1.0.0", "perPack": )" +
           (extra.empty() ? "4" : extra) + R"(, "order": ["category", "fileName"], "newPackBefore": ["mode"],
             "packName": "Caravan maps {n}", "packDescription": "Maps imported on this PC by the test."}
}
)";
}

bool MakeGame(const std::wstring& game, Fixture* f) {
    MakeDir(game);
    MakeDir(game + L"\\Data");
    MakeDir(game + L"\\Data\\Maps");
    MakeDir(game + L"\\Mods");
    const auto xomB = Desc("CAMELOT", "DAY", "Maps\\vanmap.txt");
    const auto xanB = Xan(2, "vanilla");
    f->vanXom = melange::hashutil::Sha256Hex(xomB.data(), xomB.size());
    f->vanXan = melange::hashutil::Sha256Hex(xanB.data(), xanB.size());
    return WriteFileBytes(game + L"\\Data\\vanmap.XOM", xomB) && WriteFileBytes(game + L"\\Data\\Maps\\vanmap.xan", xanB);
}

void InstallPlugin(const std::wstring& game, const std::string& recipe) {
    const std::wstring dir = game + L"\\Mods\\caravan";
    MakeDir(dir);
    WriteFileBytes(dir + L"\\spice.json", B(R"({"spiceVersion": 1, "id": "caravan", "version": "1.0.0", "name": "Caravan",
"authors": ["Melange"], "kind": "client-only", "melange": {"range": ">=0.3.3 <0.4.0"}, "importer": {"recipe": "import.json"}}
)"));
    WriteFileBytes(dir + L"\\import.json", B(recipe));
}

imp::RunSpec Spec(const std::wstring& game, const std::wstring& zip, imp::RunSpec::Kind kind = imp::RunSpec::Kind::File) {
    imp::RunSpec s;
    s.game = game;
    std::string err;
    if (!imp::LoadPlugin(game, "caravan", &s.plugin, &err)) printf("LoadPlugin: %s\n", err.c_str());
    s.kind = kind;
    s.sourceId = "mirror";
    s.file = zip;
    if (kind == imp::RunSpec::Kind::Download) {
        std::string u = "file:///" + N(zip);
        for (char& c : u)
            if (c == '\\') c = '/';
        s.urls = {u};
    }
    return s;
}

// ---------------------------------------------------------------- cases
void TestRecipe(const Fixture& f) {
    imp::Recipe r;
    std::string err;
    Expect(imp::ParseRecipe(RecipeJson(f), "caravan", &r, &err), "fixture recipe parses: " + err);
    Expect(r.categories.size() == 3 && r.modes.size() == 2 && r.groups.size() == 4 && r.select.vanilla.size() == 1, "recipe lists");
    Expect(!imp::ParseRecipe(RecipeJson(f), "other", &r, &err) && err.find("packPrefix") != std::string::npos, "prefix must be the plugin id");
    auto bad = [&](const std::string& from, const std::string& to, const std::string& what) {
        std::string t = RecipeJson(f);
        const size_t p = t.find(from);
        if (p == std::string::npos) return Expect(false, "fixture text for " + what);
        t.replace(p, from.size(), to);
        std::string e;
        imp::Recipe rr;
        Expect(!imp::ParseRecipe(t, "caravan", &rr, &e), what + " is refused");
    };
    bad("https://mod.worms.pro/resources", "http://mod.worms.pro/resources", "an http URL");
    bad("https://mod.worms.pro/resources", "https://evil.example/resources", "an unknown host");
    bad("https://mod.worms.pro/resources", "https://user@mod.worms.pro/resources", "userinfo in the URL");
    bad("https://mod.worms.pro/resources", "https://mod.worms.pro:8443/resources", "a port other than 443");
    bad("\"sha256\": \"" + f.sha, "\"sha256\": \"" + f.sha.substr(1), "a short sha");
    bad("\"textures\": \"vanilla\"", "\"textures\": \"zip\"", "textures other than vanilla");
    bad("\"scripts\": \"drop\"", "\"scripts\": \"keep\"", "scripts other than drop");
    bad("\"name\": \"Fixture maps\"", "\"name\": \"Fixture maps\", \"extra\": 1", "an unknown key");
    bad("\"root\": \"Fix_0.1/Data\"", "\"root\": \"../Data\"", "a '..' reader path");
    bad("\"root\": \"Fix_0.1/Data\"", "\"root\": \"C:/Data\"", "a drive in a reader path");
    bad("\"mode\", \"label\": \"Mode not supported\", \"default\": true", "\"mode\", \"label\": \"Mode not supported\"", "no default category");
    bad("\"perPack\": 4", "\"perPack\": 33", "perPack above 32");
    bad("\"fromGame\": 1}", "\"fromGame\": 2}", "expect counts that do not add up");
    bad("\"Maps/vanmap.xan\"", "\"../vanmap.xan\"", "a vanilla hash outside Data");
    bad("\"require\": [\"descriptor\", \"xan\"]", "\"require\": [\"xan\"]", "a partial require list");
    bool unsupported = false;
    std::string t = RecipeJson(f);
    t.replace(t.find("\"format\": 1"), 11, "\"format\": 2");
    Expect(!imp::ParseRecipe(t, "caravan", &r, &err, &unsupported) && unsupported, "an unknown format is reported unsupported");
    Expect(imp::HostOf("https://mod.worms.pro/x.zip") == "mod.worms.pro" && imp::HostOf("https://mod.worms.pro/x?token=1").empty(),
           "HostOf refuses credentials in the query");
    Expect(imp::GlobMatch("W3D_*", "w3d_atlantis") && imp::GlobMatch("*-w3d", "ShotgunChallenge1-w3d") &&
               !imp::GlobMatch("Maps/*.xan", "Maps/sub/x.xan") && imp::GlobMatch("Boss*", "Boss3"),
           "glob rules");
}

void TestPlanPure(const Fixture& f) {
    imp::Recipe r;
    std::string err;
    imp::ParseRecipe(RecipeJson(f), "caravan", &r, &err);
    Expect(imp::Slug(r, "Lucas_TwoTower(P)") == "lucastwotowerp", "slug keeps a-z0-9");
    const std::string h = melange::hashutil::Sha256Hex(kLong.data(), kLong.size());
    Expect(imp::Slug(r, kLong) == std::string("averylongfilenamet") + h.substr(0, 6), "a long slug is cut and hashed");
    Expect(imp::Categorize(r, {"DM", "stdvs", "wormpot"}) == 0, "scriptsEqual is order-insensitive");
    Expect(imp::Categorize(r, {"renew", "renewWLoad", "wormpot"}) == 1, "scriptsWithin");
    Expect(imp::Categorize(r, {"stdvs", "wormpot", "PRO"}) == 2 && imp::ModeOf(r, {"stdvs", "PRO"}) == "Pro", "default category and mode");
    Expect(imp::GroupOf(r, "W3D_Atlantis", false) == 0 && imp::GroupOf(r, "re_x", false) == 1 && imp::GroupOf(r, "balloon", true) == 2 &&
               imp::GroupOf(r, "GX_LS", false) == 3,
           "groups");
    // 70 play maps and 5 mode maps: packs of 32, a new pack before the mode maps.
    imp::Selection s;
    for (int i = 0; i < 70; ++i) s.maps.push_back({{"k" + std::to_string(i), "Map" + std::to_string(100 + i), "", "", {"stdvs", "wormpot", "DM"}, 3}, false});
    for (int i = 0; i < 5; ++i) s.maps.push_back({{"m" + std::to_string(i), "re_mode" + std::to_string(i), "", "", {"renew", "RR"}, 3}, false});
    r.output.perPack = 32;
    std::vector<imp::Planned> plan;
    Expect(imp::PlanPacks(r, s, &plan, &err) && plan.size() == 75, "plan 75 maps: " + err);
    std::map<int, int> per;
    for (const auto& p : plan) per[p.pack]++;
    Expect(per.size() == 4 && per[1] == 32 && per[2] == 32 && per[3] == 6 && per[4] == 5, "split at 32 and before the mode category");
    Expect(plan.front().stem == "caravan_1_map100" && plan.back().packId == "caravan-4", "stems and pack ids");
    s.maps.push_back({{"dup", "MAP100", "", "", {"stdvs", "wormpot", "DM"}, 3}, false});
    Expect(!imp::PlanPacks(r, s, &plan, &err) && err.find("used twice") != std::string::npos, "a stem collision fails");
    imp::Selection many;
    for (int i = 0; i < 300; ++i) many.maps.push_back({{"k", "M" + std::to_string(i), "", "", {"stdvs", "wormpot", "DM"}, 3}, false});
    Expect(!imp::PlanPacks(r, many, &plan, &err), "more than 9 packs fails");
    Expect(imp::CleanTitle("  A\x01" "B  ", 40) == "AB" && imp::CleanTitle(std::string(50, 'x'), 40).size() == 40, "titles are cleaned and clipped");
    Expect(imp::NormalTimeOfDay(r, "night") == "NIGHT" && imp::NormalTimeOfDay(r, "Vera34") == "DAY", "time of day");
}

void TestDescriptorGolden() {
    imp::DescriptorOut d;
    d.theme = "PIRATE";
    d.timeOfDay = "NIGHT";
    d.materialFile = "Maps\\caravan_1_x.txt";
    d.heightmapBase = "B01";
    d.heightmapSecond = "B13";
    d.customTextureBank = 6;
    d.customDetailBank = 1;
    std::vector<uint8_t> a, b;
    std::string berr;
    const bool built = imp::BuildDescriptor(d, &a, &berr) && imp::BuildDescriptor(d, &b, &berr);
    Expect(built && a == b, "descriptor bytes are stable: " + berr);
    const std::string sha = melange::hashutil::Sha256Hex(a.data(), a.size());
    Expect(sha == "1484fdd2f9b2b47aafe0d2b8b2395c1d9a62ca82440a677ace9a09398d6955ab", "descriptor golden bytes (" + sha + ")");
    imp::Descriptor back;
    std::string err;
    Expect(imp::ReadDescriptor(a, "", &back, &err) && back.theme == "PIRATE" && back.timeOfDay == "NIGHT" && back.materialFile == d.materialFile &&
               back.heightmapBase == "B01" && back.customTextureBank == 6 && back.customDetailBank == 1,
           "the descriptor reads back: " + err);
    d.heightmapBase.reset();
    d.heightmapSecond.reset();
    imp::BuildDescriptor(d, &a, nullptr);
    Expect(imp::ReadDescriptor(a, "", &back, &err) && !back.heightmapBase && !back.heightmapSecond, "heightmap keys only when present");
}

void TestXomV1() {
    const std::vector<uint8_t> v2 = Desc("LUNAR", "DAY", "Maps\\x.txt");
    xom::Document d;
    std::string err;
    xom::parse(v2.data(), v2.size(), d, &err);
    const size_t guidAt = 0x40 + d.types.size() * 0x40;
    std::vector<uint8_t> v1 = v2;
    v1.erase(v1.begin() + static_cast<long>(guidAt), v1.begin() + static_cast<long>(guidAt) + 16);
    v1[7] = 1;
    xom::Document d1;
    Expect(xom::parse(v1.data(), v1.size(), d1, &err) && !d1.guidRecord && d1.objects.size() == d.objects.size(), "a v1 header parses: " + err);
    std::vector<uint8_t> back;
    Expect(xom::serialize(d1, back, &err) && back == v1, "a v1 file round-trips byte for byte");
    Expect(xom::parse(v2.data(), v2.size(), d, &err) && d.guidRecord, "a v2 file keeps its GUID record");
}

void TestHidden() {
    Expect(hidden::StemOfKey("Multi.caravan_1_abc.S") == "caravan_1_abc" && hidden::StemOfKey("Multi.caravan_1_abc") == "caravan_1_abc" &&
               hidden::StemOfKey("Other.x").empty() && hidden::StemOfKey("Multi.Bad.Stem").empty(),
           "stems of bank keys");
    const auto s = hidden::Parse("caravan_1_a\r\nBAD STEM\n\ncaravan_2_b\n");
    Expect(s.size() == 2 && s.count("caravan_1_a") && hidden::Serialize(s) == "caravan_1_a\ncaravan_2_b\n", "hidden-levels.txt format");
}

void TestSpiceKeys(const std::wstring& tmp) {
    auto parse = [&](const std::string& id, const std::string& body, melange::spice::Manifest* m) {
        MakeDir(tmp + L"\\spice");
        const std::wstring dir = tmp + L"\\spice\\" + W(id);
        MakeDir(dir);
        WriteFileBytes(dir + L"\\spice.json", B("{\"spiceVersion\": 1, \"id\": \"" + id + "\", \"version\": \"1.0.0\", \"name\": \"X\", " +
                                                 "\"melange\": {\"range\": \">=0.3.3\"}, " + body + "}"));
        std::vector<melange::spice::Error> errs;
        return melange::spice::Parse(dir, m, &errs) && !m->implicit;
    };
    melange::spice::Manifest m;
    Expect(parse("caravan", "\"kind\": \"client-only\", \"importer\": {\"recipe\": \"import.json\"}", &m) && m.importerRecipe == "import.json",
           "importer key");
    Expect(!parse("caravan", "\"kind\": \"client-only\", \"importer\": {\"recipe\": \"../x.json\"}", &m), "importer path with ..");
    Expect(!parse("caravan", "\"kind\": \"client-only\", \"importer\": {\"recipe\": \"x.lua\"}", &m), "importer path not .json");
    Expect(parse("caravan-2", "\"kind\": \"content\", \"generated\": {\"by\": \"caravan\", \"recipe\": \"r-1\", \"format\": 1}", &m) &&
               m.generatedBy == "caravan" && m.generatedFormat == 1,
           "generated key");
    Expect(!parse("other-2", "\"kind\": \"content\", \"generated\": {\"by\": \"caravan\", \"recipe\": \"r-1\", \"format\": 1}", &m),
           "generated in a folder that is not <by>-<n>");
    Expect(!parse("caravan-2", "\"kind\": \"content\", \"generated\": {\"by\": \"caravan\", \"recipe\": \"r-1\"}", &m), "generated without format");
}

void TestArchiveChecks(const std::wstring& tmp, const imp::Reader& rd) {
    std::string why;
    Expect(imp::CheckMemberName("Fix/Data/Lucas_TwoTower(P).XOM", &why), "parentheses are allowed");
    for (const char* bad : {"../x.XOM", "/abs.XOM", "C:/x.XOM", "Fix/Data/.hidden.XOM", "Fix\\Data\\x.XOM", "Fix/Data/con.XOM", "Fix/Data/x.dll"})
        Expect(!imp::CheckMemberName(bad, &why), std::string("member name refused: ") + bad);
    Expect(!imp::CheckMemberName(std::string(200, 'a'), &why), "an over-long member name");
    auto opens = [&](const std::vector<ZipEntry>& entries, const char*) {
        const std::wstring z = tmp + L"\\chk.zip";
        WriteZip(z, entries);
        imp::Archive a;
        std::string e;
        return a.Open(z, rd, &e);
    };
    const std::string reg = kRoot + "Tweak/SCRIPTS.XOM";
    Expect(!opens({{kRoot + "Maps/a.xan", Xan(1, "a")}, {kRoot + "Maps/A.XAN", Xan(1, "b")}}, "dup"), "duplicate names (case-insensitive) fail");
    Expect(!opens({{kRoot + "Maps/big.xan", std::vector<uint8_t>(5u << 20, 0)}}, "cap"), "a member above its type cap fails");
    Expect(!opens({{kRoot + "Maps/bomb.txt", std::vector<uint8_t>(60000, 0)}, {kRoot + "Maps/bomb.hmp", std::vector<uint8_t>(200000, 0)}}, "ratio"),
           "a ratio bomb fails");
    Expect(opens({{kRoot + "../../x.xan", Xan(1, "x")}, {"dinput8.dll", B("MZ")}}, "outside"), "members outside the allowlist are ignored");
    {
        const std::wstring z = tmp + L"\\mz.zip";
        WriteZip(z, {{kRoot + "Maps/evil.xan", B("MZ\x90\x00 pretending")}});
        imp::Archive a;
        std::string e;
        std::vector<uint8_t> out;
        Expect(a.Open(z, rd, &e) && a.Find("Maps/evil.xan") && !a.Read(*a.Find("Maps/evil.xan"), &out, &e) &&
                   e.find("executable") != std::string::npos,
               "executable magic in an allowlisted member fails");
    }
    {
        // Method 12 (bzip2) and the encrypted flag, patched into a stored entry's headers.
        const std::wstring z = tmp + L"\\m12.zip";
        WriteZip(z, {{kRoot + "Maps/m.txt", B("hello"), true}});
        std::vector<uint8_t> b = ReadFileBytes(z);
        auto patch = [&](uint16_t method, uint16_t flags) {
            std::vector<uint8_t> c = b;
            for (size_t i = 0; i + 4 < c.size(); ++i) {
                const bool local = c[i] == 'P' && c[i + 1] == 'K' && c[i + 2] == 3 && c[i + 3] == 4;
                const bool central = c[i] == 'P' && c[i + 1] == 'K' && c[i + 2] == 1 && c[i + 3] == 2;
                const size_t off = local ? 6 : central ? 8 : 0;
                if (!off) continue;
                c[i + off] = static_cast<uint8_t>(flags), c[i + off + 1] = static_cast<uint8_t>(flags >> 8);
                c[i + off + 2] = static_cast<uint8_t>(method), c[i + off + 3] = static_cast<uint8_t>(method >> 8);
            }
            WriteFileBytes(z, c);
            imp::Archive a;
            std::string e;
            return a.Open(z, rd, &e);
        };
        Expect(!patch(12, 0), "compression method 12 fails");
        Expect(!patch(0, 1), "an encrypted entry fails");
    }
    {
        // A deflated member whose declared size is smaller than what it inflates to.
        const std::wstring z = tmp + L"\\inf.zip";
        WriteZip(z, {{kRoot + "Maps/i.txt", std::vector<uint8_t>(4000, 'a')}});
        std::vector<uint8_t> b = ReadFileBytes(z);
        for (size_t i = 0; i + 30 < b.size(); ++i) {
            const bool local = b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 3 && b[i + 3] == 4;
            const bool central = b[i] == 'P' && b[i + 1] == 'K' && b[i + 2] == 1 && b[i + 3] == 2;
            const size_t off = local ? 22 : central ? 24 : 0;
            if (off) b[i + off] = 100, b[i + off + 1] = 0, b[i + off + 2] = 0, b[i + off + 3] = 0;
        }
        WriteFileBytes(z, b);
        imp::Archive a;
        std::string e;
        std::vector<uint8_t> out;
        Expect(a.Open(z, rd, &e) && a.Find("Maps/i.txt") && !a.Read(*a.Find("Maps/i.txt"), &out, &e), "inflating past the declared size fails");
    }
    (void)reg;
}

std::map<std::string, std::string> PackTree(const std::wstring& game) {
    std::map<std::string, std::string> t;
    for (int n = 1; n <= 9; ++n) {
        const std::wstring d = game + L"\\Mods\\caravan-" + std::to_wstring(n);
        if (Exists(d)) Tree(d, "caravan-" + std::to_string(n) + "/", &t);
    }
    return t;
}

void TestImport(const std::wstring& tmp, Fixture& f) {
    const std::wstring g1 = tmp + L"\\game1", g2 = tmp + L"\\game2";
    MakeGame(g1, &f);
    MakeGame(g2, &f);
    InstallPlugin(g1, RecipeJson(f));
    InstallPlugin(g2, RecipeJson(f));
    const std::wstring work1 = g1 + L"\\Mods\\.import\\caravan";

    // Hash refusal: one byte changed, then one byte too many.
    {
        std::vector<uint8_t> b = ReadFileBytes(f.zip);
        b[b.size() / 2] ^= 1;
        const std::wstring bad = tmp + L"\\bad.zip";
        WriteFileBytes(bad, b);
        imp::RunResult r = imp::Run(Spec(g1, bad));
        Expect(!r.ok && r.reason == "hash", "a changed byte is refused before reading (" + r.reason + ": " + r.message + ")");
        b[b.size() / 2] ^= 1;
        b.push_back(0);
        WriteFileBytes(bad, b);
        r = imp::Run(Spec(g1, bad, imp::RunSpec::Kind::Download));
        Expect(!r.ok && r.reason == "hash", "a download one byte too long is refused (" + r.reason + ": " + r.message + ")");
        std::map<std::string, std::string> t;
        Tree(work1, "", &t);
        bool onlyDirs = true;
        for (const auto& [k, _] : t) onlyDirs = false, printf("  left: %s\n", k.c_str());
        Expect(onlyDirs && PackTree(g1).empty(), "nothing is written after a hash refusal");
    }

    // Cancel during the transfer and during building: nothing placed.
    {
        std::atomic<bool> cancel{false};
        imp::RunSpec s = Spec(g1, f.zip, imp::RunSpec::Kind::Download);
        s.cancel = &cancel;
        s.progress = [&](const imp::Progress& p) {
            if (p.phase == "downloading") cancel = true;
        };
        imp::RunResult r = imp::Run(s);
        Expect(!r.ok && r.reason == "cancelled" && !Exists(work1 + L"\\dl\\Fix_0.1.zip") && !Exists(work1 + L"\\dl\\Fix_0.1.zip.part"),
               "cancel during the download (" + r.reason + ")");
        cancel = false;
        s.progress = [&](const imp::Progress& p) {
            if (p.phase == "building" && p.step == 2) cancel = true;
        };
        r = imp::Run(s);
        Expect(!r.ok && r.reason == "cancelled" && PackTree(g1).empty(), "cancel while building places nothing (" + r.reason + ")");
    }

    // A real import from a local file into game1 and a download into game2.
    imp::RunSpec s1 = Spec(g1, f.zip);
    std::vector<imp::Progress> seen;
    s1.progress = [&](const imp::Progress& p) { seen.push_back(p); };
    imp::RunResult r1 = imp::Run(s1);
    Expect(r1.ok, "import from a local zip (" + r1.reason + ": " + r1.message + ")");
    imp::RunResult r2 = imp::Run(Spec(g2, f.zip, imp::RunSpec::Kind::Download));
    Expect(r2.ok, "import by download (" + r2.reason + ": " + r2.message + ")");
    Expect(r1.maps == 7 && r1.skipped == 2 && r1.counts["play"] == 5 && r1.counts["dm"] == 1 && r1.counts["mode"] == 1,
           "counts: 7 maps, 2 skipped, 5/1/1 (" + std::to_string(r1.maps) + ", " + std::to_string(r1.skipped) + ")");
    Expect(r1.packs == std::vector<std::string>({"caravan-1", "caravan-2", "caravan-3"}), "packs: 4 play, 1 play + 1 dm, 1 mode");
    const auto t1 = PackTree(g1), t2 = PackTree(g2);
    Expect(!t1.empty() && t1 == t2 && r1.fingerprint == r2.fingerprint, "two imports give identical packs and fingerprints");
    Expect(r1.fingerprint == "b8e552ff8de65e778430ac07bdd2dd3bdaba1f550fe8b159573b36931991b280", "fixture fingerprint (" + r1.fingerprint + ")");

    const std::string longStem = "caravan_1_" + imp::Slug(s1.plugin.recipe, kLong);
    const std::set<std::string> want = {
        "caravan-1/spice.json",
        "caravan-1/assets/levels/caravan_1_alphaone.XOM", "caravan-1/assets/levels/Maps/caravan_1_alphaone.xan",
        "caravan-1/assets/levels/Maps/caravan_1_alphaone.txt", "caravan-1/assets/levels/Maps/caravan_1_alphaone.hmp",
        "caravan-1/assets/levels/" + longStem + ".XOM", "caravan-1/assets/levels/Maps/" + longStem + ".xan",
        "caravan-1/assets/levels/caravan_1_gamma.XOM", "caravan-1/assets/levels/Maps/caravan_1_gamma.xan",
        "caravan-1/assets/levels/caravan_1_vanmap.XOM", "caravan-1/assets/levels/Maps/caravan_1_vanmap.xan",
        "caravan-2/spice.json",
        "caravan-2/assets/levels/caravan_2_w3dbeta.XOM", "caravan-2/assets/levels/Maps/caravan_2_w3dbeta.xan",
        "caravan-2/assets/levels/Maps/caravan_2_w3dbeta.txt",
        "caravan-2/assets/levels/caravan_2_redelta.XOM", "caravan-2/assets/levels/Maps/caravan_2_redelta.xan",
        "caravan-2/assets/levels/Maps/caravan_2_redelta.txt",
        "caravan-3/spice.json",
        "caravan-3/assets/levels/caravan_3_reepsilon.XOM", "caravan-3/assets/levels/Maps/caravan_3_reepsilon.xan"};
    std::set<std::string> have;
    for (const auto& [k, _] : t1) have.insert(k);
    Expect(have == want, "the packs hold exactly the expected files (no decoys, no orphan)");
    for (const auto& k : have)
        if (!want.count(k)) printf("  unexpected: %s\n", k.c_str());
    for (const auto& k : want)
        if (!have.count(k)) printf("  missing: %s\n", k.c_str());

    const std::wstring lv1 = g1 + L"\\Mods\\caravan-1\\assets\\levels\\";
    Expect(ReadFileBytes(lv1 + L"Maps\\caravan_1_alphaone.xan") == Xan(1, "alpha"), ".xan copied byte for byte (v1 header kept)");
    Expect(ReadFileBytes(g1 + L"\\Mods\\caravan-2\\assets\\levels\\Maps\\caravan_2_w3dbeta.txt") == B("alpha textures\n"),
           "a shared .txt is copied under the map's stem");
    imp::Descriptor d;
    std::string err;
    imp::ReadDescriptor(ReadFileBytes(lv1 + L"caravan_1_alphaone.XOM"), "", &d, &err);
    Expect(d.materialFile == "Maps\\caravan_1_alphaone.txt" && d.theme == "PIRATE" && d.customTextureBank == 6, "own .txt rewritten");
    imp::ReadDescriptor(ReadFileBytes(lv1 + L"caravan_1_gamma.XOM"), "", &d, &err);
    Expect(d.materialFile == "Maps\\vanillatheme.txt" && d.timeOfDay == "DAY" && !d.heightmapBase, "a vanilla .txt stays, bogus TOD -> DAY");
    imp::ReadDescriptor(ReadFileBytes(lv1 + L"caravan_1_vanmap.XOM"), "", &d, &err);
    Expect(d.materialFile == "Maps\\vanmap.txt" && d.theme == "CAMELOT", "a vanilla-file map keeps its vanilla .txt");
    imp::ReadDescriptor(ReadFileBytes(g1 + L"\\Mods\\caravan-2\\assets\\levels\\caravan_2_w3dbeta.XOM"), "", &d, &err);
    Expect(d.timeOfDay == "EVENING", "time of day normalised to upper case");

    melange::spice::Manifest m;
    std::vector<melange::spice::Error> errs;
    Expect(melange::spice::Parse(g1 + L"\\Mods\\caravan-1", &m, &errs) && m.generatedBy == "caravan" && m.levels.size() == 4 &&
               m.melangeRange == ">=0.3.3 <0.4.0" && m.version == "1.0.0" && m.content,
           "generated spice.json parses");
    std::map<std::string, std::string> titles;
    for (const auto& l : m.levels) titles[l.slug] = l.title;
    Expect(titles["alphaone"] == "Alpha One" && titles["gamma"] == "Gamma" && titles["vanmap"] == "Vanilla Map" &&
               titles[imp::Slug(s1.plugin.recipe, kLong)].size() == 40,
           "titles: English bank first, file name fallback, clipped to 40");
    const auto spice = ReadFileBytes(g1 + L"\\Mods\\caravan-3\\spice.json");
    Expect(!spice.empty() && spice.back() == '\n' && std::string(spice.begin(), spice.end()).find('\r') == std::string::npos &&
               std::string(spice.begin(), spice.end()).find("\"generated\": { \"by\": \"caravan\"") != std::string::npos,
           "spice.json: LF, trailing newline, generated key");

    Expect(Exists(work1 + L"\\previews\\caravan_1_alphaone.png") && Exists(work1 + L"\\maps.json") && Exists(work1 + L"\\state.json"),
           "preview, catalogue and state are local");
    Expect(!Exists(g1 + L"\\Mods\\caravan-1\\assets\\levels\\caravan_1_alphaone.png"), "no preview in a pack");
    const auto hid = hidden::Load(g1);
    Expect(hid.size() == 1 && hid.count("caravan_3_reepsilon"), "mode maps are hidden on first import");
    bool phases = false;
    for (const auto& p : seen) phases |= p.phase == "building" && p.of == 7;
    Expect(phases, "progress reports building step/of");

    // Hide a map, re-import (cached zip): hidden choices survive, the packs are identical.
    std::string e;
    const imp::Paths p1 = imp::MakePaths(g1, "caravan");
    Expect(imp::SetHidden(p1, {"Gamma"}, true, &e) == 2 && hidden::Load(g1).count("caravan_1_gamma"), "hide a map by file name");
    Expect(imp::SetHidden(p1, {"re_epsilon"}, false, &e) == 1 && !hidden::Load(g1).count("caravan_3_reepsilon"), "show a map");
    imp::RunSpec again = Spec(g1, L"");
    again.kind = imp::RunSpec::Kind::Download;
    again.urls = {"file:///does/not/exist.zip"};
    imp::RunResult r3 = imp::Run(again);
    Expect(r3.ok && r3.fingerprint == r1.fingerprint && PackTree(g1) == t1, "re-import from the cached zip (" + r3.message + ")");
    Expect(hidden::Load(g1) == std::set<std::string>({"caravan_1_gamma"}), "hidden choices survive a re-import");
    imp::State st;
    imp::Plugin pl;
    imp::LoadPlugin(g1, "caravan", &pl, &e);
    Expect(imp::LoadState(p1, &st) && imp::Status(p1, pl, &st, &e) == "imported", "status imported");

    // A stale recipe and a damaged pack.
    {
        imp::Plugin newer = pl;
        newer.recipe.output.version = "1.0.1";
        Expect(imp::Status(p1, newer, &st, &e) == "stale", "status stale after a recipe update");
        const std::wstring sp = g1 + L"\\Mods\\caravan-2\\spice.json";
        const auto keep = ReadFileBytes(sp);
        DeleteFileW(sp.c_str());
        Expect(imp::Status(p1, pl, &st, &e) == "damaged", "status damaged with a pack missing its spice.json");
        WriteFileBytes(sp, keep);
    }

    // Placement rollback: the third move fails, the previous import stays as it was.
    {
        int moves = 0;
        imp::RunSpec s = Spec(g1, f.zip);
        s.move = [&](const std::wstring& from, const std::wstring& to) -> unsigned long {
            if (++moves == 5) return ERROR_ACCESS_DENIED;
            return inst::DefaultMove(from, to);
        };
        imp::RunResult r = imp::Run(s);
        Expect(!r.ok && r.reason == "write" && PackTree(g1) == t1, "a failed move rolls back (" + r.reason + ": " + r.message + ")");
        Expect(!Exists(work1 + L"\\place.json"), "no journal left after a rollback");
    }

    // Crash journal: packs moved aside but not replaced are moved back at the next run.
    {
        MakeDir(work1 + L"\\old");
        inst::DefaultMove(g1 + L"\\Mods\\caravan-3", work1 + L"\\old\\caravan-3");
        WriteFileBytes(work1 + L"\\place.json", B("{\"old\":[\"caravan-1\",\"caravan-2\",\"caravan-3\"],\"new\":[\"caravan-1\",\"caravan-2\",\"caravan-3\"],\"committed\":false}\n"));
        imp::Recover(p1, "caravan");
        Expect(PackTree(g1) == t1 && !Exists(work1 + L"\\place.json"), "crash journal recovery restores the packs");
    }

    // A folder caravan-4 not made by the importer: refused as occupied and left alone.
    {
        const std::wstring foreign = g1 + L"\\Mods\\caravan-4";
        MakeDir(foreign);
        WriteFileBytes(foreign + L"\\keep.txt", B("mine"));
        imp::RunResult r = imp::Run(Spec(g1, f.zip));
        Expect(!r.ok && r.reason == "occupied" && r.message == "caravan-4" && Exists(foreign + L"\\keep.txt") && PackTree(g1).size() > t1.size(),
               "a foreign caravan-4 fails occupied and is untouched");
        std::vector<std::string> removed;
        Expect(imp::Uninstall(p1, "caravan", false, &removed, &e) && removed.size() == 3 && Exists(foreign + L"\\keep.txt"),
               "uninstall removes only generated packs");
        Expect(!Exists(g1 + L"\\Mods\\caravan-1") && hidden::Load(g1).empty() && !Exists(work1 + L"\\state.json") &&
                   Exists(work1 + L"\\dl\\Fix_0.1.zip"),
               "uninstall cleans hidden stems and state, keeps the zip");
        inst::DeleteTree(foreign);
    }

    // The store cascade: removing the importer plugin removes the packs it generated in game2.
    {
        const auto paths = inst::MakePaths(g2 + L"\\Mods");
        Expect(inst::GeneratedBy(paths, "caravan").size() == 3, "GeneratedBy finds the packs");
        Expect(!inst::ReservedGeneratedId(paths, "caravan-2").empty() && !inst::ReservedGeneratedId(paths, "caravan-7").empty() &&
                   inst::ReservedGeneratedId(paths, "caravan").empty(),
               "generated ids are reserved for the importer");
        const auto removed = inst::RemoveGenerated(paths, "caravan", false);
        Expect(removed.size() == 3 && PackTree(g2).empty() && hidden::Load(g2).empty() && Exists(g2 + L"\\Mods\\.import\\caravan\\dl") &&
                   !Exists(g2 + L"\\Mods\\.import\\caravan\\state.json"),
               "the cascade removes generated packs and keeps dl unless asked");
    }

    // An expect mismatch fails "recipe" and places nothing.
    {
        InstallPlugin(g2, RecipeJson(f, "", 8));
        imp::RunResult r = imp::Run(Spec(g2, f.zip));
        Expect(!r.ok && r.reason == "recipe" && PackTree(g2).empty(), "an expect mismatch fails recipe (" + r.message + ")");
        InstallPlugin(g2, RecipeJson(f));
    }

    // A vanilla file that differs from its pin fails "vanilla".
    {
        WriteFileBytes(g2 + L"\\Data\\Maps\\vanmap.xan", Xan(2, "modified"));
        imp::RunResult r = imp::Run(Spec(g2, f.zip));
        Expect(!r.ok && r.reason == "vanilla" && PackTree(g2).empty(), "a changed vanilla file fails vanilla (" + r.message + ")");
    }

    // Executable bytes in an allowlisted member fail "zip".
    {
        std::vector<ZipEntry> z = FixtureEntries();
        for (auto& en : z)
            if (en.name == kRoot + "Maps/Gamma.xan") en.data = B("MZ\x90\x00 not a map");
        const std::wstring evil = tmp + L"\\evil.zip";
        WriteZip(evil, z);
        Fixture fe = f;
        fe.zip = evil;
        const auto b = ReadFileBytes(evil);
        fe.sha = melange::hashutil::Sha256Hex(b.data(), b.size());
        fe.size = b.size();
        const std::wstring g3 = tmp + L"\\game3";
        MakeGame(g3, &fe);
        InstallPlugin(g3, RecipeJson(fe));
        imp::RunResult r = imp::Run(Spec(g3, evil));
        Expect(!r.ok && r.reason == "zip" && PackTree(g3).empty(), "executable content in a map fails zip (" + r.message + ")");
    }
}
}  // namespace

// --run <game> <plugin> <zip>: one import of a local zip with an installed plugin's recipe, for checks against real
// files kept outside the repository.
int RunOne(const std::wstring& game, const std::string& plugin, const std::wstring& zip) {
    imp::RunSpec s;
    s.game = game;
    std::string err;
    if (!imp::LoadPlugin(game, plugin, &s.plugin, &err)) {
        printf("{\"ok\":false,\"reason\":\"recipe\",\"message\":\"%s\"}\n", err.c_str());
        return 2;
    }
    s.kind = imp::RunSpec::Kind::File;
    s.sourceId = s.plugin.recipe.sources.front().id;
    s.file = zip;
    const imp::RunResult r = imp::Run(s);
    printf("{\"ok\":%s,\"reason\":\"%s\",\"message\":\"%s\",\"maps\":%d,\"skipped\":%d,\"fingerprint\":\"%s\",\"bytes\":%llu,\"counts\":{",
           r.ok ? "true" : "false", r.reason.c_str(), r.message.c_str(), r.maps, r.skipped, r.fingerprint.c_str(),
           static_cast<unsigned long long>(r.bytes));
    bool first = true;
    for (const auto& [k, v] : r.counts) printf("%s\"%s\":%d", first ? "" : ",", k.c_str(), v), first = false;
    printf("},\"packs\":[");
    for (size_t i = 0; i < r.packs.size(); ++i) printf("%s\"%s\"", i ? "," : "", r.packs[i].c_str());
    printf("]}\n");
    return r.ok ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc == 5 && std::wstring(argv[1]) == L"--run") return RunOne(argv[2], N(argv[3]), argv[4]);
    wchar_t tmpBase[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpBase);
    const std::wstring tmp = std::wstring(tmpBase) + L"melange-import-" + W(melange::hashutil::RandomSalt().substr(0, 8));
    MakeDir(tmp);

    Fixture f;
    f.root = tmp;
    f.zip = tmp + L"\\Fix_0.1.zip";
    {
        // The vanilla pins come from the synthetic game files; build one game first to know them.
        const std::wstring g0 = tmp + L"\\game0";
        MakeGame(g0, &f);
    }
    Expect(WriteZip(f.zip, FixtureEntries()), "fixture zip written");
    const auto zb = ReadFileBytes(f.zip);
    f.sha = melange::hashutil::Sha256Hex(zb.data(), zb.size());
    f.size = zb.size();

    imp::Recipe r;
    std::string err;
    imp::ParseRecipe(RecipeJson(f), "caravan", &r, &err);
TestRecipe(f);
TestPlanPure(f);
TestDescriptorGolden();
TestXomV1();
TestHidden();
TestSpiceKeys(tmp);
TestArchiveChecks(tmp, r.reader);
TestImport(tmp, f);

    inst::DeleteTree(tmp);
    printf("import_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
