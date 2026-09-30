#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Asset previews for the editor: detail meshes as glTF (.glb), textures and theme materials as PNG, cached by key.
// Pure file conversion over src/xom: no game-memory access, so it can run on a worker thread in melange.asi or in
// oasis.exe once SetGameDir has been called.
namespace melange::erg::preview {
bool Available();  // true: previews are built into this version

// <install>, the folder holding Data\. Call once before any conversion; until it names a folder with a
// Data\Bundles subfolder, DetailKey/ThemeAtlasKey return "" and Get refuses.
void SetGameDir(std::wstring dir);

// The cache key of a detail's preview mesh, "" when the resource has no mesh (the editor then draws a labelled
// box instead). theme is accepted for a future per-theme override.
std::string DetailKey(const std::string& theme, const std::string& resource);

// The cache key of a theme's material-colour atlas; "" if theme is not one of the fixed themes (erg::ValidTheme).
std::string ThemeAtlasKey(const std::string& theme);

// A converted asset's bytes, ready to serve as the body of GET /erg/assets/<key>.<ext>.
struct Asset {
    std::vector<uint8_t> bytes;
    std::string contentType;  // "model/gltf-binary" or "image/png"
    std::string etag;         // quoted, stable for the same bytes
};
// Converts (or serves from the on-disk cache) the asset named by a key from DetailKey/ThemeAtlasKey. ext must
// match the key's own kind ("glb" for a mesh key, "png" for an atlas key). A failure is remembered for the
// process session, so a repeatedly-missing resource is not reconverted on every request.
bool Get(const std::string& key, const std::string& ext, Asset* out, std::string* error = nullptr);

// The JSON index of a theme atlas: {"cell":16,"columns":8,"rows":8,
// "materials":[{"index":0,"name":"BeigeRock/Grass01","resolved":true}, ...]}. "" + *error if theme is invalid.
std::string ThemeAtlasIndex(const std::string& theme, std::string* error = nullptr);

// Deletes least-recently-served cache files until the cache is at or under [Erg] PreviewCacheMB.
void TrimCache();

struct Stats { uint32_t hits = 0, misses = 0, conversions = 0, failed = 0; uint64_t cacheBytes = 0; };
Stats GetStats();
}  // namespace melange::erg::preview
