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
// One entry of "weaponText": a vanilla weapon's new panel name and help text, checked for shape only;
// weapons/manifest.cpp checks it against the clone names of every mod. An empty name or help means "not renamed".
struct WeaponText { std::string weapon, name, help; int line = 0; };
// One entry of "weaponIcons": new panel and/or HUD icon files for a vanilla weapon, checked for shape only;
// weapons/manifest.cpp checks the file names and the clone names of every mod. An empty field means "not replaced".
struct WeaponIcon { std::string weapon, panelIcon, hudIcon; int line = 0; };
// One entry of the "levels" array, checked for shape only; levels/manifest.cpp checks slugs, stems and limits.
struct Level { std::string slug, title, type = "multi", source; bool chunk = false; int line = 0; std::string sim; bool survivor = false; };
// One entry of "schemes" / "factoryWeapons": a data file in the mod folder, checked for shape only; schemes/builder.cpp reads it.
struct DataFile { std::string file; int line = 0; };
// One entry of "music": an MP3 for a music slot, checked for shape only; music/bank.cpp reads the file.
struct Music { std::string slot, file, title, credit; int line = 0; };
// One entry of "meshes": a mesh bank (.xom) under the mod's assets root, checked for shape only; assets/meshbank.cpp loads it.
struct MeshFile { std::string file; int line = 0; };
// One entry of "vehicleMeshes": the mesh an engine-picked vehicle (the Airstrike helicopter, the Super Airstrike's) is drawn
// with, checked for shape only; weapons/manifest.cpp checks it again and resolves clashes between mods.
struct VehicleMesh { std::string vehicle, mesh; int line = 0; };
constexpr size_t kMaxMeshes = 64;  // "meshes" entries per mod; the engine has 44 sections for all mods together (assets/meshbank.h)
// The engine-picked vehicles "vehicleMeshes" can name (the graphic entities' mesh names, docs/meshes.md): the Airstrike
// helicopter and the Super Airstrike's. The vanilla "Bomber" mesh is not drawn by any entity in this build.
inline constexpr const char* kVehicleKeys[] = {"BomberHelicopter", "SuperAirstrike"};
// A mod mesh name as vehicleMeshes takes it: "<modId>." and then 1-96 of [A-Za-z0-9._-] in all; whether a loaded bank has it is
// checked when the match starts.
bool ValidVehicleMeshName(const std::string& modId, const std::string& name);
struct Manifest {
    std::string id, version, name, description, website, melangeRange; std::vector<std::string> authors;
    bool content = false, unsafe = false, defaultEnabled = true, implicit = false;
    std::string filesystem = "none", entryClient, entrySim, assetsRoot = "assets", shaders = "shaders", effects = "effects";
    std::vector<Dep> dependencies, optional, conflicts; std::vector<std::string> loadAfter, messages, hashInclude;
    std::vector<Setting> settings; std::vector<Weapon> weapons;
    std::vector<WeaponText> weaponText;
    std::vector<WeaponIcon> weaponIcons;
    std::vector<Level> levels; std::vector<DataFile> schemes, factoryWeapons; std::vector<Music> music; std::vector<MeshFile> meshes; std::vector<VehicleMesh> vehicleMeshes; std::wstring dir;
    std::string importerRecipe;                       // "importer": {"recipe"}: a local content importer's recipe file
    std::string generatedBy, generatedRecipe;         // "generated": a pack an importer made on this PC
    int generatedFormat = 0;
    // Optional "graphics" block: a client-only mod's request for texture clarity (Mirage's MirageTextures
    // component, and the shadow-map size for MirageShadows), applied unless the user overrides it in Melange.ini.
    // Never affects the simulation.
    bool graphicsPresent = false, graphicsTrilinear = false, graphicsLodBiasSet = false;
    int graphicsAnisotropy = 0, graphicsShadowMapSize = 0;  // shadowMapSize 0 = no request
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
