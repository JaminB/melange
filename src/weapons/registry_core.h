#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "melange/weapons.h"
#include "weapons/manifest.h"

// The clone registry without the game: creation per match (all clones or none), the panel cells, the name-slot swap,
// the virtual-id mapping the hooks apply, and the clone text. Everything that touches the game goes through Engine,
// so the offline self-test drives it with a fake.
namespace melange::weapons::core {
constexpr uintptr_t kNames = 0x90c920, kPanel = 0x920e58;
constexpr int kEnumCount = 0x45, kPanelCells = 42;
constexpr uint32_t kUndefined = 0x43;
constexpr int32_t kFallbackBase = 1;  // kWeaponBazooka

class Engine {
public:
    virtual ~Engine() = default;
    virtual uintptr_t Lookup(const char* name) = 0;
    virtual uintptr_t LookupDesc(const char* name) = 0;
    virtual int AddResource(const char* name, uintptr_t obj, uint32_t flags) = 0;
    virtual int LoadModBank(const char* mod, const char* rel, std::string* err) = 0;
    virtual uintptr_t ClassOf(uintptr_t container) = 0;
    virtual FieldType Field(uintptr_t container, const char* field, uint32_t* offset) = 0;
    virtual bool Read(uintptr_t addr, void* out, size_t n) = 0;
    virtual bool Write(uintptr_t addr, const void* data, size_t n) = 0;
    virtual bool AssignString(uintptr_t field, const char* s) = 0;
    virtual int AddText(const char* key, const char* value) = 0;
    virtual bool GetText(const char* key, std::string* out) = 0;
    virtual uint32_t ReserveIcon(const char* mod, const char* relPng, std::string* err) = 0;  // 0 = none
    virtual bool HudUsable(const char* mod) = 0;  // the mod's loose-file root is an engine search path
    // A mod's <modId>.*.tga exists under its assets/loose (a missing file keeps the vanilla HUD icon).
    virtual bool HudFileExists(const char* mod, const char* file) { (void)mod; (void)file; return true; }
    // Weapon icon replacement (weaponIcons): write the mod's PNG over the vanilla sub-icon iconCode names (atlas | sub << 8), and
    // drop all such patches again (the vanilla pixels return when the atlas is next uploaded).
    virtual bool PatchPanelIcon(const char* mod, const char* relPng, uint32_t iconCode, std::string* err) = 0;
    virtual void ClearPanelIcons() = 0;
    // Vehicle meshes (vehicleMeshes): the mesh `name` is in the engine's mesh table with its graph loaded (its bank went
    // in), make `vehicle`'s graphic entity draw it from now until ClearVehicleMeshes(), which puts every vanilla name
    // back. False (with *err) when the build's bytes differ or the entity's name is not the vanilla one.
    virtual bool MeshLoaded(const char* name) { (void)name; return false; }
    // The nodes the entity's Setup looks up that the loaded mesh `name` lacks, comma separated; "" = none (or unknown).
    // A missing one makes Setup's lookup assert, so such a mesh is never armed.
    virtual std::string MeshNodesMissing(const char* name) { (void)name; return ""; }
    virtual bool SetVehicleMesh(const char* vehicle, const char* name, std::string* err) {
        (void)vehicle; (void)name;
        if (err) *err = "not supported";
        return false;
    }
    virtual void ClearVehicleMeshes() {}
    virtual bool EnableHooks(bool on) = 0;
    virtual uint32_t Tick() = 0;
    // The NUL-terminated string at addr, cut at max bytes; false if its first byte cannot be read. Byte by byte, so a
    // string that ends just before an unmapped page still comes back, and it runs once per weapon per match.
    virtual bool ReadString(uintptr_t addr, std::string* out, size_t max = 64) {
        out->clear();
        if (!addr) return false;
        while (out->size() < max) {
            char c = 0;
            if (!Read(addr + out->size(), &c, 1)) return !out->empty();
            if (!c) break;
            out->push_back(c);
        }
        return true;
    }
    // The text of an XString field (as AssignString writes it); false if unreadable. The default reads a plain
    // pointer-to-chars, which is only right for a fake; the game engine overrides it.
    virtual bool ReadXString(uintptr_t field, std::string* out) {
        uint32_t p = 0;
        return Read(field, &p, sizeof p) && p && ReadString(p, out, 256);
    }
};

// A vanilla weapon's renamed panel text. The game formats "Text.%s" / "HelpText.%s%d" with the name it reads from its
// name table, so a rename is a pair of string-table entries under a short key of our own ("wt" + the rule index) plus a hook
// that hands the game that key in place of the weapon's name.
struct TextRule {
    manifest::TextDecl decl;
    std::string token;           // the "name" the game formats into the keys; c_str() is stable after Configure
    int id = -1;                 // the weapon's slot in the name table, -1 until resolved for a match
    uintptr_t slot = 0;          // what that slot held when resolved: the hook compares against it
    bool text = false, help = false;  // registered for this match
    // The in-world weapon-name tag reads the container's DisplayName, not the panel keys: while the rename is live that
    // field holds "Text.<token>", and tagOrig is what to put back. tagContainer/tagField are 0 when nothing was written.
    uintptr_t tagContainer = 0, tagField = 0;
    std::string tagOrig;
};

// A vanilla weapon's replacement icons (weaponIcons). The panel icon is written over the sub-icon the weapon's own panel
// cell names; the HUD icon is substituted when the game is about to load the weapon's vanilla HUD file.
struct IconRule {
    manifest::IconDecl decl;
    int id = -1;                  // the weapon's slot in the name table, -1 until resolved for a match
    uint32_t iconCode = 0;        // the vanilla panel icon code the PNG was written over (atlas | sub << 8)
    bool panel = false, hud = false;  // armed for this match
    const char* hudFile = nullptr;    // the vanilla HUD file name this rule replaces (static storage)
};

// The vanilla HUD icon file (Data\HUD\Weapons\<file>) of a vanilla weapon container name, nullptr when the name is not
// in the table. The name is the container name minus kWeapon/kUtility, lower-cased, through a few aliases.
const char* VanillaHudFile(const std::string& weapon);

// A vehicle's replacement mesh (vehicleMeshes). Armed per match when the mod's mesh is loaded and the engine accepted the swap.
struct VehicleRule {
    manifest::VehicleDecl decl;
    bool armed = false;
};

struct Clone {
    CloneInfo info{};
    manifest::CloneDecl decl;
    const char* namePtr = nullptr;  // info.name; its address stands in for a name slot
    char textKey[64] = {};
    bool text = false, help = false, hud = false;
    bool created = false;  // registered in this launch, so a container by that name may be ours
    bool iconAsked = false;
};

class Registry {
public:
    explicit Registry(Engine& e) : e_(e) { std::fill(std::begin(ruleOf_), std::end(ruleOf_), int16_t{-1}); }
    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    // The frozen tables; reads the original name slots. texts = the vanilla renames (see TextRule), none by default.
    void Configure(const std::vector<manifest::CloneDecl>& decls, const std::vector<manifest::TextDecl>& texts = {},
                   const std::vector<manifest::IconDecl>& icons = {}, const std::vector<manifest::VehicleDecl>& vehicles = {});
    bool Init(std::string* why);                                    // match Init: every clone, or none; then the renames
    void MatchEnd();
    void TurnEnded();
    int32_t Select(int32_t value);  // selection handler: the id to store in the worm's selected weapon

    // Hook helpers (no logging, no allocation).
    int32_t GuardId(int32_t id) const;
    uintptr_t TextName(int32_t id, uintptr_t current, bool help) const;
    uintptr_t CanUseSlot(int32_t id, uintptr_t current) const;
    const char* HudName() const;
    // The HUD file to load instead of `incoming` (the file name or path the game is about to load): the active clone's
    // hudIcon, else the replacement of a vanilla weapon whose HUD file `incoming` is. nullptr = leave it alone. Only
    // answers for a vanilla weapon while its rule is armed for this match. No allocation.
    const char* HudNameFor(const char* incoming) const;
    bool HudIconsLive() const { return hudIconsLive_; }

    int Count() const { return n_; }
    bool Live() const { return live_; }
    // Vanilla renames. TextLive: at least one rule is registered for this match, so TextName may answer for vanilla ids.
    bool TextLive() const { return textLive_; }
    int TextCount() const { return static_cast<int>(rules_.size()); }
    int TagCount() const;  // renames whose container DisplayName is currently pointed at the rename
    const TextRule* TextAt(int i) const { return i >= 0 && i < TextCount() ? &rules_[i] : nullptr; }
    // Vanilla icon replacements. IconsLive: at least one rule is armed (panel and/or HUD) for this match.
    bool IconsLive() const { return iconsLive_; }
    int IconCount() const { return static_cast<int>(iconRules_.size()); }
    const IconRule* IconAt(int i) const { return i >= 0 && i < IconCount() ? &iconRules_[i] : nullptr; }
    // Vehicle meshes. VehiclesLive: at least one rule is armed for this match.
    bool VehiclesLive() const { return vehiclesLive_; }
    int VehicleCount() const { return static_cast<int>(vehicleRules_.size()); }
    const VehicleRule* VehicleAt(int i) const { return i >= 0 && i < VehicleCount() ? &vehicleRules_[i] : nullptr; }
    int Active() const { return active_; }
    int SwappedBase() const { return swapped_ < 0 ? -1 : clones_[swapped_].info.base; }
    const Clone* At(int k) const { return k >= 0 && k < n_ ? &clones_[k] : nullptr; }
    const CloneInfo* ByDesc(uintptr_t desc) const;
    int32_t BaseOf(int32_t v) const;  // a declared vid's base, else kFallbackBase
    bool IsVidValue(int32_t v) const { return static_cast<uint32_t>(v) >= static_cast<uint32_t>(kVidBase); }

private:
    bool Create(Clone& c, std::string* why);
    bool ApplySet(Clone& c, std::string* why);
    void RegisterText(Clone& c);
    void ResolveText();
    void ResetText();
    void ResolveIcons();
    void ResetIcons();
    void ResolveVehicles();
    void ResetVehicles();
    int NameSlot(const std::string& weapon, uintptr_t* slot);  // the weapon's name-table id, -1 if absent
    bool SetTag(TextRule& r, uintptr_t container);
    void RestoreTag(TextRule& r);
    uintptr_t Renamed(int32_t id, uintptr_t current, bool help) const;
    bool PutText(const std::string& key, const std::string& value);
    uint32_t BaseIcon(int32_t base);
    void Swap(int k);
    void Unswap();
    void RestoreCells();
    void Reset();
    uint32_t U32(uintptr_t a);
    bool PutU32(uintptr_t a, uint32_t v);

    Engine& e_;
    std::array<Clone, kMaxClones> clones_{};
    int n_ = 0;
    uintptr_t orig_[kEnumCount] = {};
    bool live_ = false;
    int active_ = -1, swapped_ = -1;
    bool cellWritten_[kMaxClones] = {};
    std::vector<TextRule> rules_;
    int16_t ruleOf_[kEnumCount];  // name-table id -> index into rules_, -1 for none
    bool textLive_ = false;
    std::vector<IconRule> iconRules_;
    bool iconsLive_ = false, hudIconsLive_ = false;
    std::vector<VehicleRule> vehicleRules_;
    bool vehiclesLive_ = false;
};
}  // namespace melange::weapons::core
