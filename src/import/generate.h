#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "import/plan.h"
#include "import/recipe.h"

// Format 1 output: the rebuilt descriptor, the generated spice.json, the local catalogue and the import fingerprint.
// Every function here is byte-stable for a given input; changing what it writes needs a new format.
namespace melange::import {
struct DescriptorOut {
    std::string theme, timeOfDay, materialFile;
    std::optional<std::string> heightmapBase, heightmapSecond;
    uint32_t customTextureBank = 0, customDetailBank = 0;
};
bool BuildDescriptor(const DescriptorOut& d, std::vector<uint8_t>* out, std::string* err);
// TimeOfDay normalised to the recipe's allowed values (ASCII case-insensitive), else the fallback.
std::string NormalTimeOfDay(const Recipe& r, const std::string& tod);
// Printable ASCII, trimmed, at most `max` characters; "" when nothing is left.
std::string CleanTitle(const std::string& raw, size_t max);

struct PackLevel { std::string slug, title; bool survivor = true; };
struct PackSpec {
    std::string id, name, description, author, melangeRange, generatedBy, recipe;
    std::string version;
    std::vector<PackLevel> levels;
};
std::string SpiceJson(const PackSpec& p);   // fixed key order, two-space indent, LF, trailing newline

struct MapInfo {
    std::string file, stem, pack, title, author, group, groupLabel, category, categoryLabel, mode, theme, timeOfDay;
    bool survivor = true, preview = false;
};
std::string CatalogueJson(const std::vector<MapInfo>& maps);
bool ParseCatalogue(const std::string& text, std::vector<MapInfo>* out);

// SHA-256 over the sorted "<pack>/<path>\t<sha256>\n" lines of every pack file.
std::string Fingerprint(std::vector<std::pair<std::string, std::string>> fileHashes);
}  // namespace melange::import
