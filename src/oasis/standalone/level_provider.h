#pragma once
#include <string>

// level.* for oasis.exe: the same Erg level service as the game's Oasis, over <gameDir>\Data and the packs Thumper's
// state enables. With the game running, projects still work but writes under Mods are -32003.
namespace melange::oasis::standalone::levelprov {
// projectsDir: [Erg] ProjectsDir resolved by the caller (empty = Documents\Melange\erg\projects).
void Install(const std::wstring& gameDir, const std::wstring& projectsDir, const std::string& melangeVersion);
}  // namespace melange::oasis::standalone::levelprov
