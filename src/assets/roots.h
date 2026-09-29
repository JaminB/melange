#pragma once
#include <string>
#include <vector>

#include "assets/crcsafe.h"

// A mod's assets/loose/ folder as an engine search path: every file in it must be named "<modId>.*" and none may
// collide with a CRC-listed path, so a mod file can never shadow a vanilla one by bare name.
namespace melange::assets::roots {
// fileNames are the loose directory's direct children (bare names, no path). Pure: callers pass either the live
// CRC table (crcsafe::Entries()) or a synthetic one in a self-test.
bool CheckNames(const std::string& modId, const std::vector<std::string>& fileNames,
                 const std::vector<crcsafe::Entry>& crcTable, std::string* err);

// Lists absLooseDir's direct files, checks them, and on success adds gameRelLooseDir once per launch
// (searchpath::Add). Returns true (no error) if the directory does not exist -- most mods have none. False and
// *err set on any refusal, including an unverifiable CRC table.
bool Add(const std::string& modId, const std::wstring& absLooseDir, const std::string& gameRelLooseDir, std::string* err);

uint32_t Count();  // roots added this launch
}  // namespace melange::assets::roots
