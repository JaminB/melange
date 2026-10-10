#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "melange/weapons.h"
#include "mods/spice.h"

// The spice.json "weapons" array: validation against the base whitelist and the container schema, and the
// per-launch k order (mod load order, then array order). Pure functions apart from Freeze/Frozen.
namespace melange::weapons::manifest {
struct SetValue {
    std::string field;
    FieldType type = FieldType::None;
    double number = 0;
    bool boolean = false;
    std::string string;
};
struct Text {
    std::string name, help;
};
struct CloneDecl {
    std::string mod, name, base;
    int baseId = -1;
    int cell = -1;
    std::string bank;
    std::vector<SetValue> set;
    std::string panelIcon, hudIcon;
    Text text;
    uint16_t k = 0;
};
struct Error {
    std::string mod, text;
};
constexpr const char* kContainerClass = "PayloadWeaponPropertiesContainer";

int BaseId(const std::string& base);             // v1 whitelist; -1 otherwise
const char* BaseName(int id);                    // nullptr unless whitelisted
bool ValidName(const std::string& name);
std::vector<CloneDecl> Parse(const spice::Manifest& m, std::vector<Error>* errs);  // empty when refused
// Mods in load order, each with its own Parse() result. A mod whose clones exceed the free cells, reuse another mod's
// name or cell is refused as a whole (its reason in *refused). Returns the accepted clones with k and cell set.
std::vector<CloneDecl> Assign(const std::vector<std::vector<CloneDecl>>& perModInLoadOrder, std::vector<Error>* refused);

void Freeze(std::vector<CloneDecl> decls);       // once per launch; later calls are ignored
bool IsFrozen();
const std::vector<CloneDecl>& Frozen();

// The spice.json "weaponText" object: new panel name and help text for vanilla weapons. Display only, so nothing
// here touches a container; the registry answers the game's text lookups for these (registry_core.cpp).
struct TextDecl {
    std::string mod, weapon, name, help;  // name and/or help empty = that text is not renamed
};
constexpr size_t kMaxTextPerMod = 64;
constexpr size_t kMaxTextName = 24, kMaxTextHelp = 160;

bool ValidTextKey(const std::string& key);       // ^k(Weapon|Utility)[A-Z][A-Za-z0-9]{2,40}$
// Empty when refused (the reasons in *errs). Rechecks what spice.cpp checked, since a Manifest can come from anywhere.
std::vector<TextDecl> ParseText(const spice::Manifest& m, std::vector<Error>* errs);
// Mods in load order, each with its own ParseText() result; cloneNames = every clone name declared by any mod. A mod
// that renames a weapon an earlier mod already renamed, or that renames a clone, is refused as a whole (reason in
// *refused, naming both mods). Returns the accepted renames in load order.
std::vector<TextDecl> AssignText(const std::vector<std::vector<TextDecl>>& perModInLoadOrder,
                                 const std::vector<std::string>& cloneNames, std::vector<Error>* refused);

// The spice.json "weaponIcons" object: a replacement panel icon (PNG under the assets root) and/or HUD icon (a
// <modId>.*.tga under assets/loose/) for a vanilla weapon. Presentation only; the registry applies them per match.
struct IconDecl {
    std::string mod, weapon, panelIcon, hudIcon;  // an empty icon = that one is not replaced
};
constexpr size_t kMaxIconsPerMod = 64;

// Empty when refused (the reasons in *errs). Same key rules as ParseText; the file names as for a clone's icons.
std::vector<IconDecl> ParseIcons(const spice::Manifest& m, std::vector<Error>* errs);
// Like AssignText: a weapon claimed by an earlier mod, or a clone name, refuses the later mod as a whole.
std::vector<IconDecl> AssignIcons(const std::vector<std::vector<IconDecl>>& perModInLoadOrder,
                                  const std::vector<std::string>& cloneNames, std::vector<Error>* refused);

// The spice.json "vehicleMeshes" object: the mesh the Airstrike helicopter (BomberHelicopter) and the Super Airstrike's
// (SuperAirstrike) are drawn with, a "<modId>.<Name>" mesh from one of the mod's banks. The graphic entities read their mesh
// name from a static string, so the registry swaps that string for a match (registry_core.cpp). Presentation only.
struct VehicleDecl {
    std::string mod, vehicle, mesh;
};

// Empty when refused (the reasons in *errs). Rechecks what spice.cpp checked: a known vehicle (spice::kVehicleKeys), a
// "<modId>." mesh name, a kind: content mod that lists a meshes bank.
std::vector<VehicleDecl> ParseVehicles(const spice::Manifest& m, std::vector<Error>* errs);
// Like AssignIcons: a vehicle claimed by an earlier mod refuses the later mod as a whole.
std::vector<VehicleDecl> AssignVehicles(const std::vector<std::vector<VehicleDecl>>& perModInLoadOrder, std::vector<Error>* refused);

// All passes together, for the mods in load order (index i of each list is the same mod; a mod with nothing to
// declare in one kind has an empty entry there). A mod refused by any pass loses its clones, renames, icons and vehicle meshes,
// and the clone names that block a rename or an icon are only those of mods still accepted. All reasons are in
// `refused`. iconsPerMod and vehiclesPerMod may be left out (no such rules).
struct Resolved {
    std::vector<CloneDecl> clones;
    std::vector<TextDecl> texts;
    std::vector<IconDecl> icons;
    std::vector<VehicleDecl> vehicles;
    std::vector<Error> refused;
};
Resolved Resolve(const std::vector<std::vector<CloneDecl>>& clonesPerMod, const std::vector<std::vector<TextDecl>>& textsPerMod,
                 const std::vector<std::vector<IconDecl>>& iconsPerMod = {},
                 const std::vector<std::vector<VehicleDecl>>& vehiclesPerMod = {});
void FreezeIcons(std::vector<IconDecl> decls);   // once per launch, like Freeze
void FreezeVehicles(std::vector<VehicleDecl> decls);   // once per launch, like Freeze
bool IsVehiclesFrozen();
const std::vector<VehicleDecl>& FrozenVehicles();
bool IsIconsFrozen();
const std::vector<IconDecl>& FrozenIcons();
void FreezeText(std::vector<TextDecl> decls);    // once per launch, like Freeze
bool IsTextFrozen();
const std::vector<TextDecl>& FrozenText();
}  // namespace melange::weapons::manifest
