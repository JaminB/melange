// CheckNames: the "<modId>.*" naming rule plus the CRC-collision check. Pure (given a file-name list and a CRC
// table), kept apart from roots.cpp (which lists the real directory and touches the engine search path) so an
// offline self-test can link this file alone.
#include "assets/roots.h"

#include <cctype>

namespace melange::assets::roots {
namespace {
bool StartsWithI(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}
}  // namespace

bool CheckNames(const std::string& modId, const std::vector<std::string>& fileNames,
                 const std::vector<crcsafe::Entry>& crcTable, std::string* err) {
    const std::string prefix = modId + ".";
    for (const auto& f : fileNames) {
        if (!StartsWithI(f, prefix)) {
            if (err) *err = "loose/" + f + " is not named '" + modId + ".*'";
            return false;
        }
    }
    for (const auto& f : fileNames) {
        if (crcsafe::Collides(crcTable, f)) {
            if (err) *err = "loose/" + f + " collides with a protected file";
            return false;
        }
    }
    return true;
}
}  // namespace melange::assets::roots
