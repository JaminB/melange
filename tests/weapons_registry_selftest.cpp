// Offline self-test for the clone registry (weapons/registry_core.cpp) against a fake engine: creation per match (all
// or none), set values, panel cells, text, icons, the name-slot swap, the virtual-id mapping of the hooks and the
// match lifecycle. Exit code 0 = all passed.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "mods/spice.h"
#include "weapons/manifest.h"
#include "weapons/registry_core.h"

namespace wm = melange::weapons::manifest;
namespace core = melange::weapons::core;
using melange::weapons::FieldType;
using melange::weapons::kVidBase;

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

constexpr uintptr_t kPayloadClass = 0x9688e0, kOtherClass = 0x9999a0;
const char* const kBaseNames[] = {"kWeaponBazooka", "kWeaponGrenade", "kWeaponHolyHandGrenade", "kWeaponBananaBomb",
                                  "kWeaponGasCanister"};
const int kBaseIds[] = {1, 2, 6, 7, 16};

struct FieldDef {
    const char* name;
    uint32_t off;
    FieldType type;
};
const FieldDef kFields[] = {
    {"WormDamageMagnitude", 0x15c, FieldType::F32}, {"WormDamageRadius", 0x164, FieldType::F32},
    {"LandDamageRadius", 0x168, FieldType::F32},    {"ImpulseRadius", 0x16c, FieldType::F32},
    {"PayloadGraphicsResourceID", 0xd4, FieldType::String}, {"LaunchSfx", 0x1a0, FieldType::String},
    {"IsHoming", 0x1b0, FieldType::Bool},            {"NumBomblets", 0x1b4, FieldType::U8},
    {"Fuse", 0x1b8, FieldType::I32},               {"DetonatesOnLandImpact", 0x1bc, FieldType::Bool},
};

struct Fake final : core::Engine {
    std::map<uintptr_t, uint8_t> mem;
    std::map<std::string, uintptr_t> res, descs;
    std::map<uintptr_t, uintptr_t> cls;
    std::map<uintptr_t, std::string> strs;
    std::map<std::string, std::string> texts;
    std::map<std::string, std::string> banks;  // "mod/rel" -> container name it holds
    uintptr_t next = 0x20000000;
    int adds = 0, lastAddFlags = -1, bankLoads = 0, iconCalls = 0, textAdds = 0;
    bool failAdd = false, hooksOk = true, hooksOn = false, hud = true, failTextAdd = false;
    uint32_t icon = 0, tick = 100;
    uintptr_t cloneClassOverride = 0;

    uintptr_t NewObj(const std::string& name, uintptr_t c) {
        const uintptr_t o = next;
        next += 0x1000;
        res[name] = o;
        descs[name] = o + 0x800;
        cls[o] = c;
        return o;
    }
    void Put32(uintptr_t a, uint32_t v) { Write(a, &v, 4); }
    uint32_t Get32(uintptr_t a) {
        uint32_t v = 0;
        Read(a, &v, 4);
        return v;
    }
    float GetF(uintptr_t a) {
        float v = 0;
        Read(a, &v, 4);
        return v;
    }
    void NewScene() {
        std::map<std::string, uintptr_t> keep;
        for (auto& [n, o] : res)
            if (n.rfind("kWeaponBazooka", 0) == 0 || n == "kWeaponGrenade" || n == "kWeaponHolyHandGrenade" ||
                n == "kWeaponBananaBomb" || n == "kWeaponGasCanister")
                keep[n] = o;
        res = keep;
        texts.clear();
        texts["Text.kWeaponBazooka"] = "Bazooka";
        texts["HelpText.kWeaponBazooka0"] = "Fire it.";
        texts["Text.kWeaponGrenade"] = "Grenade";
        texts["HelpText.kWeaponGrenade0"] = "Throw it.";
        texts["HelpText.kWeaponGrenade1"] = "Then run.";
    }

    uintptr_t Lookup(const char* name) override {
        auto it = res.find(name);
        return it == res.end() ? 0 : it->second;
    }
    uintptr_t LookupDesc(const char* name) override {
        auto it = descs.find(name);
        return it == descs.end() || !res.count(name) ? 0 : it->second;
    }
    int AddResource(const char* name, uintptr_t obj, uint32_t flags) override {
        ++adds;
        lastAddFlags = static_cast<int>(flags);
        if (failAdd) return static_cast<int>(0x80004005);
        if (res.count(name) && !(flags & 1)) return static_cast<int>(0x80004005);
        const uintptr_t o = NewObj(name, cloneClassOverride ? cloneClassOverride : cls[obj]);
        for (uint32_t i = 0; i < 0x200; ++i) {
            auto it = mem.find(obj + i);
            if (it != mem.end()) mem[o + i] = it->second;
        }
        for (auto& f : kFields)
            if (strs.count(obj + f.off)) strs[o + f.off] = strs[obj + f.off];
        return 0;
    }
    int LoadModBank(const char* mod, const char* rel, std::string* err) override {
        ++bankLoads;
        auto it = banks.find(std::string(mod) + "/" + rel);
        if (it == banks.end()) {
            if (err) *err = "no such bank";
            return -1;
        }
        const uintptr_t o = NewObj(it->second, kPayloadClass);
        Put32(o + 0x15c, 0);
        float v = 200.f;
        Write(o + 0x15c, &v, 4);
        return 0;
    }
    uintptr_t ClassOf(uintptr_t c) override {
        auto it = cls.find(c);
        return it == cls.end() ? 0 : it->second;
    }
    FieldType Field(uintptr_t c, const char* f, uint32_t* off) override {
        if (ClassOf(c) != kPayloadClass) return FieldType::None;
        for (auto& d : kFields)
            if (strcmp(d.name, f) == 0) {
                *off = d.off;
                return d.type;
            }
        return FieldType::None;
    }
    bool Read(uintptr_t a, void* out, size_t n) override {
        auto* o = static_cast<uint8_t*>(out);
        for (size_t i = 0; i < n; ++i) {
            auto it = mem.find(a + i);
            o[i] = it == mem.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(uintptr_t a, const void* data, size_t n) override {
        auto* d = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) mem[a + i] = d[i];
        return true;
    }
    bool AssignString(uintptr_t field, const char* s) override {
        strs[field] = s;
        return true;
    }
    int AddText(const char* key, const char* value) override {
        ++textAdds;
        if (failTextAdd) return -1;
        texts[key] = value;
        return 0;
    }
    bool GetText(const char* key, std::string* out) override {
        auto it = texts.find(key);
        if (it == texts.end()) return false;
        *out = it->second;
        return true;
    }
    uint32_t ReserveIcon(const char*, const char*, std::string* err) override {
        ++iconCalls;
        if (!icon && err) *err = "stub";
        return icon;
    }
    bool HudUsable(const char*) override { return hud; }
    bool EnableHooks(bool on) override {
        if (on && !hooksOk) return false;
        hooksOn = on;
        return true;
    }
    uint32_t Tick() override { return tick; }

    Fake() {
        for (int i = 0; i < 5; ++i) {
            Put32(core::kNames + 4 * kBaseIds[i], static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kBaseNames[i])));
            const uintptr_t o = NewObj(kBaseNames[i], kPayloadClass);
            float dmg = 50.f + i;
            Write(o + 0x15c, &dmg, 4);
            strs[o + 0xd4] = std::string(kBaseNames[i] + 7) + ".Payload";
        }
        for (int c = 0; c < core::kPanelCells; ++c) {
            uint32_t id = 5, code = 0x0501;
            if (c == 0) id = 1, code = 0x0001;
            if (c == 6) id = 2, code = 0x0002;
            if (c == 9) id = 6, code = 0x0302;
            if (c == 29 || c == 39 || c == 40) id = core::kUndefined, code = 0;
            Put32(core::kPanel + 8 * c, id);
            Put32(core::kPanel + 8 * c + 4, code);
        }
        NewScene();
    }
    std::string Slot(int id) {
        const auto p = static_cast<uintptr_t>(Get32(core::kNames + 4 * id));
        return p ? reinterpret_cast<const char*>(p) : "";
    }
    uint32_t CellId(int c) { return Get32(core::kPanel + 8 * c); }
    uint32_t CellIcon(int c) { return Get32(core::kPanel + 8 * c + 4); }
};

wm::CloneDecl Decl(const char* name, const char* base, int baseId, int cell, uint16_t k) {
    wm::CloneDecl d;
    d.mod = "testmod";
    d.name = name;
    d.base = base;
    d.baseId = baseId;
    d.cell = cell;
    d.k = k;
    return d;
}

wm::SetValue F(const char* f, double v) {
    wm::SetValue s;
    s.field = f;
    s.type = FieldType::F32;
    s.number = v;
    return s;
}

wm::SetValue S(const char* f, const char* v) {
    wm::SetValue s;
    s.field = f;
    s.type = FieldType::String;
    s.string = v;
    return s;
}

uintptr_t P(const char* s) { return reinterpret_cast<uintptr_t>(s); }

void Basic() {
    Fake e;
    core::Registry r(e);
    auto d = Decl("kWeaponMegaBazooka", "kWeaponBazooka", 1, 29, 0);
    d.set = {F("WormDamageMagnitude", 120), F("LandDamageRadius", 90), S("PayloadGraphicsResourceID", "Cow.Payload")};
    d.text = {"Mega Bazooka", "Like a bazooka, only more so."};
    d.hudIcon = "testmod.hud.tga";
    r.Configure({d});
    Expect(r.Count() == 1 && !r.Live() && r.Active() == -1, "configured, not live");

    std::string why;
    Expect(r.Init(&why), "init: " + why);
    const auto* c = r.At(0);
    Expect(r.Live() && c->info.live && c->info.container && c->info.descriptor, "live with container and descriptor");
    Expect(e.adds == 1 && e.lastAddFlags == 0, "one AddResource without overwrite");
    Expect(e.GetF(c->info.container + 0x15c) == 120.f && e.GetF(c->info.container + 0x168) == 90.f, "numbers set");
    Expect(e.strs[c->info.container + 0xd4] == "Cow.Payload", "string set");
    Expect(e.GetF(e.res["kWeaponBazooka"] + 0x15c) == 50.f && e.strs[e.res["kWeaponBazooka"] + 0xd4] == "Bazooka.Payload",
           "base untouched");
    Expect(e.CellId(29) == 0x100 && e.CellIcon(29) == 0x0001, "cell 29 = {vid, base's icon}");
    Expect(e.CellId(39) == core::kUndefined && e.CellId(40) == core::kUndefined, "other free cells untouched");
    Expect(e.texts["Text.kWeaponMegaBazooka"] == "Mega Bazooka", "Text. registered");
    Expect(e.texts["HelpText.kWeaponMegaBazooka0"] == "Like a bazooka, only more so.", "HelpText.0 registered");
    Expect(e.hooksOn, "hooks enabled in a live match");
    Expect(r.ByDesc(c->info.descriptor) == &c->info && !r.ByDesc(e.descs["kWeaponBazooka"]) && !r.ByDesc(0), "ByDesc");

    // The panel guards.
    Expect(r.GuardId(0x100) == 1 && r.GuardId(1) == 1 && r.GuardId(0x43) == 0x43, "id guard maps the vid only");
    Expect(r.TextName(0x100, 0x1234, false) == P(c->namePtr) && r.TextName(0x100, 0x1234, true) == P(c->namePtr),
           "clone cell shows its own name and help");
    Expect(r.TextName(2, P(kBaseNames[1]), false) == P(kBaseNames[1]), "other cells untouched");
    Expect(*reinterpret_cast<const char* const*>(r.CanUseSlot(0x100, 0)) == c->namePtr, "can-use reads the clone container");
    Expect(r.CanUseSlot(7, 0x5555) == 0x5555, "can-use of a vanilla id untouched");
    Expect(r.HudName() == nullptr, "no HUD name before a select");

    // Selection and the name-slot swap.
    Expect(r.Select(0x100) == 1, "select vid -> base id");
    Expect(r.Active() == 0 && e.Slot(1) == "kWeaponMegaBazooka" && r.SwappedBase() == 1, "slot swapped, clone active");
    Expect(r.HudName() && std::string(r.HudName()) == "testmod.hud.tga", "HUD name while active");
    Expect(r.TextName(1, P(c->namePtr), false) == P(kBaseNames[0]), "the base's own cell keeps the base's name");
    Expect(*reinterpret_cast<const char* const*>(r.CanUseSlot(1, 0)) == kBaseNames[0], "the base's can-use reads the base");
    Expect(r.Select(2) == 2 && r.Active() == -1 && e.Slot(1) == "kWeaponBazooka", "another weapon restores the slot");
    Expect(r.Select(0x100) == 1 && e.Slot(1) == "kWeaponMegaBazooka", "reselect swaps again");
    r.TurnEnded();
    Expect(r.Active() == -1 && e.Slot(1) == "kWeaponBazooka" && r.SwappedBase() == -1, "turn end restores");
    Expect(r.Select(0x102) == 1 && r.Active() == -1 && e.Slot(1) == "kWeaponBazooka", "undeclared vid -> Bazooka, no swap");
    Expect(r.Select(0x100) == 1 && r.Select(0x7fff) == 1 && r.Active() == -1 && e.Slot(1) == "kWeaponBazooka",
           "out-of-range value -> Bazooka, swap dropped");
    Expect(r.Select(-5) == 1, "negative value mapped defensively");

    // Match end, then a second match in a fresh scene.
    r.Select(0x100);
    r.MatchEnd();
    Expect(!r.Live() && !c->info.live && !c->info.container && r.Active() == -1, "match end: not live");
    Expect(e.CellId(29) == core::kUndefined && e.CellIcon(29) == 0 && e.Slot(1) == "kWeaponBazooka" && !e.hooksOn,
           "match end restores cell, slot and hooks");
    Expect(r.GuardId(0x100) == 1 && r.TextName(0x100, 0, false) == P(kBaseNames[0]), "without a live clone: base's name");
    e.NewScene();
    Expect(r.Init(&why) && e.adds == 2 && e.lastAddFlags == 0, "second match creates it again: " + why);
    Expect(e.texts["Text.kWeaponMegaBazooka"] == "Mega Bazooka", "text registered again in the new scene");
    // A match whose scene kept our container: overwrite from the base.
    r.MatchEnd();
    Expect(r.Init(&why) && e.adds == 3 && e.lastAddFlags == 1, "a leftover clone of ours is overwritten: " + why);
    r.MatchEnd();
    // A missed match end: the next Init drops the old state first.
    e.NewScene();
    r.Init(&why);
    r.Select(0x100);
    e.NewScene();
    Expect(r.Init(&why) && r.Active() == -1 && e.Slot(1) == "kWeaponBazooka" && e.CellId(29) == 0x100, "unclosed match recovered");
    r.MatchEnd();
}

void Refusals() {
    {
        Fake e;
        core::Registry r(e);
        auto d = Decl("kWeaponGrenade2", "kWeaponGrenade", 2, 29, 0);
        e.NewObj("kWeaponGrenade2", kPayloadClass);
        r.Configure({d});
        std::string why;
        Expect(!r.Init(&why) && why.find("already a resource") != std::string::npos, "vanilla name refused: " + why);
        Expect(e.adds == 0 && e.CellId(29) == core::kUndefined, "no cell or resource change");
        Expect(e.hooksOn && r.GuardId(0x100) == 2, "the id guards stay on and keep mapping this peer's vid to its base");
    }
    {
        Fake e;
        core::Registry r(e);
        auto a = Decl("kWeaponAaa", "kWeaponBazooka", 1, 29, 0);
        auto b = Decl("kWeaponBbb", "kWeaponGrenade", 2, 39, 1);
        b.set = {S("WormDamageMagnitude", "x")};
        r.Configure({a, b});
        std::string why;
        Expect(!r.Init(&why) && why.find("WormDamageMagnitude") != std::string::npos, "type mismatch refuses all: " + why);
        Expect(!r.Live() && !r.At(0)->info.live && !r.At(0)->info.container && e.CellId(29) == core::kUndefined &&
                   e.CellId(39) == core::kUndefined,
               "all or nothing");
    }
    {
        Fake e;
        core::Registry r(e);
        auto a = Decl("kWeaponAaa", "kWeaponBazooka", 1, 29, 0);
        a.set = {F("NoSuchField", 1)};
        r.Configure({a});
        std::string why;
        Expect(!r.Init(&why) && why.find("NoSuchField") != std::string::npos, "unknown live field refused");
    }
    {
        Fake e;
        core::Registry r(e);
        r.Configure({Decl("kWeaponAaa", "kWeaponBazooka", 1, 29, 0)});
        e.Put32(core::kPanel + 8 * 29, 7);
        std::string why;
        Expect(!r.Init(&why) && why.find("cell 29") != std::string::npos && e.CellId(29) == 7, "occupied cell refused");
    }
    {
        Fake e;
        core::Registry r(e);
        r.Configure({Decl("kWeaponAaa", "kWeaponBazooka", 1, 29, 0)});
        e.failAdd = true;
        std::string why;
        Expect(!r.Init(&why) && why.find("AddResource") != std::string::npos, "AddResource failure");
        // A peer whose own clone failed to go live (e.g. a local CRC or hook problem) must still guard any vid a
        // live peer sends it: the content hash only covers what is declared, not whether it went live here.
        Expect(e.hooksOn && !r.Live() && r.GuardId(0x100) == 1 && r.Select(0x100) == 1 && r.Active() == -1,
               "no live clone here: the guards still map the vid to its base");
    }
    {
        Fake e;
        core::Registry r(e);
        r.Configure({Decl("kWeaponAaa", "kWeaponBazooka", 1, 29, 0)});
        e.cloneClassOverride = kOtherClass;
        std::string why;
        Expect(!r.Init(&why) && why.find("class") != std::string::npos, "class mismatch refused");
    }
    {
        Fake e;
        core::Registry r(e);
        r.Configure({Decl("kWeaponAaa", "kWeaponBazooka", 1, 29, 0), Decl("kWeaponBbb", "kWeaponGrenade", 2, 39, 1)});
        e.hooksOk = false;
        std::string why;
        Expect(!r.Init(&why) && why.find("hooks") != std::string::npos, "hooks failing refuses");
        Expect(e.CellId(29) == core::kUndefined && e.CellId(39) == core::kUndefined && !r.Live(), "cells restored");
    }
    {
        Fake e;
        core::Registry r(e);
        r.Configure({Decl("kWeaponAaa", "kWeaponBazooka", 1, 29, 0)});
        const char* other = "kWeaponOther";
        e.Put32(core::kNames + 4, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(other)));
        std::string why;
        Expect(!r.Init(&why) && why.find("name slot") != std::string::npos, "a changed name slot refuses");
    }
    {
        Fake e;
        core::Registry r(e);
        std::string why;
        Expect(!r.Init(&why) && !e.hooksOn, "no declarations, nothing to do");
    }
}

void Banks() {
    Fake e;
    core::Registry r(e);
    auto d = Decl("kWeaponBanked", "kWeaponBazooka", 1, 40, 0);
    d.bank = "banked.xom";
    d.set = {F("WormDamageRadius", 150)};
    r.Configure({d});
    std::string why;
    Expect(!r.Init(&why) && why.find("not loaded") != std::string::npos && e.bankLoads == 1, "missing bank refused");
    e.banks["testmod/banked.xom"] = "kWeaponWrongName";
    Expect(!r.Init(&why) && why.find("no container") != std::string::npos, "bank without the named container refused");
    e.NewScene();
    e.banks["testmod/banked.xom"] = "kWeaponBanked";
    Expect(r.Init(&why) && e.adds == 0, "bank clone live without AddResource: " + why);
    const auto* c = r.At(0);
    Expect(e.GetF(c->info.container + 0x15c) == 200.f && e.GetF(c->info.container + 0x164) == 150.f, "bank values plus set");
    Expect(e.CellId(40) == 0x100, "bank clone in cell 40");
    r.MatchEnd();
    const int loads = e.bankLoads;
    Expect(r.Init(&why) && e.bankLoads == loads, "a bank container left in the scene is reused");
    r.MatchEnd();
}

void ThreeClones() {
    Fake e;
    core::Registry r(e);
    auto a = Decl("kWeaponOne", "kWeaponBazooka", 1, 29, 0);
    auto b = Decl("kWeaponTwo", "kWeaponGrenade", 2, 39, 1);
    auto c = Decl("kWeaponThree", "kWeaponBazooka", 1, 40, 2);
    a.set = {F("WormDamageMagnitude", 101)};
    b.set = {F("WormDamageMagnitude", 102)};
    c.set = {F("WormDamageMagnitude", 103)};
    r.Configure({a, b, c});
    std::string why;
    Expect(r.Init(&why), "three clones: " + why);
    Expect(e.CellId(29) == 0x100 && e.CellId(39) == 0x101 && e.CellId(40) == 0x102, "vids by k in their cells");
    Expect(e.CellIcon(39) == 0x0002, "Grenade clone uses the Grenade icon");
    for (int k = 0; k < 3; ++k)
        Expect(e.GetF(r.At(k)->info.container + 0x15c) == 101.f + k, "own stats k=" + std::to_string(k));
    Expect(r.Select(0x101) == 2 && e.Slot(2) == "kWeaponTwo" && e.Slot(1) == "kWeaponBazooka", "Grenade clone swaps slot 2");
    Expect(r.Select(0x100) == 1 && e.Slot(1) == "kWeaponOne" && e.Slot(2) == "kWeaponGrenade", "switch base restores slot 2");
    Expect(r.Select(0x102) == 1 && e.Slot(1) == "kWeaponThree" && r.Active() == 2, "same base, other clone");
    Expect(r.TextName(1, P(r.At(2)->namePtr), false) == P(kBaseNames[0]), "base cell text while its slot holds clone k2");
    Expect(r.TextName(1, P(r.At(0)->namePtr), false) == P(r.At(0)->namePtr), "only the swapped name is replaced");
    Expect(r.GuardId(0x101) == 2 && r.GuardId(0x102) == 1, "guards per vid");
    r.MatchEnd();
    Expect(e.Slot(1) == "kWeaponBazooka" && e.Slot(2) == "kWeaponGrenade" && e.CellId(40) == core::kUndefined, "all restored");
}

void TextAndIcons() {
    Fake e;
    core::Registry r(e);
    auto g = Decl("kWeaponGrenadeX", "kWeaponGrenade", 2, 29, 0);
    g.panelIcon = "icons/x.png";
    r.Configure({g});
    e.icon = 0x0c03;
    std::string why;
    Expect(r.Init(&why), "init: " + why);
    Expect(e.texts["Text.kWeaponGrenadeX"] == "Grenade" && e.texts["HelpText.kWeaponGrenadeX0"] == "Throw it." &&
               e.texts["HelpText.kWeaponGrenadeX1"] == "Then run.",
           "without text the base's strings are copied");
    Expect(r.At(0)->text && r.At(0)->help, "copied text counts as the clone's own");
    Expect(e.CellIcon(29) == 0x0c03 && r.At(0)->info.iconCode == 0x0c03 && e.iconCalls == 1, "reserved icon used");
    r.MatchEnd();
    e.NewScene();
    Expect(r.Init(&why) && e.iconCalls == 1 && e.CellIcon(29) == 0x0c03, "icon reserved once per launch");
    r.MatchEnd();

    Fake f;
    core::Registry q(f);
    auto h = Decl("kWeaponHud", "kWeaponBazooka", 1, 29, 0);
    h.hudIcon = "testmod.hud.tga";
    h.panelIcon = "icons/y.png";
    q.Configure({h});
    f.failTextAdd = true;
    f.hud = false;
    Expect(q.Init(&why), "text failures are not fatal: " + why);
    Expect(!q.At(0)->text && q.TextName(0x100, 0, false) == P(kBaseNames[0]) && q.TextName(0x100, 0, true) == P(kBaseNames[0]),
           "without registered text the clone cell shows the base's");
    Expect(f.CellIcon(29) == 0x0001 && q.At(0)->info.iconCode == 0, "icon refused: base's icon");
    q.Select(0x100);
    Expect(q.HudName() == nullptr, "HUD name only when the mod's loose root is a search path");
    q.MatchEnd();
}

std::wstring g_root;

void FromManifest() {
    const char* json = R"({"spiceVersion":1,"id":"mega-bazooka","version":"1.0.0","name":"m","melange":{"range":">=0.0.0"},
      "kind":"content","entry":{},"weapons":[{"name":"kWeaponMegaBazooka","base":"kWeaponBazooka",
      "set":{"WormDamageMagnitude":120,"WormDamageRadius":123.75,"LandDamageRadius":90,"ImpulseRadius":165,
             "PayloadGraphicsResourceID":"Grenade.Payload","LaunchSfx":"weapons/SheepBaa","IsHoming":false},
      "text":{"name":"Mega Bazooka","help":"Like a bazooka, only more so."}}]})";
    const std::wstring dir = g_root + L"\\mega-bazooka";
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* fp = _wfopen((dir + L"\\spice.json").c_str(), L"wb");
    if (!fp) {
        Expect(false, "temp manifest");
        return;
    }
    fwrite(json, 1, strlen(json), fp);
    fclose(fp);
    melange::spice::Manifest m;
    std::vector<melange::spice::Error> perr;
    Expect(melange::spice::Parse(dir, &m, &perr), "manifest parses");
    std::vector<wm::Error> errs;
    auto decls = wm::Assign({wm::Parse(m, &errs)}, &errs);
    Expect(decls.size() == 1 && errs.empty(), "one clone declared");
    if (decls.size() != 1) return;
    Fake e;
    core::Registry r(e);
    r.Configure(decls);
    std::string why;
    Expect(r.Init(&why), "manifest clone live: " + why);
    const uintptr_t o = r.At(0)->info.container;
    Expect(e.CellId(29) == 0x100, "default cell 29");
    Expect(e.GetF(o + 0x15c) == 120.f && e.GetF(o + 0x164) == 123.75f && e.GetF(o + 0x16c) == 165.f, "manifest numbers");
    Expect(e.strs[o + 0xd4] == "Grenade.Payload" && e.strs[o + 0x1a0] == "weapons/SheepBaa", "manifest strings");
    uint8_t b = 9;
    e.Read(o + 0x1b0, &b, 1);
    Expect(b == 0, "manifest bool");
    r.MatchEnd();
}
}  // namespace

// weapons_registry_selftest <modDir>...: parses those mods in that order and creates their clones in the fake engine.
int CheckMods(int argc, wchar_t** argv) {
    std::vector<std::vector<wm::CloneDecl>> perMod;
    std::vector<wm::Error> errs;
    for (int i = 1; i < argc; ++i) {
        melange::spice::Manifest m;
        std::vector<melange::spice::Error> perr;
        if (!melange::spice::Parse(argv[i], &m, &perr)) {
            for (auto& e : perr) printf("%ls: %s\n", argv[i], e.text.c_str());
            return 1;
        }
        perMod.push_back(wm::Parse(m, &errs));
    }
    auto decls = wm::Assign(perMod, &errs);
    for (auto& e : errs) printf("%s: %s\n", e.mod.c_str(), e.text.c_str());
    Fake e;
    core::Registry r(e);
    r.Configure(decls);
    std::string why;
    const bool live = r.Init(&why);
    for (int k = 0; k < r.Count(); ++k) {
        const auto& c = r.At(k)->info;
        printf("k=%d %s base %d vid %x cell %d (%s)\n", k, c.name, c.base, static_cast<unsigned>(c.vid), c.cell, c.mod);
    }
    printf("%s%s\n", live ? "live" : "not live: ", why.c_str());
    return errs.empty() && live ? 0 : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) return CheckMods(argc, argv);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = std::wstring(tmp) + L"melange_registry_selftest_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(g_root.c_str(), nullptr);
    Basic();
    Refusals();
    Banks();
    ThreeClones();
    TextAndIcons();
    FromManifest();
    printf("weapons_registry_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
