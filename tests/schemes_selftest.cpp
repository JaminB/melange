// Offline self-test of the schemes module's data side: spice.json "schemes" / "factoryWeapons" parsing, and the
// DATA.LockedSchemes / DATA.LockedWeapons banks built from a synthetic LOCAL.XOM (wrapper, collective, schemes with
// their WeaponSettingsData, one preset graph) plus scheme and preset files written to a temp dir.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "erg/xomutil.h"
#include "mods/spice.h"
#include "schemes/builder.h"
#include "xom/xom.h"

namespace fs = std::filesystem;
namespace sc = melange::schemes;
namespace xu = melange::erg::xomutil;
using melange::xom::Object;
using melange::xom::Type;
using melange::xom::Value;

namespace {
int g_checks = 0, g_failed = 0;

void Check(bool ok, const char* what, const std::string& detail = {}) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
}

void Write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary | std::ios::trunc) << text;
}

// ---------------------------------------------------------------- the synthetic LOCAL.XOM
struct Local {
    melange::xom::Document doc;
    size_t weaponFields = 0;   // the Ref fields of a SchemeData (one WeaponSettingsData each)
};

uint32_t Add(melange::xom::Document& d, const char* cls, Object* out = nullptr) {
    Object o;
    std::string err;
    if (!xu::NewObject(d, cls, &o, &err)) printf("NewObject %s: %s\n", cls, err.c_str());
    d.objects.push_back(std::move(o));
    if (out) *out = d.objects.back();
    return static_cast<uint32_t>(d.objects.size());
}

Object& At(melange::xom::Document& d, uint32_t ref) { return d.objects[ref - 1]; }

uint32_t AddScheme(Local& l, const char* name, const char* lock, int ammo, uint32_t* firstWeapon = nullptr) {
    auto& d = l.doc;
    const uint32_t s = Add(d, "SchemeData");
    size_t n = 0;
    for (size_t i = 0; i < At(d, s).fields.size(); ++i) {
        Value& f = At(d, s).fields[i].second;
        if (f.type != Type::Ref || f.array) continue;
        const uint32_t w = Add(d, "WeaponSettingsData");
        At(d, w).field("Ammo")->setInt(ammo);
        At(d, w).field("Crate")->setInt(ammo + 1);
        At(d, w).field("Delay")->setInt(ammo + 2);
        At(d, s).fields[i].second.bits = w;
        if (!n && firstWeapon) *firstWeapon = w;
        ++n;
    }
    l.weaponFields = n;
    xu::SetStr(At(d, s), "Name", name);
    xu::SetStr(At(d, s), "Lock", lock);
    At(d, s).field("Permanent")->bits = 1;
    At(d, s).field("Wins")->setInt(2);
    At(d, s).field("RoundTime")->setInt(1200000);
    At(d, s).field("SuddenDeath")->setInt(1);
    return s;
}

// Types in LOCAL's order; objects grouped by them (wrappers, data bank, then the two resources' graphs).
Local MakeLocal() {
    Local l;
    auto& d = l.doc;
    {   // XContainer is not a schema class, so EnsureType cannot add it; LOCAL.XOM lists it first
        melange::xom::TypeEntry t;
        t.name = "XContainer";
        const uint8_t guid[16] = {0x46, 0xd4, 0x1c, 0x5e, 0xa3, 0x48, 0xfe, 0x44, 0xa5, 0x5a, 0xe2, 0x47, 0xb8, 0xf5, 0xe7, 0x13};
        std::copy(guid, guid + 16, t.guid.begin());
        std::copy(t.name.begin(), t.name.end(), t.rawName.begin());
        d.types.push_back(t);
    }
    for (const char* t : {"XResourceDetails", "XContainerResourceDetails", "XDataBank", "WeaponSettingsData", "SchemeData",
                          "SchemeColective", "WeaponFactoryContainer", "StoreWeaponFactory", "WeaponFactoryCollective"})
        xu::EnsureType(d, t, "");
    for (auto& t : d.types)
        if (t.name == "SchemeData") t.version = 2;
    d.strings = {""};
    const uint32_t wrapS = Add(d, "XContainerResourceDetails"), wrapW = Add(d, "XContainerResourceDetails");
    const uint32_t bank = Add(d, "XDataBank");
    d.root = bank;
    const uint32_t a = AddScheme(l, "FE.Scheme.Standard", "Lock.Scheme.Standard", 5);
    const uint32_t b = AddScheme(l, "FETXT.Scheme.Darksider", "Lock.Scheme.Darksider", 1);
    const uint32_t coll = Add(d, "SchemeColective");
    At(d, coll).field("Schemes")->items = {xu::RefValue(a), xu::RefValue(b)};
    At(d, coll).field("Schemes")->array = true;
    const uint32_t weapon = Add(d, "WeaponFactoryContainer"), cluster = Add(d, "WeaponFactoryContainer");
    xu::SetStr(At(d, weapon), "Name", "FETXT.WipeOut");
    At(d, weapon).field("WormDamageMagnitude")->setFloat(2.0);
    At(d, weapon).field("FuseTime")->setInt(3000);
    xu::SetStr(At(d, weapon), "DetonationFX", "WXP_Old");
    At(d, cluster).field("WormDamageMagnitude")->setFloat(1.0);
    const uint32_t store = Add(d, "StoreWeaponFactory");
    At(d, store).field("StockWeapon")->bits = 1;
    At(d, store).field("Weapon")->bits = weapon;
    At(d, store).field("Cluster")->bits = cluster;
    const uint32_t fcoll = Add(d, "WeaponFactoryCollective");
    At(d, fcoll).field("Weapons")->items = {xu::RefValue(store)};
    At(d, fcoll).field("Weapons")->array = true;
    xu::SetStr(At(d, wrapS), "Name", "DATA.LockedSchemes");
    At(d, wrapS).field("Flags")->setInt(113);
    At(d, wrapS).field("Value")->bits = coll;
    xu::SetStr(At(d, wrapW), "Name", "DATA.LockedWeapons");
    At(d, wrapW).field("Flags")->setInt(80);
    At(d, wrapW).field("Value")->bits = fcoll;
    return l;
}

bool ParseBank(const std::vector<uint8_t>& bytes, melange::xom::Document* d) {
    std::string err;
    melange::xom::ParseOptions opt;
    opt.strict = true;
    return melange::xom::parse(bytes.data(), bytes.size(), *d, &err, opt);
}

const Object* Find(const melange::xom::Document& d, const char* cls, const char* name) {
    for (const auto& o : d.objects)
        if (o.type == cls && xu::Str(o, "Name") == name) return &o;
    return nullptr;
}

bool HasError(const sc::Built& b, const char* needle) {
    for (const auto& e : b.errors)
        if (e.text.find(needle) != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------- schemes
const char* kKanly = R"({
  "key": "FETXT.Scheme.Kanly", "title": "Kanly", "lock": "Lock.Scheme.Standard",
  "fields": { "RoundTime": 300000, "SuddenDeath": 2, "MineFactoryOn": true },
  "weapons": { "ConcreteDonkey": { "Ammo": 1 }, "HealthMystery": { "Crate": 50 }, "*": { "Ammo": 10, "Delay": 0 } }
})";

void TestSchemes(const Local& l, const fs::path& dir) {
    Write(dir / "schemes" / "kanly.json", kKanly);
    const sc::Built b = sc::BuildSchemes(l.doc, {{"kanly", dir, {"schemes/kanly.json"}}});
    Check(b.fatal.empty() && b.errors.empty(), "a good scheme builds without errors", b.fatal + (b.errors.empty() ? "" : b.errors[0].text));
    Check(b.builtIn == 2 && b.added.size() == 1 && b.added[0].key == "FETXT.Scheme.Kanly" && b.added[0].title == "Kanly" &&
              b.added[0].mod == "kanly",
          "the built-in count and the text to register");
    melange::xom::Document d;
    Check(ParseBank(b.bank, &d), "the bank parses back (strict)");
    const size_t n = l.weaponFields;
    Check(n >= 58, "SchemeData has at least the 58 weapon fields", std::to_string(n));
    Check(d.objects.size() == 6 + 3 * n, "object count: wrapper, bank, collective, 3 schemes, 3*n settings", std::to_string(d.objects.size()));
    std::vector<std::string> names;
    for (const auto& t : d.types) names.push_back(t.className());
    const std::vector<std::string> want = {"XContainer", "XResourceDetails", "XContainerResourceDetails", "XDataBank",
                                           "WeaponSettingsData", "SchemeData", "SchemeColective"};
    { std::string j; for (auto& nm : names) j += nm + " "; Check(names == want, "TYPE table: only the classes in use, in LOCAL's order", j); }
    Check(d.types.size() == want.size() && d.types[2].count == 1 && d.types[3].count == 1 && d.types[4].count == 3 * n &&
              d.types[5].count == 3 && d.types[6].count == 1,
          "TYPE counts follow the grouping");
    Check(d.objects.size() > 2 && d.objects[0].type == "XContainerResourceDetails" && d.objects[1].type == "XDataBank" && d.root == 2,
          "wrapper #1, data bank #2 as the root");
    const Object& wrap = d.objects[0];
    Check(xu::Str(wrap, "Name") == "DATA.LockedSchemes" && xu::Int(wrap, "Flags") == 113, "the wrapper keeps its Name and Flags");
    const Object& bank = d.objects[1];
    Check(xu::Int(bank, "Section", -1) == 0 && bank.field("ContainerResources")->items.size() == 1 &&
              bank.field("ContainerResources")->items[0].asRef() == 1,
          "data bank: Section 0, ContainerResources [1]");
    const Object* coll = d.object(wrap.field("Value")->asRef());
    Check(coll && coll->type == "SchemeColective" && coll->field("Schemes")->items.size() == 3, "the collective holds n+1 = 3 refs");
    if (!coll || coll->field("Schemes")->items.size() != 3) return;
    const Object* s0 = d.object(coll->field("Schemes")->items[0].asRef());
    const Object* s1 = d.object(coll->field("Schemes")->items[1].asRef());
    const Object* s2 = d.object(coll->field("Schemes")->items[2].asRef());
    Check(s0 && s1 && s2 && xu::Str(*s0, "Name") == "FE.Scheme.Standard" && xu::Str(*s1, "Name") == "FETXT.Scheme.Darksider" &&
              xu::Str(*s2, "Name") == "FETXT.Scheme.Kanly",
          "built-ins keep their order, the new scheme comes last");
    if (!s0 || !s1 || !s2) return;
    Check(s2->field("Permanent")->asBool() && xu::Str(*s2, "Lock") == "Lock.Scheme.Standard", "Permanent true and the lock from the file");
    Check(xu::Int(*s2, "RoundTime") == 300000 && xu::Int(*s2, "SuddenDeath") == 2 && s2->field("MineFactoryOn")->asBool() &&
              xu::Int(*s2, "Wins") == 2,
          "fields: overridden values and the base's others");
    auto weapon = [&](const Object& s, const char* name) {
        const Object* w = d.object(s.field(name)->asRef());
        return w;
    };
    const Object* cd = weapon(*s2, "ConcreteDonkey");
    const Object* hm = weapon(*s2, "HealthMystery");
    const Object* bz = weapon(*s2, "Bazooka");
    Check(cd && xu::Int(*cd, "Ammo") == 1 && xu::Int(*cd, "Delay") == 0 && xu::Int(*cd, "Crate") == 6,
          "per-entry beats \"*\" (Ammo 1), \"*\" still applies to Delay, Crate stays the base's");
    Check(hm && xu::Int(*hm, "Crate") == 50 && xu::Int(*hm, "Ammo") == 10, "HealthMystery: Crate 50 over \"*\" Ammo 10");
    Check(bz && xu::Int(*bz, "Ammo") == 10 && xu::Int(*bz, "Delay") == 0, "an untouched weapon gets \"*\"");
    const Object* b0 = weapon(*s0, "ConcreteDonkey");
    Check(b0 && xu::Int(*b0, "Ammo") == 5 && xu::Int(*b0, "Delay") == 7, "the base scheme's settings are not shared with the copy");
    Check(s0->field("Bazooka")->asRef() != s2->field("Bazooka")->asRef() && xu::Int(*s0, "RoundTime") == 1200000,
          "the base scheme is unchanged");
    std::vector<uint8_t> again;
    std::string err;
    Check(melange::xom::serialize(d, again, &err) && again == b.bank, "serialize(parse(bank)) is byte-identical");
    Check(l.doc.objects.size() == 10 + 2 * n && !Find(l.doc, "SchemeData", "FETXT.Scheme.Kanly"), "the input document is not modified");

    // Two mods, a bad file between good ones, a duplicate and a clash with a built-in key.
    Write(dir / "schemes" / "two.json", R"({"key": "FETXT.Scheme.Second", "title": "Second", "base": "FETXT.Scheme.Darksider", "fields": {"Wins": 5}})");
    Write(dir / "schemes" / "dup.json", R"({"key": "FETXT.Scheme.Kanly", "title": "Again"})");
    Write(dir / "schemes" / "vanilla.json", R"({"key": "FETXT.Scheme.Darksider", "title": "Mine"})");
    const sc::Built m = sc::BuildSchemes(l.doc, {{"a", dir, {"schemes/kanly.json", "schemes/dup.json"}},
                                                  {"b", dir, {"schemes/vanilla.json", "schemes/two.json"}}});
    Check(m.added.size() == 2 && m.added[0].key == "FETXT.Scheme.Kanly" && m.added[1].key == "FETXT.Scheme.Second",
          "load order is kept and refused entries are skipped");
    Check(m.errors.size() == 2 && m.errors[0].mod == "a" && m.errors[0].file == "schemes/dup.json" && HasError(m, "already used") &&
              m.errors[1].mod == "b",
          "a key used twice and a key clashing with a built-in are refused, each logged with mod and file");
    melange::xom::Document md;
    Check(ParseBank(m.bank, &md), "the two-mod bank parses");
    const Object* second = Find(md, "SchemeData", "FETXT.Scheme.Second");
    Check(second && xu::Int(*second, "Wins") == 5 && xu::Str(*second, "Lock") == "Lock.Scheme.Darksider", "base from another built-in, its lock kept");

    // No entries: nothing, unless the empty bank is asked for (the vanilla list put back).
    const sc::Built none = sc::BuildSchemes(l.doc, {});
    Check(none.bank.empty() && none.fatal.empty() && none.builtIn == 2, "no entries: no bank");
    const sc::Built plain = sc::BuildSchemes(l.doc, {}, true);
    melange::xom::Document pd;
    Check(ParseBank(plain.bank, &pd) && pd.objects.size() == 5 + 2 * n && plain.added.empty(), "emitEmpty: LOCAL's own list as a bank");
    const sc::Built broken = sc::BuildSchemes(melange::xom::Document{}, {{"a", dir, {"schemes/kanly.json"}}});
    Check(!broken.fatal.empty() && broken.bank.empty(), "no DATA.LockedSchemes in LOCAL.XOM: fatal, no bank");
}

void TestSchemeErrors(const Local& l, const fs::path& dir) {
    struct Case {
        const char* name;
        const char* file;   // "" = write `text` to its own file
        const char* text;
        const char* error;
    };
    const Case cases[] = {
        {"bad key", "", R"({"key": "Kanly", "title": "K"})", "key must match"},
        {"short key", "", R"({"key": "FETXT.Scheme.K", "title": "K"})", "key must match"},
        {"key with a symbol", "", R"({"key": "FETXT.Scheme.Ka_nly", "title": "K"})", "key must match"},
        {"empty title", "", R"({"key": "FETXT.Scheme.Kanly", "title": ""})", "title must be"},
        {"long title", "", R"({"key": "FETXT.Scheme.Kanly", "title": "0123456789012345678901234"})", "title must be"},
        {"unknown base", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "base": "FE.Scheme.Nope"})", "not a built-in"},
        {"unknown field", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "fields": {"Nope": 1}})", "fields.Nope"},
        {"field of the wrong kind", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "fields": {"Name": 1}})", "fields.Name"},
        {"a weapon entry as a field", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "fields": {"Bazooka": 1}})", "fields.Bazooka"},
        {"non-integer value", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "fields": {"RoundTime": 1.5}})", "integer within int32"},
        {"value outside int32", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "fields": {"RoundTime": 2147483648}})", "integer within int32"},
        {"boolean field given a number", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "fields": {"MineFactoryOn": 1}})", "true or false"},
        {"unknown weapon", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "weapons": {"Nope": {"Ammo": 1}}})", "weapons.Nope"},
        {"unknown weapon key", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "weapons": {"*": {"Count": 1}}})", "unknown key 'Count'"},
        {"weapon value outside int32", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "weapons": {"*": {"Ammo": -2147483649}}})", "within int32"},
        {"unknown top-level key", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "extra": 1})", "unknown key 'extra'"},
        {"bad lock", "", R"({"key": "FETXT.Scheme.Kanly", "title": "K", "lock": "no spaces"})", "lock must be"},
        {"not an object", "", R"([1])", "JSON object"},
        {"broken JSON", "", R"({"key": )", ""},
        {"path leaves the folder", "../outside.json", "", "inside the mod folder"},
        {"absolute path", "C:/outside.json", "", "inside the mod folder"},
        {"not a .json file", "schemes/kanly.txt", "", "inside the mod folder"},
        {"missing file", "schemes/missing.json", "", ""},
    };
    int i = 0;
    for (const Case& c : cases) {
        std::string file = c.file;
        if (file.empty()) {
            file = "schemes/err" + std::to_string(++i) + ".json";
            Write(dir / file, c.text);
        }
        const sc::Built b = sc::BuildSchemes(l.doc, {{"m", dir, {file}}});
        Check(b.errors.size() == 1 && b.added.empty() && b.bank.empty() && (!*c.error || HasError(b, c.error)), c.name,
              b.errors.empty() ? "no error" : b.errors[0].text);
    }
}

// ---------------------------------------------------------------- factory weapons
void TestFactory(const Local& l, const fs::path& dir) {
    Write(dir / "weapons" / "red.json", R"({
  "key": "FETXT.Kanly.Red", "title": "Red Kanly", "base": "FETXT.WipeOut", "stock": false,
  "weapon": { "WormDamageMagnitude": 1.5, "FuseTime": 1500, "DetonationFX": "WXP_ExplosionX_Med", "GraphicalResourceID": ["a", "b"] },
  "cluster": { "WormDamageMagnitude": 0.25 }
})");
    Write(dir / "weapons" / "plain.json", R"({"key": "FETXT.Kanly.Plain", "title": "Plain", "base": "FETXT.WipeOut"})");
    const sc::Built b = sc::BuildFactoryWeapons(l.doc, {{"kanly", dir, {"weapons/red.json", "weapons/plain.json"}}});
    Check(b.fatal.empty() && b.errors.empty() && b.added.size() == 2 && b.builtIn == 1, "two presets build", b.fatal);
    melange::xom::Document d;
    Check(ParseBank(b.bank, &d), "the preset bank parses back (strict)");
    std::vector<std::string> names;
    for (const auto& t : d.types) names.push_back(t.className());
    const std::vector<std::string> want = {"XContainer", "XResourceDetails", "XContainerResourceDetails", "XDataBank",
                                           "WeaponFactoryContainer", "StoreWeaponFactory", "WeaponFactoryCollective"};
    Check(names == want, "TYPE table for the weapons bank");
    Check(d.objects.size() == 2 + 1 + 3 * 2 + 3 && d.types[4].count == 6 && d.types[5].count == 3, "object counts: 3 containers pairs, 3 stores",
          std::to_string(d.objects.size()));
    const Object& wrap = d.objects[0];
    Check(xu::Str(wrap, "Name") == "DATA.LockedWeapons" && xu::Int(wrap, "Flags") == 80 && d.root == 2, "wrapper and root");
    const Object* coll = d.object(wrap.field("Value")->asRef());
    Check(coll && coll->type == "WeaponFactoryCollective" && coll->field("Weapons")->items.size() == 3, "the collective holds n+1 = 3 refs");
    if (!coll || coll->field("Weapons")->items.size() != 3) return;
    auto store = [&](size_t i) { return d.object(coll->field("Weapons")->items[i].asRef()); };
    auto part = [&](const Object* s, const char* f) { return s ? d.object(s->field(f)->asRef()) : nullptr; };
    const Object *s0 = store(0), *s1 = store(1), *s2 = store(2);
    Check(s0 && xu::Str(*part(s0, "Weapon"), "Name") == "FETXT.WipeOut" && s0->field("StockWeapon")->asBool(), "the vanilla preset is first and untouched");
    Check(s1 && xu::Str(*part(s1, "Weapon"), "Name") == "FETXT.Kanly.Red" && !s1->field("StockWeapon")->asBool(),
          "the new preset: Name from the key, stock false");
    const Object* w = part(s1, "Weapon");
    const Object* c = part(s1, "Cluster");
    Check(w && std::fabs(w->field("WormDamageMagnitude")->asFloat() - 1.5) < 1e-6 && xu::Int(*w, "FuseTime") == 1500 &&
              xu::Str(*w, "DetonationFX") == "WXP_ExplosionX_Med" && w->field("GraphicalResourceID")->items.size() == 2 &&
              w->field("GraphicalResourceID")->items[1].str == "b",
          "weapon overrides: float, int, string and string array");
    Check(c && std::fabs(c->field("WormDamageMagnitude")->asFloat() - 0.25) < 1e-6 && xu::Str(*c, "Name").empty(), "cluster override, its Name stays empty");
    const Object* v = part(s0, "Weapon");
    Check(v && std::fabs(v->field("WormDamageMagnitude")->asFloat() - 2.0) < 1e-6 && xu::Int(*v, "FuseTime") == 3000, "the base preset is not shared with the copy");
    Check(s2 && s2->field("StockWeapon")->asBool() && xu::Int(*part(s2, "Weapon"), "FuseTime") == 3000 &&
              xu::Str(*part(s2, "Weapon"), "DetonationFX") == "WXP_Old",
          "stock defaults to true and the rest is the base's");
    std::vector<uint8_t> again;
    std::string err;
    Check(melange::xom::serialize(d, again, &err) && again == b.bank, "serialize(parse(bank)) is byte-identical");

    struct Case {
        const char* name;
        const char* text;
        const char* error;
    };
    const Case cases[] = {
        {"bad key", R"({"key": "Kanly.Red", "title": "K", "base": "FETXT.WipeOut"})", "key must be"},
        {"key too short", R"({"key": "FETXT.", "title": "K", "base": "FETXT.WipeOut"})", "key must be"},
        {"key part starting with a digit", R"({"key": "FETXT.1Red", "title": "K", "base": "FETXT.WipeOut"})", "key must be"},
        {"vanilla key", R"({"key": "FETXT.WipeOut", "title": "K", "base": "FETXT.WipeOut"})", "already used"},
        {"missing base", R"({"key": "FETXT.Kanly.Red", "title": "K"})", "base must be"},
        {"unknown base", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.Nope"})", "not a built-in"},
        {"unknown field", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.WipeOut", "weapon": {"Nope": 1}})", "weapon.Nope"},
        {"cluster unknown field", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.WipeOut", "cluster": {"Nope": 1}})", "cluster.Nope"},
        {"Name override", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.WipeOut", "weapon": {"Name": "x"}})", "set from the key"},
        {"wrong value type", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.WipeOut", "weapon": {"FuseTime": "soon"}})", "weapon.FuseTime"},
        {"non-integer int", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.WipeOut", "weapon": {"FuseTime": 1.5}})", "weapon.FuseTime"},
        {"string array of numbers", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.WipeOut", "weapon": {"GraphicalResourceID": [1]}})", "printable"},
        {"stock not a bool", R"({"key": "FETXT.Kanly.Red", "title": "K", "base": "FETXT.WipeOut", "stock": 1})", "stock must be"},
    };
    int i = 0;
    for (const Case& cs : cases) {
        const std::string file = "weapons/err" + std::to_string(++i) + ".json";
        Write(dir / file, cs.text);
        const sc::Built e = sc::BuildFactoryWeapons(l.doc, {{"m", dir, {file}}});
        Check(e.errors.size() == 1 && e.added.empty() && e.bank.empty() && HasError(e, cs.error), cs.name, e.errors.empty() ? "no error" : e.errors[0].text);
    }
    // Duplicate keys across two mods.
    Write(dir / "weapons" / "a.json", R"({"key": "FETXT.Same", "title": "A", "base": "FETXT.WipeOut"})");
    const sc::Built dup = sc::BuildFactoryWeapons(l.doc, {{"one", dir, {"weapons/a.json"}}, {"two", dir, {"weapons/a.json"}}});
    Check(dup.added.size() == 1 && dup.added[0].mod == "one" && dup.errors.size() == 1 && dup.errors[0].mod == "two", "the second mod's duplicate preset is refused");
}

void TestLimits(const Local& l, const fs::path& dir) {
    std::vector<std::string> files;
    for (size_t i = 0; i < sc::kMaxSchemes + 1; ++i) {
        const std::string name = "Lim" + std::string(1, char('a' + i / 26)) + std::string(1, char('a' + i % 26));
        const std::string f = "limits/s" + std::to_string(i) + ".json";
        Write(dir / f, "{\"key\": \"FETXT.Scheme." + name + "\", \"title\": \"" + name + "\"}");
        files.push_back(f);
    }
    const sc::Built b = sc::BuildSchemes(l.doc, {{"m", dir, files}});
    Check(b.added.size() == sc::kMaxSchemes && b.errors.size() == 1 && HasError(b, "more than 32 schemes"), "32 schemes across mods, the 33rd is refused");
}

// ---------------------------------------------------------------- spice.json
std::string Spice(const std::string& extra) {
    return R"({"spiceVersion": 1, "id": "schemetest", "version": "1.0.0", "kind": "client-only", "name": "Scheme test", "melange": {"range": ">=0.4.0"})" +
           (extra.empty() ? std::string() : ", " + extra) + "}";
}

bool Manifest(const fs::path& root, const std::string& extra, melange::spice::Manifest* m, std::vector<melange::spice::Error>* errs) {
    const fs::path dir = root / "schemetest";
    Write(dir / "spice.json", Spice(extra));
    return melange::spice::Parse(dir.wstring(), m, errs);
}

void TestManifest(const fs::path& root) {
    melange::spice::Manifest m;
    std::vector<melange::spice::Error> errs;
    const bool ok = Manifest(root, R"("schemes": [{"file": "schemes/kanly.json"}], "factoryWeapons": [{"file": "weapons/red.json"}, {"file": "weapons/blue.json"}])", &m, &errs);
    Check(ok && m.schemes.size() == 1 && m.schemes[0].file == "schemes/kanly.json" && m.factoryWeapons.size() == 2 &&
              m.factoryWeapons[1].file == "weapons/blue.json" && !m.content,
          "schemes and factoryWeapons parse for a client-only mod", errs.empty() ? "" : errs[0].text);
    auto bad = [&](const char* what, const std::string& extra, const char* field) {
        melange::spice::Manifest bm;
        std::vector<melange::spice::Error> be;
        const bool good = Manifest(root, extra, &bm, &be);
        bool named = false;
        for (const auto& e : be) named |= e.field == field;
        Check(!good && named, what, be.empty() ? "no error" : be[0].text);
    };
    bad("schemes must be an array", R"("schemes": {"file": "a.json"})", "schemes");
    bad("a scheme entry is {file}", R"("schemes": ["a.json"])", "schemes");
    bad("a scheme path leaves the folder", R"("schemes": [{"file": "../a.json"}])", "schemes");
    bad("a scheme path is not .json", R"("schemes": [{"file": "a.txt"}])", "schemes");
    bad("an entry with other keys", R"("factoryWeapons": [{"file": "a.json", "x": 1}])", "factoryWeapons");
    std::string nine = "\"schemes\": [";
    for (int i = 0; i < 9; ++i) nine += std::string(i ? "," : "") + "{\"file\": \"s" + std::to_string(i) + ".json\"}";
    bad("at most 8 schemes per mod", nine + "]", "schemes");
    std::string seventeen = "\"factoryWeapons\": [";
    for (int i = 0; i < 17; ++i) seventeen += std::string(i ? "," : "") + "{\"file\": \"w" + std::to_string(i) + ".json\"}";
    bad("at most 16 factory weapons per mod", seventeen + "]", "factoryWeapons");
}

void TestKeys() {
    Check(sc::ValidSchemeKey("FETXT.Scheme.Kanly") && sc::ValidSchemeKey("FETXT.Scheme.K1") &&
              sc::ValidSchemeKey("FETXT.Scheme." + std::string(32, 'a')),
          "scheme keys: letter then 1-31 letters or digits");
    Check(!sc::ValidSchemeKey("FETXT.Scheme.K") && !sc::ValidSchemeKey("FETXT.Scheme.1ab") && !sc::ValidSchemeKey("FETXT.Scheme." + std::string(33, 'a')) &&
              !sc::ValidSchemeKey("FE.Scheme.Kanly") && !sc::ValidSchemeKey("FETXT.Scheme.Ka.nly") && !sc::ValidSchemeKey(""),
          "scheme keys: too short, digit first, too long, wrong prefix, dot, empty");
    Check(sc::ValidFactoryKey("FETXT.Kanly.Red") && sc::ValidFactoryKey("FETXT.Ab") &&
              sc::ValidFactoryKey("FETXT." + std::string(34, 'a')) && !sc::ValidFactoryKey("FETXT." + std::string(35, 'a')),
          "factory keys: 6..40 characters");
    Check(!sc::ValidFactoryKey("FETXT.Kanly..Red") && !sc::ValidFactoryKey("FETXT.Kanly.") && !sc::ValidFactoryKey("FETXT.Kanly.1") &&
              !sc::ValidFactoryKey("Kanly.Red.Blue") && !sc::ValidFactoryKey("FETXT.Ka_nly"),
          "factory keys: empty part, trailing dot, digit first, wrong prefix, symbol");
}
}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "melange_schemes_selftest";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    const Local local = MakeLocal();
    Check(local.weaponFields > 0 && local.doc.objects.size() > 10, "the synthetic LOCAL.XOM has objects");
    TestKeys();
    TestSchemes(local, root / "mod");
    TestSchemeErrors(local, root / "mod");
    TestFactory(local, root / "mod");
    TestLimits(local, root / "mod");
    TestManifest(root / "mods");
    fs::remove_all(root, ec);
    printf("schemes_selftest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
