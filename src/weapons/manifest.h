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

// Both passes together, for the mods in load order (index i of each list is the same mod; a mod with nothing to
// declare in one kind has an empty entry there). A mod refused by either pass loses both its clones and its renames,
// and the clone names that block a rename are only those of mods still accepted. All reasons are in `refused`.
struct Resolved {
    std::vector<CloneDecl> clones;
    std::vector<TextDecl> texts;
    std::vector<Error> refused;
};
Resolved Resolve(const std::vector<std::vector<CloneDecl>>& clonesPerMod, const std::vector<std::vector<TextDecl>>& textsPerMod);
void FreezeText(std::vector<TextDecl> decls);    // once per launch, like Freeze
bool IsTextFrozen();
const std::vector<TextDecl>& FrozenText();
}  // namespace melange::weapons::manifest
