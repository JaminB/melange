#pragma once
// The stored Spotify sign-in (Melange 0.13.0): %LOCALAPPDATA%\Melange\spotify\token.bin, UTF-8 JSON encrypted with
// Windows DPAPI for the current user (CryptProtectData). LocalAppData is per user and never roamed or synced, unlike
// Documents. It holds the refresh token, the account's id, name, product and country, and the mods the user granted;
// the access token lives in memory only. Nothing here logs a token. Any thread; spotify.cpp serialises the calls.
#include "spotify/spotify_logic.h"

namespace melange::spotify::tokens {
using Stored = logic::StoredTokens;  // {clientId, refresh, scope, userId, user, product, country, grants}
// False: no file, unreadable, malformed, or DPAPI refused it (another user or machine).
bool Load(Stored* out);
// Atomic: <file>.tmp, then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH).
bool Save(const Stored& s);
// Deletes the file and a stray .tmp.
void Delete();
}  // namespace melange::spotify::tokens
