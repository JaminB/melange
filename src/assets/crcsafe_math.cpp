// Collides: pure name matching, kept apart from crcsafe.cpp (which reads the game's live CRC table) so an offline
// self-test can link this file alone, against a synthetic table.
#include "assets/crcsafe.h"

#include <cctype>

namespace melange::assets::crcsafe {
namespace {
std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool EqualsI(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
}  // namespace

bool Collides(const std::vector<Entry>& table, const std::string& fileName) {
    const std::string want = BaseName(fileName);
    for (auto& e : table)
        if (EqualsI(BaseName(e.path), want)) return true;
    return false;
}
}  // namespace melange::assets::crcsafe
