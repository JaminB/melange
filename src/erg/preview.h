#pragma once
#include <cstdint>
#include <string>

// Asset previews for the editor: detail meshes as glTF, textures and theme materials as PNG, cached by key.
namespace melange::erg::preview {
bool Available();                                          // false while previews are not built into this version
// The cache key of a detail's preview, "" when the resource has none (the editor then draws a labelled box).
std::string DetailKey(const std::string& theme, const std::string& resource);
}  // namespace melange::erg::preview
