#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// An importer recipe (import-1): where a zip comes from, which members to read, how to select, label and pack the
// maps. A recipe only selects and labels; every byte the importer writes is fixed by the importer format. Pure.
namespace melange::import {
constexpr int kImportVersion = 1;
constexpr int kFormat = 1;   // the only importer format this Melange writes
constexpr size_t kMaxPacks = 9;

struct Source {
    std::string id, name, fileName, sha256;
    std::vector<std::string> urls;
    uint64_t size = 0;
};
struct Content { std::string title, publisher, termsUrl, credit; };
struct Reader {
    std::string type, root, registry, descriptors, previews;
    std::vector<std::string> titles, maps;
};
struct VanillaPin {
    std::string file;
    std::vector<std::pair<std::string, std::string>> sha256;   // path under Data/ -> hex
};
struct Select {
    std::vector<int> levelTypes;   // "levelType": one value or a list
    std::string skipKeySuffix;
    std::vector<std::string> require, exclude;
    std::vector<VanillaPin> vanilla;
    int expectMaps = 0, expectArchive = 0, expectGame = 0;
};
struct Category {
    std::string id, label;
    std::vector<std::string> scriptsEqual, scriptsWithin;
    bool isDefault = false, hidden = false;
};
struct Group {
    std::string id, label;
    std::vector<std::string> match;
    bool vanilla = false, isDefault = false;
};
struct Transform {
    int slugMax = 24, hashKeep = 18, hashHex = 6;
    std::vector<std::string> timeOfDay;
    std::string timeOfDayFallback = "DAY";
    int titleMax = 40;
    std::string author;
    bool survivor = true, previews = false;
};
struct Output {
    std::string packPrefix, version, packName, packDescription;
    int perPack = 32;
    std::vector<std::string> newPackBefore;
};
struct Recipe {
    int format = 0;
    std::string id, name;
    Content content;
    std::vector<Source> sources;
    Reader reader;
    Select select;
    std::vector<Category> categories;
    std::vector<std::pair<std::string, std::string>> modes;   // pattern -> label, in recipe order
    std::vector<Group> groups;
    Transform transform;
    Output output;
};

// Hosts a recipe may download from or link to.
bool AllowedHost(std::string_view host);
std::string HostOf(std::string_view httpsUrl);   // "" unless https://host[:443]/... without userinfo
bool ValidRecipeId(std::string_view id);

// Parses and validates; `pluginId` must equal output.packPrefix. A recipe whose format this Melange cannot write
// parses with *unsupported set (and false) so the UI can say so.
bool ParseRecipe(std::string_view text, const std::string& pluginId, Recipe* out, std::string* err, bool* unsupported = nullptr);

// '*' matches any run of characters (within a segment for paths); ASCII case-insensitive.
bool GlobMatch(std::string_view pattern, std::string_view s);
}  // namespace melange::import
