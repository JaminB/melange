#pragma once
#include <string>
#include <vector>

// Engine search paths added at runtime (XomGetApp()->vtbl+0xC). Each directory is added once per launch; there is no
// removal.
namespace melange::assets::searchpath {
bool Add(const char* gameRelDir);  // true if added now or earlier
std::vector<std::string> Added();
}  // namespace melange::assets::searchpath
