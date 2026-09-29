// Offline self-test for the spice.json "weapons" array: shape (spice.cpp), names, bases, cells, set types and the
// k order across mods (weapons/manifest.cpp). Exit code 0 = all passed.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "mods/spice.h"
#include "weapons/fields.h"
#include "weapons/manifest.h"

namespace wm = melange::weapons::manifest;
using melange::weapons::FieldType;

namespace {
int g_fail = 0, g_pass = 0;
std::wstring g_root;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

std::string Manifest(const char* id, const std::string& weapons, const char* kind = "content") {
    return std::string("{\"spiceVersion\":1,\"id\":\"") + id + "\",\"version\":\"1.0.0\",\"name\":\"" + id +
           "\",\"melange\":{\"range\":\">=0.0.0\"},\"kind\":\"" + kind + "\",\"entry\":{},\"weapons\":" + weapons + "}";
}

bool Load(const char* id, const std::string& json, melange::spice::Manifest* m, std::string* errs = nullptr) {
    const std::wstring dir = g_root + L"\\" + std::wstring(id, id + strlen(id));
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((dir + L"\\spice.json").c_str(), L"wb");
    if (!f) return false;
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);
    std::vector<melange::spice::Error> e;
    const bool ok = melange::spice::Parse(dir, m, &e);
    if (errs)
        for (auto& x : e) *errs += x.text + "; ";
    return ok;
}

std::vector<wm::CloneDecl> Decls(const char* id, const std::string& weapons, std::string* why = nullptr) {
    melange::spice::Manifest m;
    std::string perr;
    if (!Load(id, Manifest(id, weapons), &m, &perr)) {
        if (why) *why = "spice: " + perr;
        return {};
    }
    std::vector<wm::Error> errs;
    auto d = wm::Parse(m, &errs);
    if (why)
        for (auto& e : errs) *why += e.text + "; ";
    return d;
}

bool Refused(const char* id, const std::string& weapons, const char* expectText) {
    std::string why;
    auto d = Decls(id, weapons, &why);
    const bool ok = d.empty() && why.find(expectText) != std::string::npos;
    if (!ok) printf("  (%s: got '%s')\n", id, why.c_str());
    return ok;
}

const char* kMega = R"([{"name":"kWeaponMegaBazooka","base":"kWeaponBazooka","cell":29,"bank":"megabazooka.xom",
  "set":{"WormDamageMagnitude":120,"WormDamageRadius":123.75,"LandDamageRadius":90,"ImpulseRadius":165,
         "PayloadGraphicsResourceID":"mega-bazooka.Payload","LaunchSfx":"weapons/SheepBaa"},
  "panelIcon":"icons/megabazooka.png","hudIcon":"mega-bazooka.hud.tga",
  "text":{"name":"Mega Bazooka","help":"Like a bazooka, only more so."}}])";
}  // namespace

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"melange_weapons_selftest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_root.c_str(), nullptr);

    // Schema types.
    Expect(melange::weapons::fields::SchemaType(wm::kContainerClass, "WormDamageMagnitude") == FieldType::F32, "F32 field");
    Expect(melange::weapons::fields::SchemaType(wm::kContainerClass, "PayloadGraphicsResourceID") == FieldType::String,
           "String field");
    Expect(melange::weapons::fields::SchemaType(wm::kContainerClass, "DisplayName") == FieldType::String, "inherited field");
    Expect(melange::weapons::fields::SchemaType(wm::kContainerClass, "NoSuchField") == FieldType::None, "unknown field");

    // The spec's example parses in full.
    {
        std::string why;
        auto d = Decls("mega-bazooka", kMega, &why);
        Expect(d.size() == 1, "mega-bazooka parses: " + why);
        if (d.size() == 1) {
            Expect(d[0].baseId == 1 && d[0].cell == 29 && d[0].bank == "megabazooka.xom", "base, cell, bank");
            Expect(d[0].set.size() == 6, "six set values");
            Expect(d[0].text.name == "Mega Bazooka" && !d[0].text.help.empty(), "text");
            bool typed = true;
            for (auto& s : d[0].set) {
                if (s.field == "WormDamageMagnitude") typed &= s.type == FieldType::F32 && s.number == 120;
                if (s.field == "LaunchSfx") typed &= s.type == FieldType::String && s.string == "weapons/SheepBaa";
            }
            Expect(typed, "set values carry schema types");
        }
    }

    // Name rules.
    Expect(Refused("n1", R"([{"name":"kweaponLower","base":"kWeaponBazooka"}])", "name must match"), "lowercase after kWeapon");
    Expect(Refused("n2", R"([{"name":"kWeaponAb","base":"kWeaponBazooka"}])", "name must match"), "too short");
    Expect(Refused("n3", R"([{"name":"kWeaponClusterX","base":"kWeaponBazooka"}])", "kWeaponCluster"), "Cluster prefix");
    Expect(Refused("n4", R"([{"name":"kWeaponFactoryX","base":"kWeaponBazooka"}])", "kWeaponCluster"), "Factory prefix");
    Expect(Refused("n5", R"([{"name":"kWeaponBad-Name","base":"kWeaponBazooka"}])", "name must match"), "bad character");
    Expect(Refused("n6", R"([{"name":"kWeaponTwin","base":"kWeaponBazooka"},{"name":"kWeaponTwin","base":"kWeaponGrenade"}])",
                   "declared twice"),
           "duplicate in one mod");
    Expect(wm::ValidName("kWeaponAbc") && !wm::ValidName("kWeaponA" + std::string(41, 'b')), "length bounds");

    // Bases.
    Expect(Refused("b1", R"([{"name":"kWeaponSuperSheep2","base":"kWeaponSheep"}])", "not clonable"), "Sheep refused");
    for (const char* b : {"kWeaponGrenade", "kWeaponHolyHandGrenade", "kWeaponBananaBomb", "kWeaponGasCanister"}) {
        const std::string w = std::string(R"([{"name":"kWeaponCloneOf","base":")") + b + "\"}]";
        Expect(Decls("b2", w).size() == 1, std::string("whitelisted base ") + b);
    }

    // Cells.
    Expect(Refused("c1", R"([{"name":"kWeaponCellX","base":"kWeaponBazooka","cell":5}])", "cell must be"), "vanilla cell");
    Expect(Refused("c2", R"([{"name":"kWeaponCellX","base":"kWeaponBazooka","cell":"29"}])", "cell"), "cell type");

    // set types.
    Expect(Refused("s1", R"([{"name":"kWeaponSetX","base":"kWeaponBazooka","set":{"Bogus":1}}])", "not a settable field"),
           "unknown field");
    Expect(Refused("s2", R"([{"name":"kWeaponSetX","base":"kWeaponBazooka","set":{"WormDamageMagnitude":"big"}}])",
                   "wrong type"),
           "string for a float");
    Expect(Refused("s3", R"([{"name":"kWeaponSetX","base":"kWeaponBazooka","set":{"PayloadGraphicsResourceID":3}}])",
                   "wrong type"),
           "number for a string");
    Expect(Refused("s4", R"([{"name":"kWeaponSetX","base":"kWeaponBazooka","set":{"IsHoming":1}}])", "wrong type"),
           "number for a bool");
    Expect(Decls("s5", R"([{"name":"kWeaponSetX","base":"kWeaponBazooka","set":{"IsHoming":true}}])").size() == 1, "bool ok");
    Expect(Refused("s6", R"([{"name":"kWeaponSetX","base":"kWeaponBazooka","set":{"SkimDamping":1}}])", ""),
           "array field not settable");

    // Paths.
    Expect(Refused("p1", R"([{"name":"kWeaponPathX","base":"kWeaponBazooka","bank":"../Tweak/WEAPTWK.XOM"}])", "bank must be"),
           "bank escaping assets/data");
    Expect(Refused("p2", R"([{"name":"kWeaponPathX","base":"kWeaponBazooka","hudIcon":"bazooka.tga"}])", "hudIcon must be"),
           "hudIcon not <id>.*");
    Expect(Decls("p3", R"([{"name":"kWeaponPathX","base":"kWeaponBazooka","hudIcon":"p3.hud.tga"}])").size() == 1, "hudIcon ok");
    Expect(Refused("p4", R"([{"name":"kWeaponPathX","base":"kWeaponBazooka","panelIcon":"C:/x.png"}])", "panelIcon must be"),
           "absolute icon path");

    // Shape errors from spice.cpp.
    Expect(Refused("f1", R"([{"name":"kWeaponShapeX","base":"kWeaponBazooka","colour":1}])", "unknown weapons key"),
           "unknown key");
    Expect(Refused("f2", R"({"name":"kWeaponShapeX"})", "must be an array"), "not an array");
    Expect(Refused("f3", R"([{"name":"kWeaponA1x","base":"kWeaponBazooka"},{"name":"kWeaponA2x","base":"kWeaponBazooka"},
        {"name":"kWeaponA3x","base":"kWeaponBazooka"},{"name":"kWeaponA4x","base":"kWeaponBazooka"}])", "at most 3"),
           "four clones in one mod");
    {
        melange::spice::Manifest m;
        std::string e;
        Load("f4", Manifest("f4", R"([{"name":"kWeaponClientX","base":"kWeaponBazooka"}])", "client-only"), &m, &e);
        Expect(e.find("requires kind") != std::string::npos, "client-only mod refused");
    }

    // k order and cells across mods.
    {
        auto a = Decls("ka", R"([{"name":"kWeaponKaa","base":"kWeaponBazooka"},{"name":"kWeaponKbb","base":"kWeaponGrenade","cell":29}])");
        auto b = Decls("kb", R"([{"name":"kWeaponKcc","base":"kWeaponBazooka"}])");
        std::vector<wm::Error> ref;
        auto all = wm::Assign({a, b}, &ref);
        Expect(ref.empty() && all.size() == 3, "three clones from two mods");
        if (all.size() == 3) {
            Expect(all[0].name == "kWeaponKaa" && all[0].k == 0 && all[0].cell == 39, "k0 takes the first free cell");
            Expect(all[1].name == "kWeaponKbb" && all[1].k == 1 && all[1].cell == 29, "k1 keeps its cell");
            Expect(all[2].name == "kWeaponKcc" && all[2].k == 2 && all[2].cell == 40, "k2 in load order");
        }
        auto c = Decls("kc", R"([{"name":"kWeaponKdd","base":"kWeaponBazooka"}])");
        ref.clear();
        all = wm::Assign({a, b, c}, &ref);
        Expect(all.size() == 3 && ref.size() == 1 && ref[0].mod == "kc", "a fourth clone refuses the later mod");
        auto d = Decls("kd", R"([{"name":"kWeaponKee","base":"kWeaponBazooka","cell":29}])");
        ref.clear();
        all = wm::Assign({a, d}, &ref);
        Expect(all.size() == 2 && ref.size() == 1 && ref[0].mod == "kd" && ref[0].text.find("cell 29") != std::string::npos,
               "cell conflict refuses the later mod");
        auto e = Decls("ke", R"([{"name":"kWeaponKaa","base":"kWeaponGrenade"}])");
        ref.clear();
        all = wm::Assign({a, e}, &ref);
        Expect(all.size() == 2 && ref.size() == 1 && ref[0].mod == "ke", "name reuse refuses the later mod");
        ref.clear();
        all = wm::Assign({e, a}, &ref);
        Expect(all.size() == 1 && all[0].mod == "ke" && ref.size() == 1 && ref[0].mod == "ka", "load order decides");
    }

    wm::Freeze({});
    Expect(wm::IsFrozen() && wm::Frozen().empty(), "freeze");
    wm::CloneDecl x;
    x.name = "kWeaponLate";
    wm::Freeze({x});
    Expect(wm::Frozen().empty(), "freeze once per launch");

    printf("weapons_manifest_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
