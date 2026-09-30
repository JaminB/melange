#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// erg-scene/1 and /2: the editable scene the level service sends to the editor. Positions, scales and voxel
// coordinates are .xan units as stored; water is in world units (20 per .xan unit), sea level 0. A /1 document is read
// as a /2 scene without the /2 features; a scene is written as /1 whenever it uses none of them. No game or OS calls.
namespace melange::erg {
constexpr std::string_view kSceneFormat = "erg-scene/1", kSceneFormat2 = "erg-scene/2";
constexpr int kWorldPerXan = 20;
constexpr size_t kMaxFrames = 4096, kMaxDetails = 65536, kMaxFrameVoxels = 262144;
constexpr size_t kMaxObjects = 256, kMaxNewFrames = 64, kMaxNewFrameSide = 32, kMaxTelepadGroups = 8;
constexpr size_t kHmpSide = 100, kHmpCells = kHmpSide * kHmpSide, kHmpBytes = kHmpCells * 5;   // f32 heights + u8 blend

using Vec3 = std::array<double, 3>;

enum class Role : uint8_t { Scenery, Spawn, Object, Camera, Light, Emitter, Sound, Collision, Marker, Other };
const char* RoleName(Role r);
bool ParseRole(std::string_view s, Role* out);
Role DeriveRole(std::string_view name, std::string_view resource);

enum class SpawnMode : uint8_t { Random, Knots };
enum class HmpMode : uint8_t { Copy, None, Flat, Paint };
const char* SpawnModeName(SpawnMode m);
const char* HmpModeName(HmpMode m);
bool ParseSpawnMode(std::string_view s, SpawnMode* out);
bool ParseHmpMode(std::string_view s, HmpMode* out);

struct Sha256Set { std::string xan, xom, hmp; };      // lower-case hex; hmp "" when the base has none
struct BaseRef {
    std::string key;                                  // "Multi.DinerMight"
    std::string source = "game";                      // "game" or "pack"
    std::string file;                                 // level file stem of the base
    Sha256Set sha256;
};
struct RegistryInfo {
    int levelType = 0, themeType = 5, previewType = 0;
    std::vector<std::string> scripts{"stdvs", "wormpot"};
};
struct DatabankInfo { std::string theme, timeOfDay, materialFile, heightmapBase, heightmapSecond; };

struct Frame {
    int64_t id = 0, parent = -1;                      // object indices in the base .xan; parent -1 for the root
    std::string name;
    Vec3 pos{0, 0, 0}, rot{0, 0, 0}, scale{1, 1, 1};  // rot: Euler radians
    std::array<int, 3> size{0, 0, 0};                 // X, Y, Z voxels
    int64_t voxels = -1, heightMap = -1;              // blob refs, -1 = none
    bool folder = false;                              // 1x1x1, identity rotation and scale
    bool isNew = false;                               // added by the author: id -1..-64, under the Scene frame
};
struct Detail {
    int64_t id = 0;
    std::optional<int64_t> src;                       // object index in the base .xan; empty for an added detail
    int64_t frame = 0;
    std::string name, resource;
    Vec3 pos{0, 0, 0}, rot{0, 0, 0}, scale{1, 1, 1}, voxelPos{0, 0, 0};
    Role role = Role::Other;
};
struct Blob {
    int64_t ref = 0;
    std::string kind;                                 // "voxels" (u32 LE), "heightMap" (f32 LE) or "hmp" (the surround)
    int64_t frame = 0;                                // 0 for "hmp"
    uint64_t bytes = 0;
};

// Level objects (erg-scene/2). Each is a detail named by its knot; the detail carries the position.
constexpr std::string_view kKnotResource = "CheesyGrinWorm";   // the non-visual marker spawn knots use
enum class ObjectType : uint8_t { Crate, Telepad, Trigger, MineFactory };
enum class CrateKind : uint8_t { Weapon, Health, Utility };
struct CrateSpec {
    CrateKind kind = CrateKind::Weapon;
    std::string contents;                             // weapon/utility: an install name (kWeapon..., kUtility...)
    int count = 1;                                    // weapon/utility: 1-99
    int amount = 0;                                   // health: 1-500
    int hitpoints = 25;                               // 1-1000
    bool parachute = false;
};
struct TriggerSpec {
    int index = 0;                                    // 0-255
    double radius = 60.0;                             // world units, 1-1000
    int teamCollect = 0, teamDestroy = 4;             // 0-8
    int hitpoints = 1;                                // 0-1000
    bool wormCollect = false;
};
struct ObjectSpec {
    std::string knot;
    ObjectType type = ObjectType::Crate;
    CrateSpec crate;
    int group = 0;                                    // telepad: 1-8
    TriggerSpec trigger;
};
struct ScriptMeta {
    bool present = false;
    std::string sha256;                               // of script.lua; "" when absent
};
const char* ObjectTypeName(ObjectType t);
const char* CrateKindName(CrateKind k);
bool ParseObjectType(std::string_view s, ObjectType* out);
bool ParseCrateKind(std::string_view s, CrateKind* out);   // weapon, health, utility (target and mystery are refused)
// The knot rules: CRATE_<n>, TP_<g>_<n> (g 1-8, the telepad's group), TRIG_<n> (n 0-255, no leading zeros), and the
// bare engine name minefactory. A detail named exactly "telepad" is never written.
bool ValidKnot(std::string_view knot, ObjectType type, int group);
bool IsObjectKnot(std::string_view name);             // CRATE_<n>, TP_<g>_<n>, TRIG_<n>, minefactory: an object
bool ValidContentsName(std::string_view s);            // [A-Za-z][A-Za-z0-9_]{0,62}
bool ValidateObject(const ObjectSpec& o, const std::string& path, std::string* err);   // shape and ranges

struct Scene {
    std::string stem, title;
    BaseRef base;
    RegistryInfo registry;
    DatabankInfo databank;
    std::optional<double> water;                      // world units; empty = the stdvs/Tweak default
    SpawnMode spawns = SpawnMode::Random;
    HmpMode hmp = HmpMode::Copy;
    int worldPerXan = kWorldPerXan;
    std::vector<Frame> frames;
    std::vector<Detail> details;
    std::vector<Blob> blobs;
    bool survivor = false;                            // also registered as a Survivor map
    std::vector<ObjectSpec> objects;
    ScriptMeta script;
    int64_t hmpRef = -1;                              // the painted surround's blob (hmp paint)

    const Frame* FindFrame(int64_t id) const;
    const Detail* FindDetailBySrc(int64_t src) const;
    bool UsesV2() const;                              // any erg-scene/2 feature: written as /2
};

// True when the frame is named "Scene" or has such an ancestor (new frames go only there).
bool UnderSceneFrame(const Scene& s, int64_t frameId);

// Parses and checks the shape, the limits and the references (frame parents, detail frames, blob refs and sizes).
bool ParseScene(std::string_view json, Scene* out, std::string* err);
bool ValidateScene(const Scene& s, std::string* err);
bool ValidateObjects(const Scene& s, std::string* err);   // knots, limits, and one added detail per knot
std::string WriteScene(const Scene& s);               // compact JSON, stable key order

// The eleven fixed themes and the times of day a databank may name.
bool ValidTheme(std::string_view theme);
bool ValidTimeOfDay(std::string_view tod);
bool PrintableAscii(std::string_view s, size_t minLen, size_t maxLen);
bool DatabankShape(const DatabankInfo& d, bool partial, std::string* err);   // partial: empty fields are allowed

// Frame-to-world transform (.xan units), composed from the frame up to the root (whose own transform is ignored).
struct Mat3x4 { double m[3][4]; };
Mat3x4 FrameLocal(const Frame& f);                    // T(pos) * R(rot) * S(scale)
bool FrameWorld(const Scene& s, int64_t frameId, Mat3x4* out);
bool DetailWorld(const Scene& s, const Detail& d, Vec3* out);
Vec3 Apply(const Mat3x4& m, const Vec3& p);
Mat3x4 Multiply(const Mat3x4& a, const Mat3x4& b);
}  // namespace melange::erg
