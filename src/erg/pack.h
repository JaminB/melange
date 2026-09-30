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
    std::vector<File> levelFiles;                          // built by the level service
    std::string patchJson;
    std::string script;                                    // the level script as sim/<slug>.lua; "" for none
};
bool WritePack(const PackSpec& spec, const std::wstring& dir, std::vector<std::string>* files, std::string* err);
}  // namespace melange::erg::pack
