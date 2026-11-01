#pragma once
// The Spotify Client ID set in the game (Melange 0.14.0): %LOCALAPPDATA%\Melange\spotify\client.json, beside token.bin,
// plain JSON {"v":1,"clientId":"..."} (spotify_logic.h EncodeClientFile). A Client ID is not a secret (it is in every
// authorize URL), so it is not encrypted. It wins over [Spotify] ClientId and the built-in app (logic::ResolveClientId).
// Main thread only; never from DllMain (the shell's known-folder lookup is not loader-lock safe).
#include <string>

namespace melange::spotify::clientfile {
// False: no file, unreadable or invalid (an invalid file logs one warning and counts as none).
bool Load(std::string* id);
// Atomic: client.json.tmp, then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH). `id` must be valid.
bool Save(const std::string& id);
// Deletes the file (and a stray .tmp). True when it is gone.
bool Delete();
}  // namespace melange::spotify::clientfile
