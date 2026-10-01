// The Erg level service without the game. Every level here is synthetic, built in memory from the compiled schema:
// load, patch, build (renumbering, byte-identical empty builds, databank and voxel edits), diff, the registry bank, the
// project store and every level.* method over a temporary install folder.
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "erg/bank.h"
#include "erg/build.h"
#include "erg/hmp.h"
#include "erg/install.h"
#include "erg/jsonio.h"
#include "erg/load.h"
#include "erg/luagen.h"
#include "erg/objects.h"
#include "erg/patch.h"
#include "erg/project.h"
#include "erg/scene.h"
#include "erg/service.h"
#include "erg/voxels.h"
#include "erg/xomutil.h"
#include "levels/manifest.h"
#include "mods/spice.h"
#include "tools/hash.h"
#include "xom/json.h"
#include "xom/xom.h"

namespace erg = melange::erg;
namespace xom = melange::xom;
namespace xu = melange::erg::xomutil;
using xom::Json;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
        return;
    }
    ++g_fail;
    std::printf("FAIL: %s\n", what.c_str());
}

// ---------------------------------------------------------------- synthetic documents
void AddType(xom::Document& d, const char* cls, uint32_t version = 0) {
    if (std::string(cls) == "XContainer") {  // an abstract base: in TYPE tables, but not in the schema
        xom::TypeEntry t;
        t.name = cls;
        const uint8_t guid[16] = {0x46, 0xd4, 0x1c, 0x5e, 0xa3, 0x48, 0xfe, 0x44, 0xa5, 0x5a, 0xe2, 0x47, 0xb8, 0xf5, 0xe7, 0x13};
        std::memcpy(t.guid.data(), guid, 16);
        std::memcpy(t.rawName.data(), cls, std::strlen(cls));
        d.types.push_back(t);
        return;
    }
    Expect(xu::EnsureType(d, cls, ""), std::string("type ") + cls);
    for (auto& t : d.types)
        if (t.name == cls) t.version = version;
}

xom::Object New(const xom::Document& d, const char* cls) {
    xom::Object o;
    std::string err;
    Expect(xu::NewObject(d, cls, &o, &err), std::string("new ") + cls + " " + err);
    return o;
}

void SetU(xom::Object& o, const char* f, uint64_t v) {
    xom::Value* x = o.field(f);
    if (x) x->setInt(static_cast<int64_t>(v));
}

void Refs(xom::Object& o, const char* f, std::vector<uint32_t> refs) {
    xom::Value* x = o.field(f);
    x->items.clear();
    for (uint32_t r : refs) x->items.push_back(xu::RefValue(r));
}

std::vector<uint8_t> Bytes(const xom::Document& d) {
    std::vector<uint8_t> out;
    std::string err;
    Expect(xom::serialize(d, out, &err), "serialize " + err);
    return out;
}

struct DetailSpec { const char* name; const char* res; erg::Vec3 pos; };

// Objects: details 1..11, then frames 12 (Worms, folder), 13 (tower), 14 (ledge, child of tower), 15 (root).
std::vector<uint8_t> SyntheticXan() {
    xom::Document d;
    for (const char* c : {"XNode", "XInteriorNode", "XContainer", "DetailEntityStore", "LandFrameStore"}) AddType(d, c);
    d.strings.push_back("");
    const DetailSpec details[] = {
        {"WORM0", "CheesyGrinWorm", {1, 2, 3}}, {"WORM1", "CheesyGrinWorm", {2, 2, 3}}, {"WORM2", "CheesyGrinWorm", {3, 2, 3}},
        {"WORM3", "CheesyGrinWorm", {4, 2, 3}}, {"WORM4", "CheesyGrinWorm", {5, 2, 3}}, {"WORM5", "CheesyGrinWorm", {6, 2, 3}},
        {"WORM6", "CheesyGrinWorm", {7, 2, 3}}, {"WORM7", "CheesyGrinWorm", {8, 2, 3}}, {"VISIBLE_Toolbox", "BUILDING4", {0.5, 1.25, 0.75}},
        {"Diner6", "Camera", {-4, 7.25, 12.5}}, {"PNTLGHT 120 30 5 100", "LIGHT", {1, 1, 1}},
    };
    for (const auto& s : details) {
        xom::Object o = New(d, "DetailEntityStore");
        xu::SetStr(o, "Name", s.name);
        xu::SetStr(o, "ResourceName", s.res);
        xu::SetVec(o, "Position", s.pos);
        xu::SetVec(o, "Scale", {1, 1, 1});
        o.field("Bounds")->setComponents({0, 0, 0, -1});
        SetU(o, "BoundMode", 1);
        d.objects.push_back(std::move(o));
    }
    auto frame = [&](const char* name, std::array<int, 3> size, erg::Vec3 pos, erg::Vec3 rot, erg::Vec3 scale) {
        xom::Object o = New(d, "LandFrameStore");
        xu::SetStr(o, "Name", name);
        xu::SetVec(o, "Position", pos);
        xu::SetVec(o, "Orientation", rot);
        xu::SetVec(o, "Scale", scale);
        SetU(o, "XSize", static_cast<uint64_t>(size[0]));
        SetU(o, "YSize", static_cast<uint64_t>(size[1]));
        SetU(o, "ZSize", static_cast<uint64_t>(size[2]));
        const size_t cells = static_cast<size_t>(size[0]) * size[1] * size[2];
        xom::Value* v = o.field("Voxels");
        for (size_t i = 0; i < cells; ++i) {
            const uint32_t w = (i % 3 ? 3u : 0u) | static_cast<uint32_t>(i % 7) << 2;
            for (int k = 0; k < 4; ++k) v->raw.push_back(static_cast<uint8_t>(w >> (8 * k)));
        }
        xom::Value* h = o.field("HeightMap");
        const size_t corners = cells ? static_cast<size_t>(size[0] + 1) * (size[2] + 1) : 0;
        for (size_t i = 0; i < corners; ++i) {
            const float f = static_cast<float>(i) * 0.25f;
            uint32_t b;
            std::memcpy(&b, &f, 4);
            for (int k = 0; k < 4; ++k) h->raw.push_back(static_cast<uint8_t>(b >> (8 * k)));
        }
        return o;
    };
    xom::Object worms = frame("Worms", {1, 1, 1}, {10, 0, 5}, {0, 0, 0}, {1, 1, 1});
    Refs(worms, "Details", {1, 2, 3, 4, 5, 6, 7, 8});
    xom::Object tower = frame("tower", {4, 3, 2}, {-3, 1, 2}, {0.25, 0.5, 1.5}, {1.5, 0.5, 1});
    Refs(tower, "Details", {9, 11});
    Refs(tower, "Children", {14});
    xom::Object ledge = frame("ledge", {2, 2, 2}, {1, 1, 1}, {0, 0, 0}, {1, 1, 1});
    Refs(ledge, "Details", {10});
    xom::Object root = frame("root", {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0});
    Refs(root, "Children", {12, 13});
    d.objects.push_back(std::move(worms));
    d.objects.push_back(std::move(tower));
    d.objects.push_back(std::move(ledge));
    d.objects.push_back(std::move(root));
    d.root = 15;
    return Bytes(d);
}

std::vector<uint8_t> SyntheticLevelXom(bool heightmapKeys) {
    xom::Document d;
    for (const char* c : {"XContainer", "XResourceDetails", "XStringResourceDetails", "XDataBank"}) AddType(d, c);
    d.strings.push_back("");
    std::vector<std::pair<const char*, const char*>> kv = {
        {"Databank.MaterialFile", "Maps\\synth.txt"}, {"Databank.Theme", "BUILDING"}, {"Databank.TimeOfDay", "NIGHT"}};
    if (heightmapKeys) kv.push_back({"Heightmap.BaseTexture", "B01"});
    std::vector<uint32_t> refs;
    for (auto& [k, v] : kv) {
        xom::Object o = New(d, "XStringResourceDetails");
        xu::SetStr(o, "Name", k);
        xu::SetStr(o, "Value", v);
        SetU(o, "Flags", 64);
        d.objects.push_back(std::move(o));
        refs.push_back(static_cast<uint32_t>(d.objects.size()));
    }
    xom::Object bank = New(d, "XDataBank");
    Refs(bank, "StringResources", refs);
    d.objects.push_back(std::move(bank));
    d.root = static_cast<uint32_t>(d.objects.size());
    return Bytes(d);
}

struct RegSpec { const char* key; const char* file; const char* name; int type; };

std::vector<uint8_t> SyntheticScripts() {
    xom::Document d;
    AddType(d, "XContainer");
    AddType(d, "XResourceDetails");
    AddType(d, "XContainerResourceDetails");
    AddType(d, "XDataBank");
    AddType(d, "WXFE_LevelDetails", 1);
    d.strings.push_back("");
    const RegSpec regs[] = {{"Multi.Synth", "Multi_Synth", "FETXT.Synth", 0}, {"Multi.DinerMight", "Multi_DinerMight", "FETXT.Diner", 0},
                            {"Story.Synth", "Multi_Synth", "FETXT.Synth", 4}};
    const uint32_t n = 3;
    for (uint32_t i = 0; i < n; ++i) {
        xom::Object r = New(d, "XContainerResourceDetails");
        xu::SetStr(r, "Name", regs[i].key);
        SetU(r, "Flags", 80);
        r.field("Value")->bits = n + 2 + i;
        d.objects.push_back(std::move(r));
    }
    xom::Object bank = New(d, "XDataBank");
    Refs(bank, "ContainerResources", {1, 2, 3});
    d.objects.push_back(std::move(bank));
    for (const auto& r : regs) {
        xom::Object l = New(d, "WXFE_LevelDetails");
        xu::SetStr(l, "Frontend_Name", r.name);
        xu::SetStr(l, "Level_FileName", r.file);
        xu::SetStr(l, "Level_ScriptName", "stdvs,wormpot");
        xu::SetStr(l, "Lock", "Lock.Map.Synth");
        SetU(l, "Level_Type", static_cast<uint64_t>(r.type));
        SetU(l, "Theme_Type", 5);
        d.objects.push_back(std::move(l));
    }
    d.root = n + 1;
    return Bytes(d);
}

// A weapon table: two crate-able names, a sub-munition without a display name and a non-weapon container.
std::vector<uint8_t> SyntheticWeapons() {
    xom::Document d;
    for (const char* c : {"XContainer", "XResourceDetails", "XContainerResourceDetails", "XDataBank", "BaseWeaponContainer"}) AddType(d, c);
    d.strings.push_back("");
    const char* names[] = {"kWeaponBazooka", "kUtilityJetpack", "kWeaponClusterBomb", "kMineFactoryData"};
    const char* shown[] = {"Text.kWeaponBazooka", "Text.kUtilityJetpack", "", "Text.kMineFactoryData"};
    const uint32_t n = 4;
    for (uint32_t i = 0; i < n; ++i) {
        xom::Object r = New(d, "XContainerResourceDetails");
        xu::SetStr(r, "Name", names[i]);
        SetU(r, "Flags", 80);
        r.field("Value")->bits = n + 2 + i;
        d.objects.push_back(std::move(r));
    }
    xom::Object bank = New(d, "XDataBank");
    Refs(bank, "ContainerResources", {1, 2, 3, 4});
    d.objects.push_back(std::move(bank));
    for (uint32_t i = 0; i < n; ++i) {
        xom::Object w = New(d, "BaseWeaponContainer");
        xu::SetStr(w, "DisplayName", shown[i]);
        d.objects.push_back(std::move(w));
    }
    d.root = n + 1;
    return Bytes(d);
}

void TestObjects() {
    const std::vector<uint8_t> wb = SyntheticWeapons();
    xom::Document wd;
    std::string err;
    Expect(xom::parse(wb.data(), wb.size(), wd, &err), "objects: the weapon table parses " + err);
    erg::objects::Catalog cat;
    Expect(erg::objects::CatalogFrom(wd, &cat) && cat.weapons == std::vector<std::string>{"kWeaponBazooka"} &&
               cat.utilities == std::vector<std::string>{"kUtilityJetpack"},
           "objects: the catalog holds the named weapons and utilities only");

    erg::Scene s;
    s.frames.push_back(erg::Frame{});
    s.frames[0].id = 1;
    auto knot = [&](const std::string& name, double y) {
        erg::Detail d;
        d.id = static_cast<int64_t>(s.details.size()) + 1;
        d.frame = 1;
        d.name = name;
        d.resource = std::string(erg::kKnotResource);
        d.pos = {0, y, 0};
        s.details.push_back(d);
    };
    erg::ObjectSpec crate;
    crate.knot = "CRATE_0";
    crate.crate.contents = "kWeaponBazooka";
    erg::ObjectSpec pad;
    pad.type = erg::ObjectType::Telepad;
    pad.knot = "TP_1_0";
    pad.group = 1;
    s.objects = {crate, pad};
    knot("CRATE_0", 1);
    knot("TP_1_0", 1);
    std::vector<std::string> warns;
    Expect(erg::objects::Validate(s, cat, &warns, &err) && warns.size() == 1 && warns[0].find("group 1") != std::string::npos,
           "objects: a lone telepad warns");
    s.water = 40;
    warns.clear();
    Expect(erg::objects::Validate(s, cat, &warns, &err) && warns.size() == 2 && warns[0].find("CRATE_0 is under the water") == 0,
           "objects: a crate under the water warns");
    s.water.reset();
    s.objects[0].crate.contents = "kWeaponClusterBomb";
    Expect(!erg::objects::Validate(s, cat, nullptr, &err) && err.find("objects[0].crate.contents") == 0, "objects: an unknown weapon is refused");
    s.objects[0].crate.kind = erg::CrateKind::Utility;
    s.objects[0].crate.contents = "kWeaponBazooka";
    Expect(!erg::objects::Validate(s, cat, nullptr, &err), "objects: a weapon in a utility crate is refused");
    s.objects[0].crate.contents = "kUtilityJetpack";
    Expect(erg::objects::Validate(s, cat, nullptr, &err), "objects: a utility crate");

    Expect(erg::objects::NextKnot(s, erg::ObjectType::Crate, 0) == "CRATE_1", "knots: the next crate");
    Expect(erg::objects::NextKnot(s, erg::ObjectType::Telepad, 1) == "TP_1_1" &&
               erg::objects::NextKnot(s, erg::ObjectType::Telepad, 2) == "TP_2_0" &&
               erg::objects::NextKnot(s, erg::ObjectType::Telepad, 9).empty(),
           "knots: telepads by group");
    Expect(erg::objects::NextKnot(s, erg::ObjectType::Trigger, 0) == "TRIG_0", "knots: the first trigger");
    Expect(erg::objects::NextKnot(s, erg::ObjectType::MineFactory, 0) == "minefactory", "knots: the factory");
    knot("minefactory", 0);
    Expect(erg::objects::NextKnot(s, erg::ObjectType::MineFactory, 0).empty(), "knots: one factory only");
    for (int i = 0; i < 256; ++i) knot("TRIG_" + std::to_string(i), 0);
    Expect(erg::objects::NextKnot(s, erg::ObjectType::Trigger, 0).empty(), "knots: none left after TRIG_255");
}

std::vector<uint8_t> SyntheticStrings() {
    xom::Document d;
    for (const char* c : {"XResourceDetails", "XStringResourceDetails", "XDataBank", "XContainer"}) AddType(d, c);
    d.strings.push_back("");
    xom::Object o = New(d, "XStringResourceDetails");
    xu::SetStr(o, "Name", "FETXT.Synth");
    xu::SetStr(o, "Value", "Synthetic Harbour");
    d.objects.push_back(std::move(o));
    xom::Object bank = New(d, "XDataBank");
    Refs(bank, "StringResources", {1});
    d.objects.push_back(std::move(bank));
    d.root = 2;
    return Bytes(d);
}

std::vector<uint8_t> SyntheticHmp() {
    std::vector<uint8_t> h(erg::load::kHmpBytes);
    for (size_t i = 0; i < h.size(); ++i) h[i] = static_cast<uint8_t>(i * 31 + 7);
    for (size_t i = 0; i < 10000; ++i) {
        const float f = static_cast<float>(i % 100) / 100.0f;
        std::memcpy(h.data() + i * 4, &f, 4);
    }
    return h;
}

erg::load::BaseFiles Base() {
    erg::load::BaseFiles f;
    f.key = "Multi.Synth";
    f.file = "Multi_Synth";
    f.title = "Synthetic Harbour";
    f.xan = SyntheticXan();
    f.xom = SyntheticLevelXom(false);
    f.hmp = SyntheticHmp();
    return f;
}

const erg::Detail* ByName(const erg::Scene& s, const std::string& n) {
    for (const auto& d : s.details)
        if (d.name == n) return &d;
    return nullptr;
}

std::vector<uint8_t> FileBytes(const std::vector<erg::build::File>& files, const std::string& rel) {
    for (const auto& f : files)
        if (f.rel == rel) return f.bytes;
    return {};
}

bool HasFile(const std::vector<erg::build::File>& files, const std::string& rel) {
    for (const auto& f : files)
        if (f.rel == rel) return true;
    return false;
}

erg::Patch EmptyPatch(const erg::load::Loaded& L, const std::string& stem) {
    erg::Patch p;
    p.stem = stem;
    p.title = "Test Level";
    p.base = L.scene.base;
    p.base.file.clear();
    return p;
}

// ---------------------------------------------------------------- tests
void TestLoad() {
    const erg::load::BaseFiles f = Base();
    erg::load::Loaded L;
    std::string err;
    Expect(erg::load::LoadScene(f, &L, &err), "load: " + err);
    const erg::Scene& s = L.scene;
    Expect(s.frames.size() == 4 && s.details.size() == 11, "load: 4 frames, 11 details");
    Expect(s.frames[0].id == 15 && s.frames[0].parent == -1 && s.frames[0].name == "root", "load: root first");
    Expect(s.frames[1].id == 12 && s.frames[1].folder && s.frames[1].parent == 15, "load: Worms is a folder under root");
    const erg::Frame* tower = s.FindFrame(13);
    Expect(tower && !tower->folder && tower->size == std::array<int, 3>{4, 3, 2} && std::fabs(tower->rot[2] - 1.5) < 1e-6, "load: tower");
    Expect(s.FindFrame(14) && s.FindFrame(14)->parent == 13, "load: ledge under tower");
    Expect(tower && tower->voxels > 0 && tower->heightMap > 0 && L.blobs.at(tower->voxels).size() == 24 * 4 &&
               L.blobs.at(tower->heightMap).size() == 15 * 4,
           "load: tower blobs");
    Expect(s.blobs.size() == 6, "load: two blobs for each of three frames with voxels");
    const erg::Detail* w0 = ByName(s, "WORM0");
    Expect(w0 && w0->src == 1 && w0->frame == 12 && w0->role == erg::Role::Spawn && w0->pos == erg::Vec3{1, 2, 3}, "load: WORM0");
    Expect(ByName(s, "Diner6") && ByName(s, "Diner6")->role == erg::Role::Camera && ByName(s, "Diner6")->frame == 14, "load: camera in ledge");
    Expect(ByName(s, "VISIBLE_Toolbox")->role == erg::Role::Scenery && ByName(s, "PNTLGHT 120 30 5 100")->role == erg::Role::Light, "load: roles");
    Expect(s.databank.theme == "BUILDING" && s.databank.timeOfDay == "NIGHT" && s.databank.materialFile == "Maps\\synth.txt" &&
               s.databank.heightmapBase.empty(),
           "load: databank");
    Expect(s.base.sha256.xan == erg::load::Sha256(f.xan) && s.base.sha256.hmp.size() == 64 && s.hmp == erg::HmpMode::Copy, "load: sha256");
    erg::Scene back;
    Expect(erg::ParseScene(erg::WriteScene(s), &back, &err), "load: the scene JSON parses: " + err);
    Expect(erg::WriteScene(back) == erg::WriteScene(s), "load: the scene JSON round-trips");

    auto refuse = [&](erg::load::BaseFiles g, const char* what) {
        erg::load::Loaded x;
        std::string e;
        Expect(!erg::load::LoadScene(g, &x, &e) && !e.empty(), std::string("load refuses ") + what + " (" + e + ")");
    };
    erg::load::BaseFiles g = f;
    g.xan.resize(g.xan.size() - 3);
    refuse(g, "a truncated .xan");
    g = f;
    g.hmp->pop_back();
    refuse(g, "a short .hmp");
    g = f;
    g.xan = f.xom;
    refuse(g, "a level .XOM as .xan");
    {
        xom::Document d;
        xom::ParseOptions opt;
        opt.strict = true;
        xom::parse(f.xan.data(), f.xan.size(), d, nullptr, opt);
        d.objects[11].field("Details")->items.push_back(xu::RefValue(99));
        g = f;
        g.xan = Bytes(d);
        refuse(g, "a Details reference out of range");
        xom::parse(f.xan.data(), f.xan.size(), d, nullptr, opt);
        d.objects[12].field("Details")->items.push_back(xu::RefValue(1));
        g.xan = Bytes(d);
        refuse(g, "a detail owned by two frames");
        xom::parse(f.xan.data(), f.xan.size(), d, nullptr, opt);
        d.objects[13].field("Children")->items.push_back(xu::RefValue(13));
        g.xan = Bytes(d);
        refuse(g, "a frame cycle");
        xom::parse(f.xan.data(), f.xan.size(), d, nullptr, opt);
        d.objects[12].field("Voxels")->raw.resize(8);
        g.xan = Bytes(d);
        refuse(g, "a voxel array of the wrong length");
    }
}

void TestEmptyBuild() {
    const erg::load::BaseFiles f = Base();
    erg::load::Loaded L;
    std::string err;
    erg::load::LoadScene(f, &L, &err);
    erg::Scene s;
    erg::build::VoxelEdits v;
    Expect(erg::build::Apply(L, EmptyPatch(L, "ergtest_synth"), {false, true}, &s, &v, &err), "empty: apply " + err);
    s.hmp = erg::HmpMode::Copy;
    std::vector<erg::build::File> files;
    Expect(erg::build::Build(L, s, v, {}, &files, &err), "empty: build " + err);
    Expect(FileBytes(files, "Maps/ergtest_synth.xan") == f.xan, "empty: .xan byte-identical");
    Expect(FileBytes(files, "ergtest_synth.XOM") == f.xom, "empty: .XOM byte-identical");
    Expect(FileBytes(files, "Maps/ergtest_synth.hmp") == *f.hmp, "empty: .hmp byte-identical");
    Expect(files.size() == 3 && !HasFile(files, "ergtest_synth.lub"), "empty: three files, no chunk");

    s.hmp = erg::HmpMode::None;
    erg::build::Build(L, s, v, {}, &files, &err);
    Expect(files.size() == 2 && !HasFile(files, "Maps/ergtest_synth.hmp"), "hmp none: no .hmp");
    s.hmp = erg::HmpMode::Flat;
    erg::build::Build(L, s, v, {}, &files, &err);
    const std::vector<uint8_t> flat = FileBytes(files, "Maps/ergtest_synth.hmp");
    Expect(flat.size() == erg::load::kHmpBytes && std::all_of(flat.begin(), flat.begin() + 40000, [](uint8_t b) { return b == 0; }) &&
               std::equal(flat.begin() + 40000, flat.end(), f.hmp->begin() + 40000),
           "hmp flat: heights 0, blend kept");
    erg::build::Options opt;
    opt.materialTxt = std::vector<uint8_t>{'B', '0', '1'};
    s.hmp = erg::HmpMode::Copy;
    Expect(erg::build::Build(L, s, v, opt, &files, &err) && FileBytes(files, "Maps/ergtest_synth.txt").size() == 3, "material .txt copied");
    erg::load::BaseFiles again = f;
    again.xan = FileBytes(files, "Maps/ergtest_synth.xan");
    again.xom = FileBytes(files, "ergtest_synth.XOM");
    erg::load::Loaded L2;
    Expect(erg::load::LoadScene(again, &L2, &err) && L2.scene.databank.materialFile == "Maps\\ergtest_synth.txt", "material file renamed");
    s.stem = "a.b";
    Expect(!erg::build::Build(L, s, v, {}, &files, &err), "build refuses a stem with a dot");
}

void TestEditBuild() {
    const erg::load::BaseFiles f = Base();
    erg::load::Loaded L;
    std::string err;
    erg::load::LoadScene(f, &L, &err);
    erg::Patch p = EmptyPatch(L, "ergtest_synth");
    auto op = [](erg::Op::Kind k) {
        erg::Op o;
        o.kind = k;
        return o;
    };
    erg::Op set = op(erg::Op::Kind::Set);
    set.src = 1;
    set.fields.pos = erg::Vec3{12.5, 3, -4};
    p.ops.push_back(set);
    erg::Op rename = op(erg::Op::Kind::Set);
    rename.src = 9;
    rename.fields.name = "VISIBLE_Box";
    rename.fields.rot = erg::Vec3{0, 1.25, 0};
    p.ops.push_back(rename);
    erg::Op rm = op(erg::Op::Kind::Remove);
    rm.src = 10;
    p.ops.push_back(rm);
    erg::Op rm2 = op(erg::Op::Kind::Remove);
    rm2.src = 5;
    p.ops.push_back(rm2);
    erg::Op add = op(erg::Op::Kind::Add);
    add.frame = 12;
    add.fields.name = "oildrum";
    add.fields.resource = "OilDrum";
    add.fields.pos = erg::Vec3{1.5, 2, 3};
    p.ops.push_back(add);
    erg::Op add2 = op(erg::Op::Kind::Add);
    add2.frame = 14;
    add2.fields.name = "mine";
    add2.fields.resource = "Landmine";
    add2.fields.pos = erg::Vec3{0.25, 0.5, 0.75};
    add2.fields.scale = erg::Vec3{2, 2, 2};
    p.ops.push_back(add2);
    p.databank.theme = "CAMELOT";
    p.databank.heightmapBase = "C01";
    p.water = 40.0;
    p.spawns = erg::SpawnMode::Knots;

    std::string text = erg::WritePatch(p);
    erg::Patch parsed;
    Expect(erg::ParsePatch(text, &parsed, &err), "edit: the patch parses " + err);
    erg::Scene s;
    erg::build::VoxelEdits v;
    Expect(!erg::build::Apply(L, parsed, {false, false}, &s, &v, &err) && err.find("ops[5]") != std::string::npos,
           "edit: without anyFrame the add to a plain frame is refused at its op index");
    Expect(erg::build::Apply(L, parsed, {false, true}, &s, &v, &err), "edit: apply " + err);
    std::vector<erg::build::File> files;
    Expect(erg::build::Build(L, s, v, {}, &files, &err), "edit: build " + err);

    erg::load::BaseFiles out = f;
    out.xan = FileBytes(files, "Maps/ergtest_synth.xan");
    out.xom = FileBytes(files, "ergtest_synth.XOM");
    erg::load::Loaded B;
    Expect(erg::load::LoadScene(out, &B, &err), "edit: the built level loads strictly " + err);
    const erg::Scene& b = B.scene;
    Expect(b.details.size() == 11, "edit: 11 - 2 + 2 details");
    Expect(ByName(b, "WORM0") && ByName(b, "WORM0")->pos == erg::Vec3{12.5, 3, -4}, "edit: WORM0 moved");
    Expect(ByName(b, "WORM1") && ByName(b, "WORM1")->pos == erg::Vec3{2, 2, 3}, "edit: WORM1 untouched");
    Expect(!ByName(b, "WORM4") && !ByName(b, "Diner6"), "edit: removed details are gone");
    const erg::Detail* box = ByName(b, "VISIBLE_Box");
    Expect(box && std::fabs(box->rot[1] - 1.25) < 1e-6 && box->pos == erg::Vec3{0.5, 1.25, 0.75} && box->resource == "BUILDING4",
           "edit: renamed and rotated in place");
    const erg::Detail* drum = ByName(b, "oildrum");
    Expect(drum && drum->frame == b.frames[1].id && b.frames[1].name == "Worms" && drum->pos == erg::Vec3{1.5, 2, 3} &&
               drum->role == erg::Role::Object,
           "edit: oildrum added to Worms");
    const erg::Detail* mine = ByName(b, "mine");
    const erg::Frame* ledge = nullptr;
    for (const auto& fr : b.frames)
        if (fr.name == "ledge") ledge = &fr;
    Expect(mine && ledge && mine->frame == ledge->id && mine->scale == erg::Vec3{2, 2, 2}, "edit: mine added to ledge (a non-folder frame)");
    size_t same = 0;
    for (const auto& fr : b.frames)
        for (const auto& ofr : L.scene.frames)
            if (fr.name == ofr.name && fr.size == ofr.size && fr.pos == ofr.pos && fr.rot == ofr.rot) {
                ++same;
                if (ofr.voxels > 0) Expect(B.blobs.at(fr.voxels) == L.blobs.at(ofr.voxels), "edit: voxels of " + fr.name + " unchanged");
            }
    Expect(same == 4, "edit: every frame kept");
    Expect(b.databank.theme == "CAMELOT" && b.databank.heightmapBase == "C01" && b.databank.timeOfDay == "NIGHT", "edit: databank written");
    Expect(b.databank.materialFile == "Maps\\synth.txt", "edit: material file untouched");

    const erg::Patch d = erg::build::Diff(L.scene, s);
    erg::Scene s2;
    erg::build::VoxelEdits v2;
    Expect(erg::build::Apply(L, d, {false, true}, &s2, &v2, &err) && erg::WriteScene(s2) == erg::WriteScene(s), "diff: re-applies to the same scene " + err);
    Expect(d.ops.size() == 6, "diff: 2 removes, 2 sets, 2 adds");

    xom::Document x;
    xom::ParseOptions strict;
    strict.strict = true;
    Expect(xom::parse(out.xan.data(), out.xan.size(), x, &err, strict), "edit: strict parse");
    size_t des = 0;
    bool grouped = true;
    for (size_t i = 0; i < x.objects.size(); ++i) {
        if (x.objects[i].type == "DetailEntityStore") {
            ++des;
            grouped &= i + 1 == des;
        }
    }
    Expect(des == 11 && grouped, "edit: details stay grouped before frames");
}

void TestScale() {
    const erg::load::BaseFiles f = Base();
    erg::load::Loaded L;
    std::string err;
    erg::load::LoadScene(f, &L, &err);
    erg::Patch p = EmptyPatch(L, "ergtest_synth");
    for (int64_t src : {5, 10}) {
        erg::Op rm;
        rm.kind = erg::Op::Kind::Remove;
        rm.src = src;
        p.ops.push_back(rm);
    }
    for (size_t i = 0; p.ops.size() < erg::kMaxOps; ++i) {
        erg::Op add;
        add.kind = erg::Op::Kind::Add;
        add.frame = 12;
        add.fields.name = "mine";
        add.fields.resource = "Landmine";
        add.fields.pos = erg::Vec3{static_cast<double>(i % 100), 1, static_cast<double>(i / 100)};
        p.ops.push_back(add);
    }
    const auto t0 = std::chrono::steady_clock::now();
    erg::Scene s;
    erg::build::VoxelEdits v;
    std::vector<erg::build::File> files;
    Expect(erg::build::Apply(L, p, {false, true}, &s, &v, &err) && erg::build::Build(L, s, v, {}, &files, &err),
           "scale: 20000 ops apply and build " + err);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Expect(ms < 5000, "scale: 20000 ops take " + std::to_string(static_cast<int>(ms)) + " ms");
    erg::load::BaseFiles out = f;
    out.xan = FileBytes(files, "Maps/ergtest_synth.xan");
    out.xom = FileBytes(files, "ergtest_synth.XOM");
    erg::load::Loaded B;
    Expect(erg::load::LoadScene(out, &B, &err) && B.scene.details.size() == 11 - 2 + erg::kMaxOps - 2,
           "scale: the built level loads with every added detail " + err);

    erg::Patch runs = EmptyPatch(L, "ergtest_synth");
    for (int k = 0; k < 40; ++k) {
        erg::Op op;
        op.kind = erg::Op::Kind::Voxels;
        op.frame = 1 + k;
        op.runs.assign(1000, erg::VoxelRun{0, static_cast<uint32_t>(erg::kMaxFrameVoxels), 0});
        runs.ops.push_back(op);
    }
    erg::Patch parsed;
    Expect(!erg::ParsePatch(erg::WritePatch(runs), &parsed, &err) && err.find("cover more than") != std::string::npos,
           "scale: overlapping runs past the covered-voxel limit are refused: " + err);
}

void TestVoxels() {
    const erg::load::BaseFiles f = Base();
    erg::load::Loaded L;
    std::string err;
    erg::load::LoadScene(f, &L, &err);
    erg::Patch p = EmptyPatch(L, "ergtest_synth");
    erg::Op op;
    op.kind = erg::Op::Kind::Voxels;
    op.frame = 13;
    op.runs = {{0, 4, 3 | 9 << 2}, {20, 4, 0}};
    p.ops.push_back(op);
    erg::Scene s;
    erg::build::VoxelEdits v;
    Expect(!erg::build::Apply(L, p, {false, true}, &s, &v, &err), "voxels: refused while the rules say so");
    Expect(erg::build::Apply(L, p, {true, true}, &s, &v, &err) && v.count(13) && v[13][0] == (3 | 9 << 2) && v[13][21] == 0, "voxels: runs applied");
    std::vector<erg::build::File> files;
    Expect(erg::build::Build(L, s, v, {}, &files, &err), "voxels: build " + err);
    erg::load::BaseFiles out = f;
    out.xan = FileBytes(files, "Maps/ergtest_synth.xan");
    erg::load::Loaded B;
    Expect(erg::load::LoadScene(out, &B, &err), "voxels: loads " + err);
    const std::vector<uint8_t>& a = L.blobs.at(L.scene.FindFrame(13)->voxels), &b = B.blobs.at(B.scene.FindFrame(13)->voxels);
    size_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff += a[i] != b[i];
    Expect(a.size() == b.size() && out.xan.size() == f.xan.size() && diff > 0 && diff <= 32, "voxels: patched in place, size unchanged");
    const erg::Patch d = erg::build::Diff(L.scene, s, v, &L.blobs);
    Expect(!d.ops.empty() && d.ops.back().kind == erg::Op::Kind::Voxels, "voxels: diff emits runs");
    erg::build::VoxelEdits bad = v;
    bad[13][1] = 0x01000003;
    Expect(!erg::build::Build(L, s, bad, {}, &files, &err), "voxels: a word with bits 24-31 set is refused");
}

// The synthetic base with its "tower" frame (#13) renamed Scene, so new frames may go under it.
erg::load::BaseFiles SceneBase() {
    erg::load::BaseFiles f = Base();
    xom::Document d;
    std::string err;
    xom::parse(f.xan.data(), f.xan.size(), d, &err);
    xu::SetStr(d.objects[12], "Name", "Scene");
    f.xan = Bytes(d);
    return f;
}

void TestNewFrames() {
    const erg::load::BaseFiles f = SceneBase();
    erg::load::Loaded L;
    std::string err;
    Expect(erg::load::LoadScene(f, &L, &err), "new frames: the base loads " + err);
    erg::Patch p = EmptyPatch(L, "ergtest_synth");
    erg::Op add;
    add.kind = erg::Op::Kind::AddFrame;
    add.newFrame = {-1, 13, "ergblock_0", {2, 1, 3.5}, {3, 2, 4}};
    erg::Op fill;
    fill.kind = erg::Op::Kind::Voxels;
    fill.frame = -1;
    fill.runs = {{0, 24, 3 | 5 << 2}, {1, 1, 0}};
    p.ops = {add, fill};
    erg::Patch back;
    Expect(erg::ParsePatch(erg::WritePatch(p), &back, &err) && back.UsesV2(), "new frames: the patch round-trips as /2 " + err);
    erg::Scene s;
    erg::build::VoxelEdits v;
    erg::PatchRules rules;
    rules.voxels = true;
    Expect(!erg::build::Apply(L, p, rules, &s, &v, &err) && err.find("new frames") != std::string::npos, "new frames: refused while the rules say so");
    rules.newFrames = true;
    Expect(erg::build::Apply(L, p, rules, &s, &v, &err), "new frames: applied " + err);
    const erg::Frame* nf = s.FindFrame(-1);
    Expect(nf && nf->isNew && nf->voxels >= 0 && v.count(-1) && v[-1].size() == 24 && v[-1][0] == (3 | 5 << 2) && v[-1][1] == 0 &&
               erg::ValidateScene(s, &err),
           "new frames: the frame has its own voxel blob, filled by the runs " + err);

    std::vector<erg::build::File> files;
    Expect(erg::build::Build(L, s, v, {}, &files, &err), "new frames: build " + err);
    erg::load::BaseFiles out = f;
    out.xan = FileBytes(files, "Maps/ergtest_synth.xan");
    erg::load::Loaded B;
    Expect(erg::load::LoadScene(out, &B, &err), "new frames: the built level loads " + err);
    const erg::Frame* built = nullptr;
    for (const auto& fr : B.scene.frames)
        if (fr.name == "ergblock_0") built = &fr;
    const erg::Frame* parent = built ? B.scene.FindFrame(built->parent) : nullptr;
    Expect(built && parent && parent->name == "Scene" && built->size == std::array<int, 3>{3, 2, 4} && built->pos == erg::Vec3{2, 1, 3.5} &&
               built->rot == erg::Vec3{0, 0, 0} && built->scale == erg::Vec3{1, 1, 1} && B.scene.frames.size() == L.scene.frames.size() + 1,
           "new frames: the block is a child of Scene with its size and centre");
    if (built) {
        std::vector<uint32_t> words;
        erg::voxels::Decode(B.blobs.at(built->voxels), &words, &err);
        const std::vector<uint8_t>& hm = B.blobs.at(built->heightMap);
        Expect(words == v[-1] && hm.size() == 4 * 5 * 4 && std::all_of(hm.begin(), hm.end(), [](uint8_t b) { return b == 0; }),
               "new frames: its voxels are the runs' and its height map is (X+1)(Z+1) zeros");
    }
    Expect(B.scene.details.size() == L.scene.details.size() && ByName(B.scene, "Diner6") && ByName(B.scene, "WORM7"),
           "new frames: every detail keeps its frame");

    const erg::Patch d = erg::build::Diff(L.scene, s, v, &L.blobs);
    erg::Scene s2;
    erg::build::VoxelEdits v2;
    Expect(d.ops.size() == 2 && d.ops[0].kind == erg::Op::Kind::AddFrame && erg::build::Apply(L, d, rules, &s2, &v2, &err) &&
               erg::WriteScene(s2) == erg::WriteScene(s) && v2[-1] == v[-1],
           "new frames: the diff re-applies to the same scene " + err);

    auto refused = [&](const char* name, int64_t parentId, const char* why) {
        erg::Patch q = p;
        q.ops[0].newFrame.name = name;
        q.ops[0].newFrame.parent = parentId;
        erg::Patch parsed;
        std::string e;
        const bool ok = erg::ParsePatch(erg::WritePatch(q), &parsed, &e) && erg::build::Apply(L, parsed, rules, &s2, &v2, &e);
        Expect(!ok && e.find(why) != std::string::npos, std::string("new frames: '") + name + "' is refused: " + e);
    };
    refused("SLIPPY_block", 13, "SLIPPY");
    refused("myPermBlock", 13, "PERM");
    refused("TeamBase1", 13, "TEAMBASE");
    refused("LEDGE", 13, "fresh");
    refused("ergblock_0", 12, "Scene frame");
}

void TestNegative() {
    const erg::load::BaseFiles f = Base();
    erg::load::Loaded L;
    std::string err;
    erg::load::LoadScene(f, &L, &err);
    erg::Scene s;
    erg::build::VoxelEdits v;
    erg::Patch p = EmptyPatch(L, "ergtest_synth");
    erg::Op set;
    set.kind = erg::Op::Kind::Set;
    set.src = 99;
    set.fields.pos = erg::Vec3{1, 1, 1};
    p.ops.push_back(erg::Op{});
    p.ops[0].kind = erg::Op::Kind::Remove;
    p.ops[0].src = 2;
    p.ops.push_back(set);
    Expect(!erg::build::Apply(L, p, {false, true}, &s, &v, &err) && err.find("ops[1].src") != std::string::npos, "negative: src out of range names op 1: " + err);
    p.ops[1].src = 2;
    Expect(!erg::build::Apply(L, p, {false, true}, &s, &v, &err) && err.find("removed") != std::string::npos, "negative: set after remove");
    p.ops.clear();
    p.base.sha256.xan[0] = p.base.sha256.xan[0] == 'a' ? 'b' : 'a';
    Expect(!erg::build::Apply(L, p, {false, true}, &s, &v, &err) && err.rfind("base.sha256", 0) == 0, "negative: a changed base is refused");
    erg::Patch q;
    const std::string good = erg::WritePatch(EmptyPatch(L, "ergtest_synth"));
    auto with = [&](const std::string& from, const std::string& to) {
        std::string t = good;
        t.replace(t.find(from), from.size(), to);
        return t;
    };
    Expect(!erg::ParsePatch(with("ergtest_synth", "ergtest_syn.th"), &q, &err), "negative: a stem with a dot");
    Expect(!erg::ParsePatch(with("ergtest_synth", "multi_dinermight"), &q, &err) && err.find("vanilla") != std::string::npos, "negative: a vanilla stem");
    Expect(!erg::ParsePatch(with("\"ops\":[]", "\"ops\":[{\"op\":\"voxels\",\"frame\":13,\"runs\":[[0,1,16777219]]}]"), &q, &err),
           "negative: a voxel value past bit 23");
}

void TestBank() {
    const std::vector<uint8_t> sb = SyntheticScripts();
    xom::Document sd;
    xom::ParseOptions strict;
    strict.strict = true;
    std::string err;
    Expect(xom::parse(sb.data(), sb.size(), sd, &err, strict), "bank: synthetic SCRIPTS parses " + err);
    std::vector<erg::bank::Entry> entries = {{"Multi.mymaps_a", "mymaps_a", "FETXT.mymaps_a", "stdvs,wormpot", 0, 5},
                                             {"Multi.mymaps_b", "mymaps_b", "FETXT.mymaps_b", "stdvs,wormpot,mymaps_b", 0, 5}};
    const std::vector<uint8_t> bank = erg::bank::RegistryBank(sd, entries, &err);
    Expect(!bank.empty(), "bank: built " + err);
    xom::Document b;
    Expect(xom::parse(bank.data(), bank.size(), b, &err, strict), "bank: parses strictly");
    Expect(b.objects.size() == 5 && b.root == 3 && b.object(3)->type == "XDataBank", "bank: 2 resources, the bank, 2 entries");
    Expect(xu::Str(b.objects[0], "Name") == "Multi.mymaps_a" && b.objects[0].field("Value")->asRef() == 4, "bank: key -> entry");
    Expect(xu::Str(b.objects[4], "Level_FileName") == "mymaps_b" && xu::Str(b.objects[4], "Level_ScriptName") == "stdvs,wormpot,mymaps_b" &&
               xu::Str(b.objects[4], "Frontend_Name") == "FETXT.mymaps_b" && xu::Str(b.objects[4], "Lock").empty() &&
               xu::Int(b.objects[4], "Theme_Type") == 5 && xu::Int(b.objects[4], "Level_Type") == 0,
           "bank: entry fields");
    Expect(b.object(3)->field("ContainerResources")->items.size() == 2, "bank: resources listed");
    Expect(b.strings.front().empty() && std::is_sorted(b.strings.begin() + 1, b.strings.end()), "bank: strings sorted after ''");
    entries[1].stem = "my.maps";
    Expect(erg::bank::RegistryBank(sd, entries, &err).empty(), "bank: a stem with a dot is refused");
    entries[1] = entries[0];
    Expect(erg::bank::RegistryBank(sd, entries, &err).empty(), "bank: a key twice is refused");
    Expect(erg::bank::RegistryBank(sd, {}, &err).empty(), "bank: no entries is refused");
}

// ---------------------------------------------------------------- the store and the service over a temp install
std::wstring TempDir() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + L"erg_level_selftest_" + std::to_wstring(GetCurrentProcessId());
    erg::install::MakeDirs(d);
    return d;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = dir + L"\\" + n;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>* out) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    out->clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->insert(out->end(), buf, buf + n);
    std::fclose(f);
    return true;
}

void Put(const std::wstring& path, const std::vector<uint8_t>& b) {
    erg::install::MakeDirs(path.substr(0, path.find_last_of(L'\\')));
    std::string err;
    Expect(erg::install::WriteAtomic(path, b.data(), b.size(), &err), "write " + err);
}

void Put(const std::wstring& path, const std::string& s) { Put(path, std::vector<uint8_t>(s.begin(), s.end())); }

void TestStore(const std::wstring& root) {
    erg::project::Store a(root + L"\\projA"), b(root + L"\\projA");
    std::string err;
    Expect(erg::project::ValidId("harbour1") && !erg::project::ValidId("Harbour") && !erg::project::ValidId("a_b") &&
               !erg::project::ValidId(std::string(25, 'a')),
           "store: ids");
    Expect(a.Create("harbour", "{}", {"Harbour", erg::project::NowIso(), "", ""}, &err), "store: create " + err);
    Expect(!a.Create("harbour", "{}", {}, &err), "store: create twice refused");
    Expect(a.FreeId("harbour") == "harbour2" && a.FreeId("fresh") == "fresh", "store: free ids");
    Expect(a.WritePatch("harbour", "{\"x\":1}", &err), "store: write");
    std::string text;
    Expect(a.ReadPatch("harbour", &text, &err) && text == "{\"x\":1}", "store: read back");
    Expect(a.Lock("harbour") == erg::project::LockResult::Ok && a.Lock("harbour") == erg::project::LockResult::Ok, "store: lock (idempotent)");
    Expect(b.Lock("harbour") == erg::project::LockResult::Busy, "store: another server gets busy");
    Expect(b.Lock("nothere") == erg::project::LockResult::Missing, "store: a missing project");
    a.Unlock("harbour");
    Expect(b.Lock("harbour") == erg::project::LockResult::Ok, "store: free after unlock");
    b.Unlock("harbour");
    Expect(!erg::install::Exists(root + L"\\projA\\harbour\\.lock"), "store: the lock file goes with the handle");

    // Leases: two connections on the same store both hold the project; only the last Unlock (or ReleaseConn) frees it.
    Expect(a.Lock("harbour", 1) == erg::project::LockResult::Ok, "lease: connection 1 opens the project");
    Expect(a.Lock("harbour", 2) == erg::project::LockResult::Ok, "lease: connection 2 also leases it (idempotent open)");
    Expect(b.Lock("harbour") == erg::project::LockResult::Busy, "lease: another server still sees it busy");
    a.Unlock("harbour", 1);
    Expect(b.Lock("harbour") == erg::project::LockResult::Busy, "lease: connection 2's lease keeps it locked");
    a.ReleaseConn(2);
    Expect(b.Lock("harbour") == erg::project::LockResult::Ok, "lease: releasing the last connection frees it");
    b.Unlock("harbour");
    Expect(a.Lock("harbour", 3) == erg::project::LockResult::Ok, "lease: reopen under a third connection");
    a.ReleaseConn(3);
    Expect(!erg::install::Exists(root + L"\\projA\\harbour\\.lock"), "lease: ReleaseConn closes the file when it was the last lease");
    Expect(a.List().size() == 1 && a.List()[0].title.empty(), "store: listed");
    Expect(!a.ReadPatch("..\\x", &text, &err) && !a.WritePatch("../x", "{}", &err), "store: ids never make paths");
}

struct Svc {
    std::unique_ptr<erg::service::Service> s;
    bool readOnly = false, inLobby = false, oasisReadOnly = false;
    std::vector<erg::install::Pack> packs;
};

erg::service::Reply Call(Svc& svc, const char* m, const std::string& params, uint64_t conn = 0) { return svc.s->Call(m, params, conn); }

Json J(const std::string& text) {
    Json v;
    xom::ParseJson(text, v);
    return v;
}

void TestSurround() {
    namespace hmp = erg::hmp;
    const erg::load::BaseFiles f = Base();
    erg::load::Loaded L;
    std::string err;
    erg::load::LoadScene(f, &L, &err);

    hmp::Surround s0;
    Expect(hmp::Read(*f.hmp, &s0, &err) && hmp::Write(s0) == *f.hmp, "hmp: read and write round-trip");
    Expect(!hmp::Read(std::vector<uint8_t>(49999), &s0, &err), "hmp: a short .hmp is refused");
    hmp::Surround s1 = s0;
    Expect(!hmp::ApplyRuns(s1, {{0, 2, 0.5}, {9999, 2, 0.5}}, {}, &err) && s1.heights == s0.heights, "hmp: a run past the cells changes nothing");
    Expect(!hmp::ApplyRuns(s1, {{0, 1, 1.5}}, {}, &err) && !hmp::ApplyRuns(s1, {}, {{0, 1, 2.5}}, &err), "hmp: out-of-range values are refused");

    erg::Patch p = EmptyPatch(L, "ergtest_synth");
    p.hmp = erg::HmpMode::Paint;
    erg::Op op;
    op.kind = erg::Op::Kind::Hmp;
    op.heights = {{0, 100, 1.0}, {50, 10, 0.25}};
    op.blend = {{9990, 10, 200}};
    p.ops.push_back(op);
    erg::PatchRules rules;
    rules.anyFrame = true;
    erg::Scene s;
    erg::build::VoxelEdits v;
    Expect(!erg::build::Apply(L, p, rules, &s, &v, &err) && err.find("surround") != std::string::npos, "paint: refused while the rules say so");
    rules.hmpPaint = true;
    Expect(erg::build::Apply(L, p, rules, &s, &v, &err) && v.hmp && s.hmpRef >= 0 && erg::ValidateScene(s, &err), "paint: applied " + err);
    hmp::Surround got;
    Expect(v.hmp && hmp::Read(*v.hmp, &got, &err) && got.heights[0] == 1.0f && got.heights[55] == 0.25f && got.heights[60] == 1.0f &&
               got.heights[100] == s0.heights[100] && got.blend[9995] == 200 && got.blend[0] == s0.blend[0],
           "paint: runs over the base's surround, in order");
    std::vector<erg::build::File> files;
    Expect(erg::build::Build(L, s, v, {}, &files, &err) && FileBytes(files, "Maps/ergtest_synth.hmp") == *v.hmp, "paint: the painted .hmp is built " + err);
    erg::build::VoxelEdits none;
    Expect(!erg::build::Build(L, s, none, {}, &files, &err), "paint: a build without the painted surround is refused");

    const erg::Patch d = erg::build::Diff(L.scene, s, v, &L.blobs, &*f.hmp);
    erg::Scene s2;
    erg::build::VoxelEdits v2;
    Expect(!d.ops.empty() && d.ops.back().kind == erg::Op::Kind::Hmp && erg::build::Apply(L, d, rules, &s2, &v2, &err) && v2.hmp == v.hmp &&
               erg::WriteScene(s2) == erg::WriteScene(s),
           "paint: the diff re-applies to the same surround " + err);
    const erg::Patch same = erg::build::Diff(L.scene, s, v, &L.blobs, &*v.hmp);
    Expect(std::none_of(same.ops.begin(), same.ops.end(), [](const erg::Op& o) { return o.kind == erg::Op::Kind::Hmp; }),
           "paint: an unchanged surround emits no hmp op");

    erg::load::BaseFiles nf = f;
    nf.hmp.reset();
    erg::load::Loaded N;
    erg::load::LoadScene(nf, &N, &err);
    erg::Patch pn = EmptyPatch(N, "ergtest_synth");
    pn.hmp = erg::HmpMode::Paint;
    pn.ops.push_back(op);
    Expect(erg::build::Apply(N, pn, rules, &s, &v, &err) && v.hmp && hmp::Read(*v.hmp, &got, &err) && got.heights[100] == 0.0f &&
               got.heights[0] == 1.0f && got.blend[0] == 0,
           "paint: without a base .hmp the runs go over zeros " + err);
    erg::Patch pt = EmptyPatch(L, "ergtest_synth");
    pt.ops.push_back(op);
    pt.ops.back().kind = erg::Op::Kind::Hmp;
    Expect(erg::voxels::TerrainChanged(pt), "paint: an hmp op is a terrain change (the shadows go)");
}

void TestService(const std::wstring& root) {
    const std::wstring game = root + L"\\game", data = game + L"\\Data";
    const erg::load::BaseFiles f = SceneBase();   // "tower" is the Scene frame, so blocks can be added
    Put(data + L"\\Tweak\\SCRIPTS.XOM", SyntheticScripts());
    Put(data + L"\\Tweak\\WEAPTWK.XOM", SyntheticWeapons());
    Put(data + L"\\Language\\PC\\EngFE.xom", SyntheticStrings());
    Put(data + L"\\Maps\\Multi_Synth.xan", f.xan);
    Put(data + L"\\Maps\\Multi_Synth.hmp", *f.hmp);
    Put(data + L"\\MULTI_SYNTH.XOM", f.xom);
    Put(data + L"\\Maps\\synth.txt", std::string("B01\n"));
    Put(data + L"\\Themes\\ThemeCamelot\\ThemeCamelot.txt", std::string("C01\n"));

    auto svc = std::make_shared<Svc>();
    erg::service::Env env;
    env.gameDir = game;
    env.projectsDir = root + L"\\projects";
    Svc* raw = svc.get();
    env.packs = [raw] { return raw->packs; };
    env.modsReadOnly = [raw] { return raw->readOnly; };
    env.modActive = [](const std::string&) { return false; };
    env.crcCollides = [](const std::string& n) { return _stricmp(n.c_str(), "SCRIPTS.XOM") == 0; };
    env.inSession = [raw](const std::string&) { return raw->inLobby; };
    env.readOnly = [raw] { return raw->oasisReadOnly; };
    // A stand-in for the engine's parser: "= =" is its one syntax error.
    env.compile = [](const std::string& text, int* line, std::string* message) {
        const size_t at = text.find("= =");
        if (at == std::string::npos) return true;
        *line = 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<ptrdiff_t>(at), '\n'));
        *message = "unexpected symbol near `='";
        return false;
    };
    svc->s = std::make_unique<erg::service::Service>(env);
    Svc& S = *svc;

    auto r = Call(S, "level.list", "{}");
    Json list = J(r.json);
    const Json* bases = list.find("bases");
    Expect(r.ok && bases && bases->arr.size() == 1, "list: one multiplayer base with files (" + r.json.substr(0, 200) + ")");
    if (bases && !bases->arr.empty()) {
        const Json& b0 = bases->arr[0];
        Expect(b0.find("key")->str == "Multi.Synth" && b0.find("title")->str == "Synthetic Harbour" && b0.find("theme")->str == "BUILDING" &&
                   b0.find("source")->str == "game" && b0.find("stem")->str == "Multi_Synth",
               "list: the base's fields");
    }

    r = Call(S, "level.load", R"({"base":"Multi.Synth"})");
    erg::Scene scene;
    std::string err;
    Expect(r.ok && erg::ParseScene(r.json, &scene, &err) && scene.stem == "Multi_Synth", "load base: a valid scene " + err + r.message);
    Expect(r.blobs.size() == scene.blobs.size() && r.blobs.size() == 6, "load base: one blob per scene blob");
    bool blobsOk = true;
    for (size_t i = 0; i < r.blobs.size() && i < scene.blobs.size(); ++i)
        blobsOk &= r.blobs[i].ref == scene.blobs[i].ref && r.blobs[i].bytes.size() == scene.blobs[i].bytes &&
                   J(r.blobs[i].meta).find("kind")->str == scene.blobs[i].kind;
    Expect(blobsOk, "load base: blob refs, sizes and meta");
    {
        const auto again = Call(S, "level.load", R"({"base":"Multi.Synth"})");
        erg::Scene s2;
        bool disjoint = again.ok && erg::ParseScene(again.json, &s2, &err) && again.blobs.size() == r.blobs.size();
        for (const auto& a : again.blobs)
            for (const auto& b : r.blobs) disjoint &= a.ref != b.ref;
        for (size_t i = 0; disjoint && i < again.blobs.size(); ++i) disjoint &= again.blobs[i].bytes == r.blobs[i].bytes;
        const erg::Frame* fr = s2.frames.empty() ? nullptr : &s2.frames.back();
        Expect(disjoint && erg::ValidateScene(s2, &err) && fr, "load: a second load sends the same blobs under new refs " + err);
    }
    Expect(!Call(S, "level.load", R"({"base":"Story.Synth"})").ok, "load: a story entry is not a base");
    Expect(Call(S, "level.load", R"({"base":"Multi.DinerMight"})").code == erg::service::kPolicy, "load: a base whose files are missing");
    Expect(Call(S, "level.load", R"({})").code == erg::service::kBadParams, "load: needs project or base");

    r = Call(S, "level.new", R"({"base":"Multi.Synth","slug":"harbour","title":"Harbour Brawl"})");
    Json created = J(r.json);
    Expect(r.ok && created.find("id")->str == "harbour" && created.find("stem")->str == "ergtest_harbour", "new: project " + r.message);
    r = Call(S, "level.new", R"({"base":"Multi.Synth","slug":"harbour","title":"Second"})");
    Expect(r.ok && J(r.json).find("id")->str == "harbour2", "new: a free id");
    Expect(Call(S, "level.new", R"({"base":"Multi.Synth","slug":"Bad_Slug","title":"x"})").code == erg::service::kBadParams, "new: bad slug");
    Expect(Call(S, "level.new", R"({"base":"Multi.Nope","slug":"x","title":"x"})").code == erg::service::kBadParams, "new: unknown base");

    r = Call(S, "level.load", R"({"project":"harbour"})");
    Expect(r.ok && erg::ParseScene(r.json, &scene, &err) && scene.stem == "ergtest_harbour" && scene.title == "Harbour Brawl",
           "load project: the scene " + r.message);
    {
        const Json loaded = J(r.json);
        const Json* pv = loaded.find("previews");
        Expect(pv && pv->kind == Json::Kind::Object, "load: the reply carries a previews object " + r.json.substr(0, 200));
    }

    const Json* patchJ = created.find("patch");
    std::string patchText = patchJ ? melange::erg::jsonio::Compact(*patchJ) : "";
    erg::Patch p;
    Expect(erg::ParsePatch(patchText, &p, &err) && p.ops.empty(), "new: an empty patch " + err);
    erg::Op set;
    set.kind = erg::Op::Kind::Set;
    set.src = 1;
    set.fields.pos = erg::Vec3{12.5, 3, -4};
    p.ops.push_back(set);
    p.spawns = erg::SpawnMode::Knots;
    p.databank.materialFile = "ThemeCamelot\\ThemeCamelot.txt";
    r = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(p) + "}");
    Json saved = J(r.json);
    Expect(r.ok && saved.find("saved")->boolean && saved.find("warnings")->arr.empty(), "save: saved without warnings " + r.message);
    r = Call(S, "level.load", R"({"project":"harbour"})");
    Expect(r.ok && erg::ParseScene(r.json, &scene, &err) && ByName(scene, "WORM0")->pos == erg::Vec3{12.5, 3, -4} &&
               scene.databank.materialFile == "ThemeCamelot\\ThemeCamelot.txt",
           "save: the edit loads back");

    auto refused = [&](erg::Patch q, int code, const char* what, const char* needle) {
        auto rr = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(q) + "}");
        Expect(!rr.ok && rr.code == code && rr.message.find(needle) != std::string::npos, std::string("save refuses ") + what + ": " + rr.message);
    };
    erg::Patch q = p;
    q.ops[0].src = 999;
    refused(q, erg::service::kBadParams, "an unknown src", "ops[0]");
    q = p;
    q.databank.materialFile = "ThemeNope\\ThemeNope.txt";
    refused(q, erg::service::kBadParams, "a material file not in the install", "materialFile");
    q = p;
    q.stem = "ergtest_other";
    refused(q, erg::service::kBadParams, "another stem", "stem");
    q = p;
    q.base.sha256.xom = std::string(64, 'a');
    refused(q, erg::service::kPolicy, "a base that changed", "sha256");
    q = p;
    q.ops.clear();
    q.ops.push_back(erg::Op{});
    q.ops[0].kind = erg::Op::Kind::Remove;
    q.ops[0].src = 3;
    q.spawns = erg::SpawnMode::Knots;
    r = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(q) + "}");
    Expect(r.ok && J(r.json).find("warnings")->arr.size() == 1, "save: a missing knot is a warning");
    r = Call(S, "level.load", R"({"project":"harbour"})");
    Expect(r.ok && erg::ParseScene(r.json, &scene, &err) && !ByName(scene, "WORM2") && ByName(scene, "WORM0")->pos == erg::Vec3{1, 2, 3},
           "save: the last save wins");
    Expect(Call(S, "level.save", R"({"project":"harbour","patch":{"format":"erg-patch/1"}})").code == erg::service::kBadParams,
           "save: a malformed patch");
    Expect(Call(S, "level.save", R"({"project":"nope","patch":)" + erg::WritePatch(p) + "}").code == erg::service::kBadParams,
           "save: an unknown project");
    {
        auto sr = Call(S, "level.load", R"({"base":"Multi.Synth","surround":true})");
        erg::Scene ss;
        const bool parsed = sr.ok && erg::ParseScene(sr.json, &ss, &err);
        Expect(parsed && ss.hmp == erg::HmpMode::Copy && ss.hmpRef >= 0 && sr.blobs.size() == 7 && sr.blobs.back().ref == ss.hmpRef &&
                   std::vector<uint8_t>(sr.blobs.back().bytes.begin(), sr.blobs.back().bytes.end()) == *f.hmp,
               "load base with surround: the base's .hmp as an hmp blob " + err + sr.message);
        Expect(Call(S, "level.load", R"({"base":"Multi.Synth","surround":1})").code == erg::service::kBadParams, "load: surround is a boolean");
        erg::Patch paint = q;
        paint.hmp = erg::HmpMode::Paint;
        erg::Op op;
        op.kind = erg::Op::Kind::Hmp;
        op.heights = {{0, 3, 0.5}};
        paint.ops.push_back(op);
        sr = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(paint) + "}");
        Expect(sr.ok, "save: a painted surround " + sr.message);
        sr = Call(S, "level.load", R"({"project":"harbour"})");
        float h0 = 0, h3 = 0;
        const bool back = sr.ok && erg::ParseScene(sr.json, &ss, &err) && ss.hmp == erg::HmpMode::Paint && !sr.blobs.empty() &&
                          sr.blobs.back().ref == ss.hmpRef && sr.blobs.back().bytes.size() == erg::kHmpBytes;
        if (back) {
            std::memcpy(&h0, sr.blobs.back().bytes.data(), 4);
            std::memcpy(&h3, sr.blobs.back().bytes.data() + 12, 4);
        }
        Expect(back && h0 == 0.5f && h3 == 0.03f, "load project: the painted surround's blob " + err + sr.message);
        sr = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(q) + "}");
        Expect(sr.ok, "save: back to the copied surround " + sr.message);

        erg::Patch block = q;
        erg::Op add, fill;
        add.kind = erg::Op::Kind::AddFrame;
        add.newFrame = {-1, 13, "ergblock_0", {1, 2.5, 0}, {2, 1, 2}};
        fill.kind = erg::Op::Kind::Voxels;
        fill.frame = -1;
        fill.runs = {{0, 4, 3 | 2 << 2}};
        block.ops.push_back(add);
        block.ops.push_back(fill);
        sr = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(block) + "}");
        Expect(sr.ok, "save: a new block " + sr.message);
        sr = Call(S, "level.load", R"({"project":"harbour"})");
        const erg::Frame* nf = sr.ok && erg::ParseScene(sr.json, &ss, &err) ? ss.FindFrame(-1) : nullptr;
        const auto blob = nf ? std::find_if(sr.blobs.begin(), sr.blobs.end(), [&](const auto& b) { return b.ref == nf->voxels; })
                             : sr.blobs.end();
        Expect(nf && nf->isNew && blob != sr.blobs.end() && blob->bytes.size() == 16 && static_cast<uint8_t>(blob->bytes[0]) == (3 | 2 << 2),
               "load project: the block and its voxels come back " + err + sr.message);
        sr = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(q) + "}");
        Expect(sr.ok, "save: without the block " + sr.message);
    }

    {
        r = Call(S, "level.objects", "{}");
        Json ob = J(r.json);
        Expect(r.ok && ob.find("weapons")->arr.size() == 1 && ob.find("weapons")->arr[0].str == "kWeaponBazooka" &&
                   ob.find("utilities")->arr.size() == 1 && ob.find("crateKinds")->arr.size() == 3 && ob.find("error")->kind == Json::Kind::Null,
               "objects: the install's catalog " + r.json.substr(0, 200));
        erg::Patch o = p;
        auto addKnot = [&](const char* name, erg::Vec3 pos) {
            erg::Op a;
            a.kind = erg::Op::Kind::Add;
            a.frame = 12;
            a.fields.name = name;
            a.fields.resource = "Unit";
            a.fields.pos = pos;
            o.ops.push_back(a);
        };
        addKnot("CRATE_0", {1, 1, 1});
        addKnot("TP_1_0", {2, 1, 1});
        addKnot("minefactory", {3, 1, 1});
        erg::ObjectSpec c, t, mf;
        c.knot = "CRATE_0";
        c.crate.contents = "kWeaponBazooka";
        t.type = erg::ObjectType::Telepad;
        t.knot = "TP_1_0";
        t.group = 1;
        mf.type = erg::ObjectType::MineFactory;
        mf.knot = "minefactory";
        o.objects = {c, t, mf};
        r = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(o) + "}");
        Json w = J(r.json);
        Expect(r.ok && w.find("warnings")->arr.size() == 1 && w.find("warnings")->arr[0].str.find("telepad group 1") != std::string::npos,
               "objects: saved as v2, a lone pad warns " + r.message + r.json.substr(0, 300));
        r = Call(S, "level.load", R"({"project":"harbour"})");
        const Json lj = r.ok ? J(r.json) : Json::Obj();
        const Json* lp = lj.find("previews");
        Expect(r.ok && erg::ParseScene(r.json, &scene, &err) && scene.objects.size() == 3 && ByName(scene, "TP_1_0") &&
                   ByName(scene, "TP_1_0")->role == erg::Role::Object && lp && lp->kind == Json::Kind::Object,
               "objects: the load reply keeps the objects, the knots and the previews " + err);
        const std::wstring test = root + L"\\testws";
        erg::install::MakeDirs(test);
        r = S.s->BuildTest("harbour", test);
        std::vector<uint8_t> lub, xan;
        std::string e2;
        Expect(r.ok && ReadAll(test + L"\\ergtest_harbour.lub", &lub) && ReadAll(test + L"\\Maps\\ergtest_harbour.xan", &xan),
               "objects: the test build writes a chunk " + r.message);
        const std::string chunk(lub.begin(), lub.end());
        Expect(chunk.find("ergCrate(\"CRATE_0\", \"weapon\", \"kWeaponBazooka\", 1, 25, 0)") != std::string::npos &&
                   chunk.find("lib_CreateTelepad(\"TP_1_0\", 1)") != std::string::npos &&
                   chunk.find("    scheme.MineFactoryOn = false\n") != std::string::npos &&
                   chunk.find("    SendMessage(\"GameLogic.PlaceObjects\")\n") != std::string::npos &&
                   erg::luagen::IsGenerated("ergtest_harbour", chunk, &e2),
               "objects: the chunk holds the crate, the pad and the factory, and verifies " + e2);
        xom::Document xd;
        Expect(xom::parse(xan.data(), xan.size(), xd, &e2), "objects: the built .xan parses " + e2);
        int knots = 0, lifted = 0;
        for (const auto& obj : xd.objects) {
            if (obj.type != "DetailEntityStore") continue;
            const std::string n = xu::Str(obj, "Name");
            Expect(n != "telepad", "objects: no detail named telepad");
            if (n == "CRATE_0" || n == "TP_1_0" || n == "minefactory") knots += xu::Str(obj, "ResourceName") == erg::kKnotResource;
            erg::Vec3 at{};
            if ((n == "CRATE_0" || n == "TP_1_0") && xu::GetVec(obj, "Position", &at))
                lifted += at[1] == (n == "TP_1_0" ? 1 + erg::build::kTelepadLift : 1);
        }
        Expect(knots == 3, "objects: every knot is the non-visual marker");
        Expect(lifted == 2, "objects: a telepad knot is written above its position, a crate knot at it");
        o.objects[0].crate.contents = "kWeaponClusterBomb";
        r = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(o) + "}");
        Expect(r.ok && J(r.json).find("warnings")->arr.size() == 2, "objects: unknown contents save with a warning");
        r = S.s->BuildTest("harbour", test);
        Expect(!r.ok && r.message.find("not a weapon of this install") != std::string::npos, "objects: and the build refuses them " + r.message);
        Expect(Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(q) + "}").ok, "objects: back to the last save");
    }

    {
        erg::project::Store other(root + L"\\projects");
        Expect(other.Lock("harbour") == erg::project::LockResult::Busy, "lock: another server sees the open project as busy");
        Expect(other.Lock("harbour2") == erg::project::LockResult::Ok, "lock: an unopened project is free");
        Expect(Call(S, "level.load", R"({"project":"harbour2"})").code == erg::service::kBusy, "lock: -32002 for a project another server holds");
    }
    Expect(Call(S, "level.close", R"({"project":"harbour"})").ok, "close");
    {
        erg::project::Store other(root + L"\\projects");
        Expect(other.Lock("harbour") == erg::project::LockResult::Ok, "close: the lock is released");
    }
    {
        // Leases: level.load records the caller's connection; the project stays locked while either connection
        // holds it, and a connection's close (ClientClosed) releases only its own lease.
        Expect(Call(S, "level.load", R"({"project":"harbour2"})", 101).ok, "lease: connection 101 opens harbour2");
        Expect(Call(S, "level.load", R"({"project":"harbour2"})", 102).ok, "lease: connection 102 also opens it");
        erg::project::Store other(root + L"\\projects");
        Expect(other.Lock("harbour2") == erg::project::LockResult::Busy, "lease: busy while either connection holds it");
        Expect(Call(S, "level.close", R"({"project":"harbour2"})", 101).ok, "lease: connection 101 closes");
        Expect(other.Lock("harbour2") == erg::project::LockResult::Busy, "lease: connection 102's lease still holds it");
        S.s->ClientClosed(102);
        Expect(other.Lock("harbour2") == erg::project::LockResult::Ok, "lease: the last connection's disconnect frees it");
        other.Unlock("harbour2");
        Expect(Call(S, "level.script.put", R"({"project":"harbour2","text":"-- lease\n"})", 103).ok, "lease: script.put leases for its connection");
        Expect(other.Lock("harbour2") == erg::project::LockResult::Busy, "lease: busy while the script writer's connection holds it");
        S.s->ClientClosed(103);
        Expect(other.Lock("harbour2") == erg::project::LockResult::Ok, "lease: the script writer's disconnect frees it");
        other.Unlock("harbour2");
        Expect(Call(S, "level.load", R"({"project":"harbour2"})", 104).ok, "lease: connection 104 opens harbour2");
        r = S.s->BuildTest("harbour2", root + L"\\testws", "", 104);
        Expect(r.ok, "lease: connection 104 tests it " + r.message);
        r = Call(S, "level.export", R"({"project":"harbour2","modId":"lease-export","name":"x","version":"1.0.0","mode":"source"})", 104);
        Expect(r.ok, "lease: connection 104 exports it " + r.message);
        Expect(Call(S, "level.close", R"({"project":"harbour2"})", 104).ok, "lease: connection 104 closes");
        Expect(other.Lock("harbour2") == erg::project::LockResult::Ok, "lease: Test and Export leave no lock after the close");
        other.Unlock("harbour2");
        RemoveTree(game + L"\\Mods\\lease-export");
    }

    r = Call(S, "level.themes", "{}");
    Json th = J(r.json);
    Expect(r.ok && th.find("themes")->arr.size() == 11 && th.find("timesOfDay")->arr.size() == 3 && th.find("materialFiles")->arr.size() == 2,
           "themes: eleven themes, three times, two material files " + r.json);
    r = Call(S, "level.palette", R"({"theme":"BUILDING"})");
    Json pal = J(r.json);
    bool toolbox = false, drum = false;
    size_t n = 0;
    if (r.ok)
        for (const auto& e : pal.find("entries")->arr) {
            ++n;
            toolbox |= e.find("resource")->str == "BUILDING4" && e.find("role")->str == "scenery" && e.find("name")->str == "VISIBLE_Toolbox";
            drum |= e.find("name")->str == "oildrum" && e.find("role")->str == "object";
        }
    Expect(r.ok && toolbox && drum && n == 11, "palette: knots, objects and the theme's scenery " + r.json.substr(0, 300));
    Expect(Call(S, "level.palette", R"({"theme":"MOON"})").code == erg::service::kBadParams, "palette: an unknown theme");

    // level.script.get / level.script.put, and the script in both export forms.
    {
        const std::wstring scriptPath = root + L"\\projects\\harbour\\script.lua";
        const std::string text = "wum.events.on(\"sim.turnStarted\", function() wum.log(wum.level.stem) end)\n";
        r = Call(S, "level.script.get", R"({"project":"harbour"})");
        Expect(r.ok && J(r.json).find("text")->str.empty(), "script.get: \"\" when there is none " + r.message);
        r = Call(S, "level.script.put", R"({"project":"harbour","text":"wum.events.on(\"sim.turnStarted\", function() wum.log(wum.level.stem) end)\n"})");
        Json put = J(r.json);
        Expect(r.ok && put.find("saved")->boolean && put.find("problems")->arr.empty() && put.find("syntaxChecked")->boolean,
               "script.put: saved and compiled " + r.message);
        r = Call(S, "level.script.get", R"({"project":"harbour"})");
        Expect(r.ok && J(r.json).find("text")->str == text, "script.get: reads it back");
        erg::Patch onDisk;
        std::string st;
        Expect(erg::project::Store(root + L"\\projects").ReadPatch("harbour", &st, &err) && erg::ParsePatch(st, &onDisk, &err) &&
                   onDisk.script.present && onDisk.script.sha256 == melange::hashutil::Sha256Hex(text.data(), text.size()),
               "script.put: the patch declares the script " + err);
        r = Call(S, "level.save", R"({"project":"harbour","patch":)" + erg::WritePatch(p) + "}");
        Expect(r.ok && erg::project::Store(root + L"\\projects").ReadPatch("harbour", &st, &err) && erg::ParsePatch(st, &onDisk, &err) &&
                   onDisk.script.present,
               "save: the script declaration follows script.lua, not the saved patch " + r.message);

        r = Call(S, "level.script.put", R"({"project":"harbour","text":"-- v2\nx = = 1\n"})");
        put = J(r.json);
        Expect(r.ok && put.find("saved")->boolean && put.find("problems")->arr.size() == 1 &&
                   put.find("problems")->arr[0].find("line")->numLiteral == "2" &&
                   put.find("problems")->arr[0].find("message")->str.find("near") != std::string::npos,
               "script.put: a syntax error is saved and reported on its line " + r.json);
        Expect(Call(S, "level.script.put", R"({"project":"harbour","text":"wum.events.on(\"sim.turnStarted\", function() wum.log(wum.level.stem) end)\n"})").ok,
               "script.put: back to the good script");
        r = Call(S, "level.script.put", R"({"project":"harbour","text":"-- ok\nlocal x = 1\n\u001bLua\n"})");
        put = J(r.json);
        Expect(r.ok && !put.find("saved")->boolean && put.find("problems")->arr.size() == 1 &&
                   put.find("problems")->arr[0].find("line")->numLiteral == "3" &&
                   put.find("problems")->arr[0].find("message")->str.find("ESC") != std::string::npos,
               "script.put: an ESC byte is a problem on its line " + r.json);
        r = Call(S, "level.script.put", R"({"project":"harbour","text":"﻿x = 1"})");
        put = J(r.json);
        Expect(r.ok && !put.find("saved")->boolean && put.find("problems")->arr[0].find("line")->numLiteral == "1", "script.put: a BOM is refused " + r.json);
        r = Call(S, "level.script.put", R"({"project":"harbour","text":")" + std::string(melange::levels::manifest::kMaxSimBytes + 1, 'x') + "\"}");
        put = J(r.json);
        Expect(r.ok && !put.find("saved")->boolean && put.find("problems")->arr[0].find("message")->str.find("256 KB") != std::string::npos,
               "script.put: larger than 256 KB is refused");
        r = Call(S, "level.script.get", R"({"project":"harbour"})");
        Expect(r.ok && J(r.json).find("text")->str == text, "script.put: a refused text leaves the saved script alone");
        Expect(Call(S, "level.script.put", R"({"project":"harbour"})").code == erg::service::kBadParams, "script.put: text is required");
        Expect(Call(S, "level.script.put", R"({"project":"../x","text":""})").code == erg::service::kBadParams, "script.put: a bad id");
        Expect(Call(S, "level.script.get", R"({"project":"nope"})").code == erg::service::kBadParams, "script.get: an unknown project");

        for (const char* mode : {"install", "source"}) {
            const std::string modId = std::string("script-") + mode;
            const std::wstring dir = game + L"\\Mods\\" + erg::install::Widen(modId);
            r = Call(S, "level.export", R"({"project":"harbour","modId":")" + modId + R"(","name":"x","version":"1.0.0","mode":")" + mode + "\"}");
            std::vector<uint8_t> sim, spice;
            Expect(r.ok && erg::install::ReadFile(dir + L"\\sim\\harbour.lua", 1u << 20, &sim, &err) &&
                       std::string(sim.begin(), sim.end()) == text && r.json.find("sim/harbour.lua") != std::string::npos,
                   std::string("export ") + mode + ": sim/<slug>.lua " + r.message);
            Expect(erg::install::ReadFile(dir + L"\\spice.json", 1u << 20, &spice, &err) &&
                       std::string(spice.begin(), spice.end()).find(R"("sim":"sim/harbour.lua")") != std::string::npos,
                   std::string("export ") + mode + ": levels[].sim");
        }
        r = Call(S, "level.script.put", R"({"project":"harbour","text":""})");
        Expect(r.ok && J(r.json).find("saved")->boolean && !erg::install::Exists(scriptPath), "script.put: \"\" removes the script");
        r = Call(S, "level.export", R"({"project":"harbour","modId":"script-source","name":"x","version":"1.0.0","mode":"source"})");
        Expect(r.ok && !erg::install::Exists(game + L"\\Mods\\script-source\\sim\\harbour.lua"), "export: a removed script is removed from the pack");
        RemoveTree(game + L"\\Mods\\script-install");
        RemoveTree(game + L"\\Mods\\script-source");
        Expect(Call(S, "level.close", R"({"project":"harbour"})").ok, "close after the script cases");
    }

    // level.build on a source pack in the temp install.
    const std::wstring mod = game + L"\\Mods\\my-maps";
    Put(mod + L"\\spice.json", std::string(R"({"spiceVersion":1,"id":"my-maps","version":"1.0.0","name":"My maps","melange":{"range":">=0.1.0"},)"
                                           R"("kind":"content","entry":{},"levels":[{"slug":"harbour","title":"Harbour Brawl","source":"src/harbour.ergpatch.json"},)"
                                           R"({"slug":"nosrc","title":"No Source"}]})"));
    erg::Patch pack = p;
    pack.stem = "my_maps_harbour";
    Put(mod + L"\\src\\harbour.ergpatch.json", erg::WritePatch(pack));
    r = Call(S, "level.build", R"({"modId":"my-maps"})");
    Json built = J(r.json);
    Expect(r.ok && built.find("levels")->arr.size() == 1 && built.find("skipped")->arr.size() == 1, "build: one level built, one skipped " + r.message);
    Expect(erg::install::Exists(mod + L"\\assets\\levels\\Maps\\my_maps_harbour.xan") &&
               erg::install::Exists(mod + L"\\assets\\levels\\my_maps_harbour.XOM") &&
               erg::install::Exists(mod + L"\\assets\\levels\\Maps\\my_maps_harbour.hmp"),
           "build: files under assets\\levels");
    std::vector<uint8_t> xan;
    erg::install::ReadFile(mod + L"\\assets\\levels\\Maps\\my_maps_harbour.xan", 1u << 26, &xan, &err);
    Expect(xan != f.xan && xan.size() == f.xan.size(), "build: the edit is in the built .xan");
    {
        const std::wstring elsewhere = root + L"\\elsewhere", assets = mod + L"\\assets";
        erg::install::MakeDirs(elsewhere);
        RemoveTree(assets);
        const std::wstring cmd = L"cmd /c mklink /J \"" + assets + L"\" \"" + elsewhere + L"\" >nul";
        if (_wsystem(cmd.c_str()) == 0) {
            r = Call(S, "level.build", R"({"modId":"my-maps"})");
            Expect(r.code == erg::service::kPolicy && !erg::install::Exists(elsewhere + L"\\levels\\my_maps_harbour.XOM"),
                   "build: never writes through a junction " + r.message);
            RemoveDirectoryW(assets.c_str());
        } else {
            std::printf("note: could not create a junction; the junction case was skipped\n");
        }
        r = Call(S, "level.build", R"({"modId":"my-maps"})");
        Expect(r.ok, "build: builds again once the junction is gone " + r.message);
    }
    {
        const std::wstring hmp = mod + L"\\assets\\levels\\Maps\\my_maps_harbour.hmp";
        erg::Patch flat = pack;
        flat.hmp = erg::HmpMode::None;
        Put(mod + L"\\src\\harbour.ergpatch.json", erg::WritePatch(flat));
        r = Call(S, "level.build", R"({"modId":"my-maps"})");
        Expect(r.ok && !erg::install::Exists(hmp), "build: a .hmp left from an earlier build is removed " + r.message);
        Put(mod + L"\\src\\harbour.ergpatch.json", erg::WritePatch(pack));
        r = Call(S, "level.build", R"({"modId":"my-maps"})");
        Expect(r.ok && erg::install::Exists(hmp), "build: the .hmp is back when the patch copies it " + r.message);
        S.inLobby = true;
        Expect(Call(S, "level.build", R"({"modId":"my-maps"})").code == erg::service::kReadOnly,
               "build: refused for a loaded pack while in a lobby");
        Expect(Call(S, "level.export", R"({"project":"harbour","modId":"my-maps","name":"x","version":"1.0.0","mode":"source"})").code ==
                   erg::service::kReadOnly,
               "export: refused for a loaded pack while in a lobby");
        S.inLobby = false;
    }
    {
        const std::wstring other = game + L"\\Mods\\data-maps";
        Put(other + L"\\spice.json", std::string(R"({"spiceVersion":1,"id":"data-maps","version":"1.0.0","name":"Data maps","melange":{"range":">=0.1.0"},)"
                                                 R"("kind":"content","assets":{"root":"data"},)"
                                                 R"("levels":[{"slug":"harbour","title":"Harbour","source":"src/h.ergpatch.json"}]})"));
        erg::Patch dp = pack;
        dp.stem = "data_maps_harbour";
        Put(other + L"\\src\\h.ergpatch.json", erg::WritePatch(dp));
        r = Call(S, "level.build", R"({"modId":"data-maps"})");
        Expect(r.ok && erg::install::Exists(other + L"\\data\\levels\\Maps\\data_maps_harbour.xan") &&
                   !erg::install::Exists(other + L"\\assets\\levels\\Maps\\data_maps_harbour.xan") &&
                   r.json.find("data/levels/Maps/data_maps_harbour.xan") != std::string::npos,
               "build: a pack's assets.root is where its levels go " + r.message);
        erg::install::Pack dpk;
        melange::spice::Manifest m;
        std::vector<melange::spice::Error> errs;
        Expect(melange::spice::Parse(other, &m, &errs) && erg::install::PackFromManifest(m, other, &dpk) &&
                   erg::install::LevelRoot(dpk) == other + L"\\data\\levels",
               "install: the level root follows assets.root");
        S.packs = {dpk};
        r = Call(S, "level.list", "{}");
        Expect(r.ok && r.json.find(R"("mod":"data-maps","built":true)") != std::string::npos, "list: a pack under assets.root reads as built " + r.json);
        S.packs.clear();
        RemoveTree(other);
    }
    S.readOnly = true;
    Expect(Call(S, "level.build", R"({"modId":"my-maps"})").code == erg::service::kReadOnly, "build: -32003 while Mods is read-only");
    Expect(Call(S, "level.export", R"({"project":"harbour","modId":"my-maps","name":"x","version":"1.0.0","mode":"install"})").code ==
               erg::service::kReadOnly,
           "export install: -32003 while Mods is read-only");
    S.readOnly = false;
    Expect(Call(S, "level.build", R"({"modId":"none"})").code == erg::service::kBadParams, "build: an unknown mod");
    pack.base.sha256.xan = std::string(64, 'b');
    Put(mod + L"\\src\\harbour.ergpatch.json", erg::WritePatch(pack));
    r = Call(S, "level.build", R"({"modId":"my-maps"})");
    Expect(r.code == erg::service::kPolicy && r.message.find("sha256") != std::string::npos, "build: refused when the base hashes differ " + r.message);

    // A pack level as a base, once the pack is enabled.
    erg::install::Pack pk;
    Expect(erg::install::ScanPacks(game).size() == 1, "packs: scanned");
    pk = erg::install::ScanPacks(game)[0];
    S.packs = {pk};
    r = Call(S, "level.list", "{}");
    Expect(r.ok && J(r.json).find("bases")->arr.size() == 3, "list: pack levels are bases");
    r = Call(S, "level.load", R"({"base":"Multi.my_maps_harbour","source":"pack"})");
    Expect(r.ok && erg::ParseScene(r.json, &scene, &err) && scene.base.source == "pack" && ByName(scene, "WORM0")->pos == erg::Vec3{12.5, 3, -4},
           "load: a pack base " + r.message);

    r = Call(S, "level.export", R"({"project":"harbour","modId":"my-maps","name":"x","version":"1.0.0","mode":"install"})");
    Expect(!r.ok && r.code == erg::service::kPolicy && r.message.find("export") != std::string::npos, "export: refused while the pack writer is absent");
    Expect(Call(S, "level.export", R"({"project":"harbour","modId":"Bad Id","name":"x","version":"1","mode":"install"})").code ==
               erg::service::kBadParams,
           "export: a bad mod id");
    Expect(Call(S, "level.export", R"({"project":"harbour","modId":"ergtest","name":"x","version":"1","mode":"source"})").code ==
               erg::service::kBadParams,
           "export: the reserved prefix");
    Expect(Call(S, "level.close", R"({"project":"harbour"})").ok, "close before the read-only case");
    S.oasisReadOnly = true;
    r = Call(S, "level.load", R"({"project":"harbour"})");
    Expect(r.ok && !erg::install::Exists(root + L"\\projects\\harbour\\.lock"), "load: a read-only server opens a project without a lock");
    Expect(Call(S, "level.close", R"({"project":"harbour"})").ok, "close: read-only");
    S.oasisReadOnly = false;
    Expect(Call(S, "level.nope", "{}").code == -32601 && Call(S, "level.list", "[1]").code == erg::service::kBadParams, "unknown method, bad params");
    Expect(erg::service::Methods().size() == 12 && erg::service::Mutating("level.save") && erg::service::Mutating("level.script.put") && !erg::service::Mutating("level.script.get") && !erg::service::Mutating("level.load"), "methods");
}
}  // namespace

int main() {
    TestLoad();
    TestEmptyBuild();
    TestEditBuild();
    TestScale();
    TestVoxels();
    TestNewFrames();
    TestSurround();
    TestNegative();
    TestBank();
    TestObjects();
    const std::wstring root = TempDir();
    TestStore(root);
    TestService(root);
    RemoveTree(root);
    std::printf("erg_level_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
