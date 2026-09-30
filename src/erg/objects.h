#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "erg/scene.h"

// Level objects against the user's install: the crate contents it knows, knot naming and the author warnings.
namespace melange::erg::objects {
struct Catalog {
    std::vector<std::string> weapons, utilities;       // install names (kWeapon..., kUtility...)
};
bool LoadCatalog(const std::filesystem::path& game, Catalog* out, std::string* err);
// Contents against the catalog; warnings for a telepad group with one pad and a crate under the water.
bool Validate(const Scene& s, const Catalog& c, std::vector<std::string>* warnings, std::string* err);
std::string NextKnot(const Scene& s, ObjectType type, int group);   // the lowest free knot name, "" when none is left
}  // namespace melange::erg::objects
