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
constexpr size_t kMaxObjects = 256, kMaxNewFrames = 64, kMaxNewFrameSide = 31, kMaxTelepadGroups = 8;
// The engine asserts on a frame side over 31 (LandFramePseudoEntity). Blocks of 32 were made before that was known and
// drew in the game, so a scene or patch with one still loads; level.save warns about it.
constexpr size_t kLegacyNewFrameSide = 32;
// The engine counts the cells (X*Y*Z, solid or empty) of all of a level's voxel frames in 16 bits
// (LandscapeLogicEntity) and crashes loading a level past this. Export, build and Test refuse such a level.
constexpr uint64_t kMaxVoxelCells = 65535;
// A new frame's uniform scale (map units per voxel); the game's own frames use 1.0-2.0, and 0.75 played in the game.
constexpr double kMinNewFrameScale = 0.5, kMaxNewFrameScale = 4;
// A new frame's corner offsets (the .xan's HeightMap: (X+1)*(Z+1) floats, row by row along x, in voxels): the engine
// moves each column's vertices up by the offsets at its corners (bilinear), its bottom too, for drawing and collision.
// The game's own frames use -1.9..3.6; 20 played in the game. Optional: a new frame without them gets zeros.
constexpr double kMaxCornerOffset = 64;
constexpr size_t kHmpSide = 100, kHmpCells = kHmpSide * kHmpSide, kHmpBytes = kHmpCells * 5;   // f32 heights + u8 blend

using Vec3 = std::array<double, 3>;

enum class Role : uint8_t { Scenery, Spawn, Object, Camera, Light, Emitter, Sound, Collision, Marker, Other };
const char* RoleName(Role r);
bool ParseRole(std::string_view s, Role* out);
Role DeriveRole(std::string_view name, std::string_view resource);

// Knots: worm slot i on the knot WORMi (WORM0-7). Teams: each worm on WORM<4 x team + its place in the team>
// (WORM0-15), whatever slots the Quick Game deals, so a team's worms stand at its own four knots. Areas: the match
// deals each team one spawn area (SpawnAreas) from its seed, and a team's worms stand on its area's knots.
enum class SpawnMode : uint8_t { Random, Knots, Teams, Areas };
enum class HmpMode : uint8_t { Copy, None, Flat, Paint };
const char* SpawnModeName(SpawnMode m);
const char* HmpModeName(HmpMode m);
bool ParseSpawnMode(std::string_view s, SpawnMode* out);
bool ParseHmpMode(std::string_view s, HmpMode* out);

// spawns.mode areas: named areas of knots in groups (e.g. "sand", "palace"). Area i's knots are the next `knots`
// WORM<n> after the earlier areas' (area 0 on WORM0.., in area order). At match start the generated chunk asks Melange
// for the deal (levels/spawn_deal.cpp): the pattern's steps in order, each an unused area of its group (picked from the
// match's seed, so every peer deals alike; "opposite" picks the partner of an area dealt earlier, by the `opposite`
// pairs, when it has one), then the areas the pattern left in a seeded order; the match's teams are shuffled over the
// dealt areas by the same seed. A team past the areas keeps the game's random spawns, as does a worm past its area's knots.
constexpr size_t kMaxSpawnAreas = 8, kMaxAreaKnots = 8, kMaxAreaKnotsTotal = 32, kMaxDealSteps = 8;
struct SpawnArea {
    std::string name;                                 // 1-24 printable ASCII; the scene's only (the chunk has none)
    std::string group;                                // [a-z][a-z0-9_]{0,15}
    int knots = 4;                                    // 1..kMaxAreaKnots
    bool operator==(const SpawnArea&) const = default;
};
struct DealStep {
    std::string group;
    bool opposite = false;                            // "group:opposite"
    bool operator==(const DealStep&) const = default;
};
struct SpawnAreas {
    std::vector<SpawnArea> areas;                     // 1..kMaxSpawnAreas, at most kMaxAreaKnotsTotal knots in all
    std::vector<DealStep> pattern;                    // 0..kMaxDealSteps; each group names an area's group
    std::vector<std::array<int, 2>> opposite;         // area index pairs; an area is in at most one pair
    int Knots() const;                                // the areas' knots in all
    int First(size_t area) const;                     // the area's first WORM<n>
    bool operator==(const SpawnAreas&) const = default;
};
bool ValidGroupName(std::string_view s);
bool ValidateSpawnAreas(const SpawnAreas& a, std::string* err);   // "spawns.<field>: why"

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
                                                      // (no rotation, uniform scale 0.5-4)
    std::vector<double> corners;                      // a new frame's corner offsets, empty = zeros (kMaxCornerOffset)
};
// A base frame's voxel box cut down to the cells [min, min + size) (erg-scene/2 `trims`, erg-patch/2 `trimFrame`). The
// scene keeps the whole grid, so voxel edits still address it; only the built level holds the kept box, which is what
// the game's cell count sees. Every cell outside the box must be empty. A box with solid cells moves the frame by the
// box's offset from the grid's centre (its details and child frames are moved back); an all-empty box keeps the frame
// where it is: a frame of the base removed from the cell count at the cost of one cell.
struct FrameTrim {
    int64_t frame = 0;
    std::array<int, 3> min{0, 0, 0}, size{1, 1, 1};
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
// The mine factory's settings (optional). Unset fields keep the game's kMineFactoryData (WEAPTWK) values; set ones are
// written there at level start and put back when the match VM closes (levels/registry.cpp, through the sim tweak
// store). The engine's factory reads them at each turn start (stdvs sends GameLogic.StartMineFactory whenever two or
// more alliances are left): it counts down `inactiveTurns` turn starts after it is placed and after each fire, then
// fires at a turn start when fewer than `activation` mines are in the level, min(activation - mines, 10) of them.
inline constexpr size_t kMaxLaunchers = 8;
inline constexpr int kMaxLauncherCoord = 100000;   // world units
inline constexpr size_t kMaxThrowTargets = 16;
inline constexpr int kMaxThrowRange = 500;         // map units
inline constexpr size_t kMaxAvoidLines = 8, kMaxAvoidPoints = 32;
inline constexpr int kMaxAvoidRadius = 50;         // map units
inline constexpr int kMaxVolley = 10;              // the engine fires at most 10 a time
enum class ThrowPattern : uint8_t { Random, Ring, Targets };
const char* ThrowPatternName(ThrowPattern p);                 // "random" | "ring" | "targets"
bool ParseThrowPattern(std::string_view s, ThrowPattern* out);
struct ThrowSpec {
    int minRange = 0;                                 // map units from the launch point, 0-500
    int maxRange = 1;                                 // map units, minRange-500 (1 or more)
    int apex = 15;                                    // map units the arc tops out over the higher end, 1-255
    ThrowPattern pattern = ThrowPattern::Random;
    bool avoidWater = true;
    std::vector<std::array<int, 2>> targets;          // world units (x, z), at most kMaxThrowTargets; pattern targets
    int scatter = 10;                                 // map units round a target a mine lands within, 0-255
    int targetShare = 50;                             // % of a fire's mines aimed at the targets, 0-100
    int volley = 0;                                   // 1-10: at most this many mines a fire; 0: the engine's min(target - mines, 10)
    // Corridors the mines keep off (trails, say): a landing point within avoidRadius map units of one of the `avoid`
    // polylines (world units x, z) is drawn again. 0: none.
    int avoidRadius = 0;                              // map units, 0-50
    std::vector<std::vector<std::array<int, 2>>> avoid;   // at most kMaxAvoidLines lines of 2-kMaxAvoidPoints points
    bool operator==(const ThrowSpec&) const = default;
};
struct FactorySpec {
    std::optional<int> activation;                    // NumMineActivation 0-255 (game 20; 0 never fires)
    std::optional<int> inactiveTurns;                 // NumTurnsInactive 0-255 (game 7)
    bool schemeMines = true;                          // false: the scheme's 15 random start mines are not placed
    bool keepKnot = false;                            // true: a drop that hits no land leaves it at its knot
                                                      // (levels/factory.cpp), not at the match's stale sweep point
    // Where its mines come from (levels/factory.cpp), map units: set, each fired mine appears launchHeight over the
    // factory (1-255, default 20 with launchRadius alone) and launchRadius round it (0-255, default 0) and falls from
    // there, instead of the game's placement (over random worms and objects).
    std::optional<int> launchHeight;
    std::optional<int> launchRadius;
    // Launchers (world units, at most kMaxLaunchers): set, a fire's mines are shared out over them and each erupts
    // straight up from a spot within launchRadius of its launcher (1 map unit over the launcher's height, the spots kept
    // apart), tops out launchHeight over it and falls back there, instead of falling over the factory. The engine runs one factory a match (GameLogicService asserts "Only allowed one mine factory at
    // a time"), so several launch points are this one factory's.
    std::vector<std::array<int, 3>> launchers;
    // The throw (levels/factory.cpp): set, each fired mine is lobbed from its launcher (or the factory) to a landing
    // point minRange-maxRange map units away, chosen by `pattern` (random: any bearing; ring: the fire's mines spread
    // evenly round the compass; targets: targetShare % of them at one of `targets` (world units x, z), within
    // `scatter` of it, the rest random), the arc topping out `apex` map units over the higher of its ends. With
    // avoidWater a landing point whose ground (the engine's own land sweep) is under the water is drawn again. Melange
    // then lands each mine where its flight first meets the land (the engine's mine collision lets them through some
    // ground: docs/erg.md), instead of erupting straight up.
    std::optional<ThrowSpec> throwing;
    bool Default() const { return !activation && !inactiveTurns && schemeMines && !keepKnot && !Launch(); }
    bool Launch() const { return launchHeight || launchRadius || !launchers.empty() || throwing; }
    bool Tweaks() const { return activation || inactiveTurns; }
};
struct ObjectSpec {
    std::string knot;
    ObjectType type = ObjectType::Crate;
    CrateSpec crate;
    int group = 0;                                    // telepad: 1-8
    TriggerSpec trigger;
    FactorySpec factory;                              // minefactory
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
    SpawnAreas spawnAreas;                            // spawns.mode areas only
    HmpMode hmp = HmpMode::Copy;
    int worldPerXan = kWorldPerXan;
    std::vector<Frame> frames;
    std::vector<Detail> details;
    std::vector<Blob> blobs;
    bool survivor = false;                            // also registered as a Survivor map
    std::vector<ObjectSpec> objects;
    ScriptMeta script;
    int64_t hmpRef = -1;                              // the painted surround's blob (hmp paint)
    std::vector<FrameTrim> trims;                     // base frames cut down in the build (erg-scene/2)

    const Frame* FindFrame(int64_t id) const;
    const FrameTrim* FindTrim(int64_t frameId) const;
    const Detail* FindDetailBySrc(int64_t src) const;
    bool UsesV2() const;                              // any erg-scene/2 feature: written as /2
};

// True when the frame is named "Scene" or has such an ancestor (new frames go only there).
bool UnderSceneFrame(const Scene& s, int64_t frameId);

// X*Y*Z summed over every frame with voxels (base and new; a trimmed frame's kept box): what the engine's 16-bit cell
// count sees.
uint64_t VoxelCells(const Scene& s);
// A trim's shape against its frame: a base frame with voxels, the box inside its grid. One sentence on failure.
bool ValidTrim(const Scene& s, const FrameTrim& t, std::string* err);
// The engine limits a scene can break and still load here: new frames over 31 a side (a warning: such blocks drew in
// the game) and more than 65535 cells (a crash: CheckEngineLimits refuses it). One sentence each.
std::vector<std::string> EngineWarnings(const Scene& s);
// False with a reason when the game would crash loading the scene (the cell count); export, build and Test call it.
bool CheckEngineLimits(const Scene& s, std::string* err);

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
// Where a frame's voxels sit: the engine centres the grid on the frame's position, so voxel (x, y, z) fills
// [x, x+1] x [y, y+1] x [z, z+1] under FrameWorld * T(-size/2). Details use FrameWorld alone.
bool VoxelWorld(const Scene& s, int64_t frameId, Mat3x4* out);
Vec3 Apply(const Mat3x4& m, const Vec3& p);
Mat3x4 Multiply(const Mat3x4& a, const Mat3x4& b);
}  // namespace melange::erg
