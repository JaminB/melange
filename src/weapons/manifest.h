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
}  // namespace melange::weapons::manifest
