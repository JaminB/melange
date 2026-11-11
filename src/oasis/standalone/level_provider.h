#pragma once
#include <string>

// level.* for oasis.exe: the same Erg level service as the game's Oasis, over <gameDir>\Data and the packs Thumper's
// state enables. With the game running, projects still work but writes under Mods are -32003.
namespace melange::oasis::standalone::levelprov {
// projectsDir: [Erg] ProjectsDir resolved by the caller (empty = Documents\Melange\erg\projects).
void Install(const std::wstring& gameDir, const std::wstring& projectsDir, const std::string& melangeVersion);
// The game folder the service was installed for ("" before Install, or with [Erg] Enabled=0).
std::wstring GameDir();
// level.build {modId} through the same service (Melange.exe's map-pack auto-build): false with the service's message.
bool Build(const std::string& modId, std::string* replyJson, std::string* err);
}  // namespace melange::oasis::standalone::levelprov
