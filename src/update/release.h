#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Melange's own releases on GitHub, as data: the /releases/latest answer, the release manifest
// (melange-<version>.json, written by scripts\release-manifest.ps1) and dotted version compares. Pure; shared by
// Melange.exe's updater and the game's once-a-day check.
namespace melange::update {
constexpr char kLatestUrl[] = "https://api.github.com/repos/JaminB/melange/releases/latest";
constexpr char kDownloadPrefix[] = "https://github.com/JaminB/melange/releases/download/";
constexpr size_t kMaxApiBytes = 1u << 20;
constexpr uint64_t kMaxManifestBytes = 16u << 10;
constexpr uint64_t kMaxZipBytes = 128ull << 20;

// Dotted numeric compare: -1, 0, 1. Pre-release and build suffixes are ignored.
int CompareVersions(const std::string& a, const std::string& b);
// "1.2.3": digits and dots only, 1-4 parts, nothing else.
bool PlainVersion(std::string_view v);

struct Asset {
    std::string name, url;
    uint64_t size = 0;
};
struct Release {
    std::string tag, version, htmlUrl;
    std::vector<Asset> assets;
    const Asset* Find(std::string_view name) const;
    std::string ManifestName() const { return "melange-" + version + ".json"; }
    std::string ZipName() const { return "melange-" + version + ".zip"; }
};
// GitHub's release object. The tag must be v<version>; drafts and prereleases are refused (/latest never returns
// them, but a custom endpoint might).
bool ParseRelease(std::string_view json, Release* out, std::string* err);

struct Manifest {
    std::string version, zip, sha256;
    uint64_t size = 0;
};
// melange-<version>.json: {"version", "zip", "sha256", "size"}. `expectVersion` must match and `zip` must be
// melange-<version>.zip; unknown members are ignored.
bool ParseManifest(std::string_view json, const std::string& expectVersion, Manifest* out, std::string* err);
// The asset URL must start with `prefix` (kDownloadPrefix, or a file:/// root in tests).
bool AllowedUrl(std::string_view url, std::string_view prefix);
}  // namespace melange::update
