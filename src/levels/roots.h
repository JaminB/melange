#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "assets/crcsafe.h"
#include "levels/manifest.h"
#include "mods/spice.h"

// A map pack's level root (assets/levels) and the engine search roots Melange adds for levels. The checks are pure
// functions of a listing; ListLevelRoot is the only file-system call.
namespace melange::levels::roots {
constexpr const char* kLevelDir = "levels";                 // under the manifest's assets root
constexpr const char* kCacheRel = "Melange/cache";         // generated .csh files, searched first
constexpr const char* kBankRel = "Melange/cache/levels";   // registry banks and .xan sidecars
constexpr const char* kTestRel = "Melange/erg/test";       // the Erg Test workspace
constexpr const char* kTodSuffixes[] = {"DAY", "EVENING", "NIGHT"};

// Files and folders under a level root, relative, '/' separated ("Maps/x.xan"; folders without a trailing '/').
struct Listing {
    bool exists = false;
    std::vector<std::string> files, dirs, other;   // other: links and anything that is not a plain file or folder
};
Listing ListLevelRoot(const std::filesystem::path& dir);

// The naming rules of a level root: <prefix>_* at the top, Maps/<prefix>_* below, nothing else, no .csh, no dot in a
// stem, stems of at most 48 characters, no CRC-listed name and no vanilla level stem.
bool CheckLevelRoot(std::string_view prefix, const Listing& l, const std::vector<assets::crcsafe::Entry>& crcTable,
                    std::string* err);
// Every level's built files are present (manifest::RequiredFiles, case-insensitive).
bool CheckBuilt(const std::vector<manifest::LevelDecl>& decls, const Listing& l, std::string* err);

// One content mod in load order, as Thumper resolved it.
struct PackInput {
    const spice::Manifest* manifest = nullptr;
    std::filesystem::path dir;
};
struct PackVerdict {
    std::string mod;
    bool ok = true;
    std::string reason;
    std::vector<manifest::LevelDecl> levels;
};
using Lister = std::function<Listing(const std::filesystem::path&)>;
// Parse, the per-mod root checks and the cross-mod Assign, in load order. Only mods with a levels array appear.
// crcAvailable false refuses every pack (the table could not be verified).
std::vector<PackVerdict> CheckPacks(const std::vector<PackInput>& inLoadOrder,
                                    const std::vector<assets::crcsafe::Entry>& crcTable, bool crcAvailable,
                                    const Lister& list = ListLevelRoot);

// The order in which the level roots are added: pack roots in load order, the Test workspace, then the cache root
// (a root added later is searched first, so generated .csh files land in the cache).
std::vector<std::string> AddOrder(const std::vector<std::string>& packRoots, bool testRoot, bool cacheRoot);

// Game-relative form of an absolute folder under the game folder ("Mods/x/assets/levels"), "" when outside it or
// when a component could not be used as an engine path.
std::string GameRelative(const std::filesystem::path& gameDir, const std::filesystem::path& dir);

// "<stem><TOD>.csh" for one of kTodSuffixes, case-insensitive.
bool IsShadowOf(std::string_view fileName, std::string_view stem);
}  // namespace melange::levels::roots
