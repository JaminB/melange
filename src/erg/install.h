#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "levels/manifest.h"

// Read-only access to the user's install (<game>\Data) and to map packs, plus the atomic writer the level service
// uses for projects and built packs. Paths are built only from validated stems and names.
namespace melange::erg::install {
struct RegistryEntry {
    std::string key, file, frontendName, scripts, lock;
    int levelType = 0, themeType = 5, previewType = 0;
};
// A map pack: an enabled (or, for xomtool, any) content mod with a levels array.
struct Pack {
    std::string modId;
    std::wstring dir;                                  // Mods\<id>
    std::string assetsRoot = "assets";                 // the manifest's assets.root
    std::vector<levels::manifest::LevelDecl> levels;
};

std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view w);
bool ValidFileStem(std::string_view s);                // [A-Za-z0-9_-]{1,63}
bool Exists(const std::wstring& path);
bool ReadFile(const std::wstring& path, size_t max, std::vector<uint8_t>* out, std::string* err);
bool WriteAtomic(const std::wstring& path, const void* data, size_t n, std::string* err);
bool MakeDirs(const std::wstring& dir);
bool Inside(const std::wstring& path, const std::wstring& dir);   // after normalising both; case-insensitive
// True when no existing folder from `top` down to `dir` is a junction or symbolic link (so writes stay where named).
bool NoReparse(const std::wstring& top, const std::wstring& dir);

std::wstring DataDir(const std::wstring& gameDir);
// Data\Tweak\SCRIPTS.XOM: every WXFE_LevelDetails under its resource key.
bool ReadRegistry(const std::wstring& gameDir, std::vector<RegistryEntry>* out, std::string* err);
// The English frontend strings (Data\Language\PC\EngFE.xom), for level titles; empty when unreadable.
std::map<std::string, std::string> ReadFrontendStrings(const std::wstring& gameDir);
// Material files a databank may name: "Theme<X>\Theme<X>.txt" under Data\Themes and "Maps\<x>.txt" under Data\Maps.
std::vector<std::string> MaterialFiles(const std::wstring& gameDir);
bool MaterialFileExists(const std::wstring& gameDir, const std::string& rel);
// Reads a material file a databank names, from Data or Data\Themes (at most 1 MB).
bool ReadMaterialFile(const std::wstring& gameDir, const std::string& rel, std::vector<uint8_t>* out, std::string* err);
// The record names of a material file: records of six lines (three textures, a blend texture or NULL, the name, a sixth
// texture), separated by blank lines in the game's files (sometimes more than one). At most 64, the voxel's material index range; names are
// printable ASCII ('?' for anything else), at most 63 characters.
std::vector<std::string> MaterialNames(const std::vector<uint8_t>& txt);

// The levels of every mod folder under <game>\Mods whose spice.json declares some (no enabled-state check).
std::vector<Pack> ScanPacks(const std::wstring& gameDir);
bool PackFromManifest(const spice::Manifest& m, const std::wstring& dir, Pack* out);
std::wstring LevelRoot(const Pack& p);                 // <dir>\<assets root>\levels
// Enabled packs in load order, minus the mods the cross-mod limits refuse (a taken prefix, the 128-level cap).
std::vector<Pack> AssignPacks(std::vector<Pack> inLoadOrder);
}  // namespace melange::erg::install
