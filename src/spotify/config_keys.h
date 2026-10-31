#pragma once
// Melange.ini [Spotify] (Melange 0.13.0), read by the Spotify module (spotify.cpp). The first read writes each key's
// default, so a fresh ini documents them. Enabled belongs to the module (modules::InstallAll); nothing here writes it.
// Main thread.
#include <string>

namespace melange::spotify::cfg {
struct Spotify {
    std::string clientId;        // ClientId: the user's Spotify app (32 alphanumerics), "" = off
    int redirectPort = 8780;     // RedirectPort (1024..65535): http://127.0.0.1:<port>/callback
    int requestsPerMinute = 90;  // RequestsPerMinute (10..600): Web API calls of all mods together
};
// ClientId is live (re-read at every connect() and at most every 2 s by Available()); a malformed one logs a warning
// once and reads as empty.
Spotify ReadSpotify();
}  // namespace melange::spotify::cfg
