#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "erg/scene.h"
#include "xom/xom.h"

// A base level's files (.xan, level .XOM, optional .hmp) parsed into the erg-scene/1 model. The documents stay with
// the scene so a build edits them in place. Untrusted input: sizes, references and array lengths are checked.
namespace melange::erg::load {
constexpr size_t kMaxXanBytes = 64u << 20, kMaxXomBytes = 16u << 20, kHmpSide = 100;
constexpr size_t kHmpBytes = kHmpSide * kHmpSide * 5;

struct BaseFiles {
    std::string key, source = "game", file, title;   // registry key, "game" | "pack", level file stem, display title
    RegistryInfo registry;
    std::vector<uint8_t> xan, xom;
    std::optional<std::vector<uint8_t>> hmp;
};

struct Loaded {
    xom::Document xan, xom;
    std::optional<std::vector<uint8_t>> hmp;
    Scene scene;                                      // the base as a scene: stem = file, title = the display title
    std::map<int64_t, std::vector<uint8_t>> blobs;    // ref -> bytes (voxels u32 LE, heightMap f32 LE)
};

std::string Sha256(const std::vector<uint8_t>& bytes);
bool LoadScene(const BaseFiles& in, Loaded* out, std::string* err);
// The databank string resource names of the scene's DatabankInfo fields, in DatabankInfo order.
extern const char* const kDatabankKeys[5];
}  // namespace melange::erg::load
