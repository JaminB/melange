#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// erg-scene/1: the editable scene the level service sends to the editor. Positions, scales and voxel coordinates are
// .xan units as stored; water is in world units (20 per .xan unit), sea level 0. No game or OS calls.
namespace melange::erg {
constexpr std::string_view kSceneFormat = "erg-scene/1";
constexpr int kWorldPerXan = 20;
constexpr size_t kMaxFrames = 4096, kMaxDetails = 65536, kMaxFrameVoxels = 262144;

using Vec3 = std::array<double, 3>;

enum class Role : uint8_t { Scenery, Spawn, Object, Camera, Light, Emitter, Sound, Collision, Marker, Other };
const char* RoleName(Role r);
bool ParseRole(std::string_view s, Role* out);
Role DeriveRole(std::string_view name, std::string_view resource);

enum class SpawnMode : uint8_t { Random, Knots };
enum class HmpMode : uint8_t { Copy, None, Flat };
const char* SpawnModeName(SpawnMode m);
const char* HmpModeName(HmpMode m);
bool ParseSpawnMode(std::string_view s, SpawnMode* out);
bool ParseHmpMode(std::string_view s, HmpMode* out);   // "paint" is refused until a later version

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
    std::string kind;                                 // "voxels" (u32 LE) or "heightMap" (f32 LE)
    int64_t frame = 0;
    uint64_t bytes = 0;
};

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

    const Frame* FindFrame(int64_t id) const;
    const Detail* FindDetailBySrc(int64_t src) const;
};

// Parses and checks the shape, the limits and the references (frame parents, detail frames, blob refs and sizes).
bool ParseScene(std::string_view json, Scene* out, std::string* err);
bool ValidateScene(const Scene& s, std::string* err);
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
