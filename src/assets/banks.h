#pragma once
#include <string>
#include <vector>

#include "assets/crcsafe.h"

// LoadBank of a mod's assets/data/<rel> file (§3.3): rel must stay inside the mod's own data folder, name a real
// file no larger than 64 MiB, and not collide with a CRC-listed path.
namespace melange::assets::banks {
// rel is safe if it has no leading slash or drive letter, no "." or ".." segment, and ends in ".xom". Pure aside
// from the size stat, which needs absDataDir; callers pass either the real CRC table or a synthetic one in tests.
bool CheckPath(const std::wstring& absDataDir, const std::string& rel, const std::vector<crcsafe::Entry>& crcTable,
               std::string* err);

// CheckPath (against the live CRC table), then engine::LoadBank(gameRelDataDir + rel, section 0). Returns the
// engine result (0 = ok) or -1 on refusal or engine failure.
int Load(const std::string& modId, const std::wstring& absDataDir, const std::string& gameRelDataDir,
         const std::string& rel, std::string* err);

uint32_t Count();  // successful loads this session
}  // namespace melange::assets::banks
