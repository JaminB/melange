#pragma once
// Melange.ini [Spotify] (Melange 0.13.0; 0.14.1: ClientId is an optional override, ReturnToGame), read by the Spotify
// module (spotify.cpp). The first read writes each key's default, so a fresh ini documents them. Enabled belongs to the
// module (modules::InstallAll); nothing here writes it. The Client ID set in the game (client_store.h) wins over
// ClientId, which wins over the release's built-in app. Main thread.
#include <string>

namespace melange::spotify::cfg {
struct Spotify {
    std::string clientId;        // ClientId: an override (32 alphanumerics) when none was set in the game; "" = the built-in app
    int redirectPort = 8780;     // RedirectPort (1024..65535): http://127.0.0.1:<port>/callback (the user's own app; the
                                 // built-in app uses 8780, 8781 or 8782)
    int requestsPerMinute = 90;  // RequestsPerMinute (10..600): Web API calls of all mods together
    bool returnToGame = true;    // ReturnToGame: bring the game to the front after a sign-in (0: only flash its taskbar button)
};
// ClientId is live (re-read at every connect() and at most every 2 s by Available()); a malformed one logs a warning
// once and reads as empty (the next source is used).
Spotify ReadSpotify();
}  // namespace melange::spotify::cfg
