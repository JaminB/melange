#pragma once
#include <string>
#include <vector>

#include "assets/crcsafe.h"

// LoadBank of a mod's assets/data/<rel> file: rel must stay inside the mod's own data folder, name a real file no
// larger than 64 MiB, not collide with a CRC-listed path, and declare no named resource that already exists live in
// the engine — LoadBank's overwrite flag would otherwise silently replace it.
namespace melange::assets::banks {
// rel is safe if it has no leading slash or drive letter, no "." or ".." segment, and ends in ".xom". Pure aside
// from the size stat, which needs absDataDir; callers pass either the real CRC table or a synthetic one in tests.
bool CheckPath(const std::wstring& absDataDir, const std::string& rel, const std::vector<crcsafe::Entry>& crcTable,
               std::string* err);

// Every "Name" declared by an Int/Uint/String/Float/Vector/Container/StringTable/Color resource entry of the
// file's XDataBank. Pure parsing (melange::xom), no engine dependency, so an offline self-test can call it
// directly on a byte buffer. False (with *err) if the bytes don't parse or hold no XDataBank.
bool BankResourceNames(const std::vector<uint8_t>& bytes, std::vector<std::string>* names, std::string* err);

// CheckPath (against the live CRC table), then BankResourceNames: refused if the bank names any resource that
// already exists live in the engine — such a name would otherwise be silently overwritten by LoadBank's overwrite
// flag. A resource the bank is meant to create (not yet live) always passes this check on its own. Then
// engine::LoadBank(gameRelDataDir + rel, section 0). Returns the engine result (0 = ok) or -1 on refusal or engine
// failure.
int Load(const std::string& modId, const std::wstring& absDataDir, const std::string& gameRelDataDir,
         const std::string& rel, std::string* err);

uint32_t Count();  // successful loads this session
}  // namespace melange::assets::banks
