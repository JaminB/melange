#pragma once
#include <string>
#include <vector>

#include "erg/luagen.h"
#include "erg/scene.h"

// Map pack export: the assets/levels layout of one level, the manifest entry and the patch. Never writes .csh.
namespace melange::erg::pack {
struct File {
    std::string rel;                                       // relative to the pack folder, '/' separators
    std::vector<uint8_t> bytes;
};
struct PackSpec {
    std::string modId, name, version, slug, title;         // title: the level's (the mod name when empty)
    bool source = false;                                   // patch-only form (no assets/levels)
    bool chunk = false;                                    // source form: the built level will ship a chunk
    bool survivor = false;                                 // also list the level as a Survivor copy (Multi.<stem>.S)
    std::vector<File> levelFiles;                          // built by the level service
    std::string patchJson;
    std::string script;                                    // the level script as sim/<slug>.lua; "" for none
    std::string timeOfDay, image;                          // the level entry's timeOfDay and image ("" = neither written)
    bool spawnAreas = false;                               // the level deals spawn areas (spawns.mode areas)
    std::string melangeRange;                              // spice.json melange.range ("" = ">=0.2.0", or kAreasRange when the
                                                           // level uses timeOfDay, image or spawn areas): a level that needs a
                                                           // newer Melange (e.g. [Fixes] MeshInstances) says so here
};
// The least range of a pack whose level uses 0.15.0's spawn areas, pinned time of day or menu picture: an older Melange
// refuses the level entry's timeOfDay and image keys and deals no areas.
inline constexpr const char* kAreasRange = ">=0.15.0";
// Refused when melangeRange is set and not a valid semver range.
bool WritePack(const PackSpec& spec, const std::wstring& dir, std::vector<std::string>* files, std::string* err);
}  // namespace melange::erg::pack
