#pragma once
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "melange/mods.h"

// spice.json parsing and mod resolution as pure functions (offline self-test).
namespace melange::spice {
struct Dep { std::string id, range; };
struct Setting { std::string key, type, label, def; double min = 0, max = 0; std::vector<std::string> options; };
// One entry of the "weapons" array, checked for shape only; weapons/manifest.cpp checks names, bases and field types.
struct WeaponSet { std::string field; enum Kind { Number, Boolean, String } kind = Number; double number = 0;
                   bool boolean = false; std::string string; int line = 0; };
struct Weapon { std::string name, base, bank, panelIcon, hudIcon, textName, textHelp; int cell = -1;
                std::vector<WeaponSet> set; int line = 0; };
// One entry of the "levels" array, checked for shape only; levels/manifest.cpp checks slugs, stems and limits.
struct Level { std::string slug, title, type = "multi", source; bool chunk = false; int line = 0; std::string sim; bool survivor = false; };
struct Manifest {
    std::string id, version, name, description, website, melangeRange; std::vector<std::string> authors;
    bool content = false, unsafe = false, defaultEnabled = true, implicit = false;
    std::string filesystem = "none", entryClient, entrySim, assetsRoot = "assets", shaders = "shaders", effects = "effects";
    std::vector<Dep> dependencies, optional, conflicts; std::vector<std::string> loadAfter, messages, hashInclude;
    std::vector<Setting> settings; std::vector<Weapon> weapons;
    std::vector<Level> levels; std::wstring dir;
    // Optional "graphics" block: a client-only mod's request for texture clarity (Mirage's MirageTextures
    // component), applied unless the user overrides it in [MirageTextures]. Never affects the simulation.
    bool graphicsPresent = false, graphicsTrilinear = false, graphicsLodBiasSet = false;
    int graphicsAnisotropy = 0;
    double graphicsLodBias = 0;
};
struct Error { std::string field; int line = 0, col = 0; std::string text; };
bool Parse(const std::wstring& dir, Manifest* out, std::vector<Error>* errs);  // synthesises the implicit manifest
struct Resolved { std::string id; mods::State state; std::string reason; int order; };
std::vector<Resolved> Resolve(const std::vector<Manifest>& all, const std::set<std::string>& userEnabled,
                              const std::string& melangeVersion, const std::vector<std::pair<std::string,std::string>>& pins);
bool SemverSatisfies(const std::string& version, const std::string& range);
bool ValidSemver(const std::string& v);
bool ValidRange(const std::string& range);
int SemverCompare(const std::string& a, const std::string& b);   // -1, 0, 1; both must be valid
bool ValidModId(const std::string& id);
}
