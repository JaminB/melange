#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "erg/scene.h"

// erg-patch/1: an edit against a pinned base, holding only identifiers and the author's own values. Untrusted input:
// every op is checked, and errors name the op index. No game or OS calls.
namespace melange::erg {
constexpr std::string_view kPatchFormat = "erg-patch/1";
constexpr size_t kMaxOps = 20000, kMaxPatchBytes = 4u << 20, kMaxRunsPerFrame = 2000;

struct DetailFields {
    std::optional<std::string> name, resource;
    std::optional<Vec3> pos, rot, scale, voxelPos;
    bool Empty() const { return !name && !resource && !pos && !rot && !scale && !voxelPos; }
};
struct VoxelRun { uint32_t start = 0, count = 0, value = 0; };
struct Op {
    enum class Kind : uint8_t { Set, Add, Remove, Voxels } kind = Kind::Set;
    int64_t src = -1;                                 // set, remove
    int64_t frame = -1;                               // add, voxels
    DetailFields fields;                              // set: the changed fields; add: the new detail
    std::vector<VoxelRun> runs;                       // voxels: [start, count, value] over the frame's index order
};
struct DatabankPatch {
    std::optional<std::string> theme, timeOfDay, materialFile, heightmapBase, heightmapSecond;
    bool Empty() const { return !theme && !timeOfDay && !materialFile && !heightmapBase && !heightmapSecond; }
};
struct Patch {
    std::string stem, title;
    BaseRef base;                                     // key, source and sha256 (no file)
    DatabankPatch databank;
    std::optional<double> water;
    SpawnMode spawns = SpawnMode::Random;
    HmpMode hmp = HmpMode::Copy;
    std::vector<Op> ops;
};

const char* OpName(Op::Kind k);
bool ParsePatch(std::string_view json, Patch* out, std::string* err);   // shape and limits only
std::string WritePatch(const Patch& p);                                  // compact JSON, stable key order

struct PatchRules {
    bool voxels = false;                              // accept voxels ops (terrain sculpting)
    bool anyFrame = false;                            // add may target any frame, not only folder frames
};
// Against the base scene: sources, frames, knots, runs and databank values.
bool ValidatePatch(const Patch& p, const Scene& base, const PatchRules& rules, std::string* err);
// Applies the ops in order to the model (details, databank, water, modes). Voxel runs are checked, and their blobs
// are changed by ApplyRuns.
bool ApplyPatch(Scene& s, const Patch& p, const PatchRules& rules, std::string* err);
bool ApplyRuns(std::vector<uint32_t>& voxels, const std::vector<VoxelRun>& runs, std::string* err);
bool ValidRunValue(uint32_t v);                       // bits 24-31 clear, solid bits 0 or 3
}  // namespace melange::erg
