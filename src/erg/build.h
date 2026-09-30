#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "erg/load.h"
#include "erg/patch.h"
#include "erg/scene.h"

// Scene -> level files. The base documents are copied and edited: changed details are rewritten in place, removed ones
// are taken out and added ones inserted with every reference renumbered, voxels are patched in place, and the level
// databank gets the changed strings. Every output parses strictly and re-serialises identically.
namespace melange::erg::build {
struct File {
    std::string rel;                                  // "Maps/<stem>.xan", "<stem>.XOM", "Maps/<stem>.hmp", ...
    std::vector<uint8_t> bytes;
};
// Frame id -> the frame's full voxel array, and the painted surround's .hmp bytes (hmp paint).
struct VoxelEdits : std::map<int64_t, std::vector<uint32_t>> {
    std::optional<std::vector<uint8_t>> hmp;
};

struct Options {
    // A per-map material file of a pack base, shipped as Maps/<stem>.txt (Databank.MaterialFile then points there).
    std::optional<std::vector<uint8_t>> materialTxt;
    bool chunk = true;                                // emit <stem>.lub when the scene needs a generated chunk
};

bool Build(const load::Loaded& base, const Scene& edited, const VoxelEdits& voxels, const Options& opt,
           std::vector<File>* out, std::string* err);
// The optional outputs of `stem` (.hmp, .txt, .lub) that `files` does not hold: left over from an earlier build.
std::vector<std::string> Stale(const std::string& stem, const std::vector<File>& files);

// A patch applied to a loaded base: the scene and the voxel arrays its voxels ops produce. With hmp paint, the surround
// (the base's .hmp or zeros, then the hmp ops in order) goes to voxels->hmp and the scene gets its blob and hmp.ref.
bool Apply(const load::Loaded& base, const Patch& p, const PatchRules& rules, Scene* scene, VoxelEdits* voxels,
           std::string* err);

// The patch that turns `base` into `edited` (details by src, databank keys that differ, water and modes). Voxel edits
// are not visible in scenes; pass them to get runs (the painted surround's against baseHmp, or zeros without one).
Patch Diff(const Scene& base, const Scene& edited, const VoxelEdits& voxels = {},
           const std::map<int64_t, std::vector<uint8_t>>* baseBlobs = nullptr, const std::vector<uint8_t>* baseHmp = nullptr);
}  // namespace melange::erg::build
