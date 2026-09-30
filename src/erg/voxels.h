#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "erg/patch.h"
#include "erg/scene.h"

// Terrain sculpting: the voxel word rules, applying and diffing `voxels` ops, and the shadow-cache files a terrain edit
// makes stale. Words are u32: bits 0-1 solid (3) or empty (0), 2-7 material, 8-9 second-material flag, 10-15 second
// material, 16-23 blend mask, 24-31 clear. Index order (z*X + x)*Y + y. No game calls.
namespace melange::erg::voxels {
constexpr uint32_t kSolid = 3, kMaterialMask = 0xfc, kKeptMask = 0x00ffff00;
constexpr uint32_t kMaterials = 64;

// Level services pass this as PatchRules::voxels.
constexpr bool kAccepted = true;

inline bool Solid(uint32_t v) { return (v & 3u) == kSolid; }
inline uint32_t Material(uint32_t v) { return (v >> 2) & 63u; }
// Fill: the brush material, no second material, no blend.
inline uint32_t Filled(uint32_t material) { return kSolid | ((material & 63u) << 2); }
// Carve: only the solid bits clear; the material stays, as the game's own empties keep theirs.
inline uint32_t Carved(uint32_t v) { return v & ~3u; }
// Paint: a solid voxel's material; second material and blend stay.
inline uint32_t Painted(uint32_t v, uint32_t material) { return Solid(v) ? (v & ~kMaterialMask) | ((material & 63u) << 2) : v; }

// Every composition of carve, fill and paint over `base` gives a word whose bits 8-23 are the base's or zero.
bool ValidEdit(uint32_t base, uint32_t now);

size_t Cells(const Frame& f);
bool Decode(const std::vector<uint8_t>& le, std::vector<uint32_t>* out, std::string* err);   // u32 LE
std::vector<uint8_t> Encode(const std::vector<uint32_t>& words);

using FrameWords = std::map<int64_t, std::vector<uint32_t>>;   // frame id -> voxel words

struct Stats { size_t frames = 0, runs = 0, changed = 0, carved = 0, filled = 0, painted = 0; };

// Applies the patch's voxels ops in order to `words` (the base's voxels by frame id). Every written word is checked
// against the base word with ValidEdit; errors name the op and run. On failure `words` is left unchanged.
bool ApplyOps(const Patch& p, const Scene& base, FrameWords& words, std::string* err, Stats* stats = nullptr);
// The same over the scene's blobs (blob ref -> u32 LE bytes), as the level service holds them.
bool ApplyToBlobs(const Patch& p, const Scene& base, std::map<int64_t, std::vector<uint8_t>>& blobs, std::string* err,
                  Stats* stats = nullptr);

// Changed words as [start, count, value] runs, merging neighbours with the same new value (the editor's voxelRuns).
std::vector<VoxelRun> Runs(const std::vector<uint32_t>& before, const std::vector<uint32_t>& after);
// One voxels op per changed frame, in the base's frame order. Refuses a frame over 2000 runs or an invalid word.
bool DiffOps(const Scene& base, const FrameWords& before, const FrameWords& after, std::vector<Op>* out, std::string* err);

bool TerrainChanged(const Patch& p);

// The shadow-cache files the engine derives from a level's voxels: Maps/<stem><DAY|EVENING|NIGHT>.csh.
std::vector<std::string> ShadowFiles(std::string_view stem);
// Deletes those files under each game-relative root (as the engine's search roots are); a root that is absolute,
// climbs with "..", or starts in Data or Data2 is refused. Returns how many files were removed.
size_t DeleteShadows(const std::filesystem::path& game, const std::vector<std::string>& roots, std::string_view stem,
                     std::string* err);
}  // namespace melange::erg::voxels
