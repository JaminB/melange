#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The retail CRC table the game's anti-tamper check walks (0x635618, never patched by Melange). C reads it only to
// refuse mod files that would collide with a protected path; the check itself is never bypassed.
namespace melange::assets::crcsafe {
struct Entry {
    std::string path;
    uint32_t crc;
};
// True if fileName (bare, no directory) equals an entry's file name, case-insensitively. Pure, so callers can pass
// either the live table (Entries()) or a synthetic one in a self-test.
bool Collides(const std::vector<Entry>& table, const std::string& fileName);

// The live 89-entry table at 0x922508. Available() is false off the known build, or if the table's first entry
// does not read back as documented; roots.cpp and banks.cpp then refuse every mod path rather than trust a table
// they can't verify.
bool Available();
const std::vector<Entry>& Entries();
}  // namespace melange::assets::crcsafe
