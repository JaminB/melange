#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The shadow-cache guard: a level's generated <stem><TOD>.csh files are deleted whenever its .xan changed. A sidecar
// <stem>.xan.sha (the .xan's sha256) records the .xan the current shadows belong to. File-system only, no game calls.
namespace melange::levels::csh {
struct Result {
    bool ok = false;          // the .xan was read and the sidecar is current
    bool changed = false;     // the .xan differs from the sidecar (or there was none)
    uint32_t deleted = 0;     // .csh files removed
    std::string sha;
    std::string error;
};
// xan: the level's .xan; sidecarDir: where <stem>.xan.sha lives; mapsDirs: every Maps folder a .csh can be in.
// A .csh older than the .xan is deleted even when the sha is unchanged. Idempotent.
Result Guard(const std::filesystem::path& xan, const std::string& stem, const std::filesystem::path& sidecarDir,
             const std::vector<std::filesystem::path>& mapsDirs);
// Deletes every <stem><TOD>.csh in mapsDirs; returns the count.
uint32_t Purge(const std::string& stem, const std::vector<std::filesystem::path>& mapsDirs);
}  // namespace melange::levels::csh
