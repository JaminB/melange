#pragma once
// The Spotify host as wum.spotify sees it (wum_spotify.cpp), Melange 0.13.0; 0.14.1 adds the in-game setup (Installed,
// GetSetup, SetClientId, the clipboard, the dashboard), Cancel and Status::app. Main thread only. Implemented by
// spotify.cpp; sandbox_selftest fakes it (tests/sandbox_fakes_spotify.inc), so the Lua binding is tested offline.
// Results and status events reach Lua through the Sink that wum_spotify.cpp installs. No token, and never a whole Client
// ID, crosses this interface towards Lua (the binding returns only what the clipboard held, after a click).
#include <cstdint>
#include <optional>
#include <string>

#include "spotify/spotify_logic.h"
#include "tools/json_read.h"

namespace melange::spotify::host {
// true, or false and "disabled" | "unavailable" | "no-client-id".
bool Available(std::string* reason);
// The module runs: true, or false and "disabled" | "unavailable" (a missing Client ID does not matter here).
bool Installed(std::string* reason);
// `mod` receives melange.spotify status events from now on.
void Touch(const std::string& mod);

struct Status {
    std::string state;  // "disconnected" | "connecting" | "connected" | "error"
    std::string user;   // the display name, when connected
    // When connected: true (Premium) or false (Free) once known, from GET /me or learned from a control call's answer
    // (Development-mode apps no longer get the plan from /me); nullopt while unknown.
    std::optional<bool> premium;
    bool linked = false;  // Melange holds a signed-in account (granted to `mod` or not)
    std::string error;    // state "error": "denied" | "timeout" | "network" | "invalid-client" | "exchange" | "revoked" |
                          // "not-allowlisted"
    std::string app;      // the Client ID in use: "user" | "ini" | "builtin", "" when there is none
};
// The state as `mod` sees it ("connected" only once it is granted). An empty `mod` (the console) sees the global state.
Status GetStatus(const std::string& mod);
bool Granted(const std::string& mod);
// The binding checked availability and the click. true: the sign-in started, the linked account was granted to `mod` (no
// browser), or `mod`'s own open sign-in page was opened again. false and "busy" (another mod's sign-in is open) |
// "port-busy" (built-in app: 8780..8782 all taken) | "browser" | "no-client-id" | "unavailable".
bool Connect(const std::string& mod, std::string* reason);
// Ends an open browser sign-in without signing anyone out, and clears an "error" state. true: a sign-in was open. An
// empty `mod` is the console.
bool Cancel(const std::string& mod);

// ---- the in-game setup (Melange 0.14.1)
struct Setup {
    std::string app;          // "user" | "ini" | "builtin" | ""
    std::string appHint;      // the last 4 characters of the Client ID in use ("" when app is "")
    std::string redirectUri;  // http://127.0.0.1:<RedirectPort>/callback, the URI to register in the user's own app
    bool builtin = false;     // this build has a built-in Client ID
    bool ini = false;         // Melange.ini holds a valid ClientId
};
// Not installed: app "", builtin as compiled, ini false, port 8780.
Setup GetSetup();
// Saves (`trimmedId` valid) or clears ("") the Client ID set in the game. A change of the Client ID in use signs every mod
// out and cancels an open sign-in; an "error" state is cleared either way. false and "invalid" | "io" | "unavailable".
bool SetClientId(const std::string& mod, const std::string& trimmedId, std::string* reason);
// The Spotify Client ID in the clipboard's text (logic::FindClientId), never anything else. false and "clipboard".
bool ClipboardClientId(std::string* id, std::string* reason);
// Puts `utf8` on the clipboard. false and "clipboard".
bool CopyText(const std::string& utf8, std::string* reason);
// Opens logic::kDashboardUrl in the default browser. false and "browser".
bool OpenDashboard(std::string* reason);
// Signs out for every mod; never pauses the music.
void Disconnect(const std::string& mod);
// Queues a Web API request for generation `gen` (its serial) of `mod`. Its id (never 0), or 0 and "auth" | "busy". The
// answer comes through Sink::Callback(mod, token, {..} | {error, message[, retryAfter]}, done = true) when token != 0.
uint32_t Submit(const std::string& mod, uint32_t gen, const logic::Request& r, uint32_t token, std::string* reason);
// Drops every request of generation `gen` of `mod`: queued ones are removed without a callback, a running one's answer
// is dropped. Requests of the mod's other generations (a reload's new one) are left alone.
void CancelGen(const std::string& mod, uint32_t gen);

// Delivery to Lua, installed once by wum_spotify.cpp. Both run on the main thread from the module's frame pump.
class Sink {
public:
    virtual ~Sink() = default;
    // A melange.spotify event for `mod` only ({type = "status", ...}).
    virtual void Event(const std::string& mod, const json::Value& payload) = 0;
    // The function registered with `token`; `done`: it is not called again.
    virtual void Callback(const std::string& mod, uint32_t token, const json::Value& arg, bool done) = 0;
};
void SetSink(Sink* sink);
}  // namespace melange::spotify::host
