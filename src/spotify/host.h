#pragma once
// The Spotify host as wum.spotify sees it (wum_spotify.cpp), Melange 0.13.0. Main thread only. Implemented by
// spotify.cpp; sandbox_selftest fakes it (tests/sandbox_fakes_spotify.inc), so the Lua binding is tested offline.
// Results and status events reach Lua through the Sink that wum_spotify.cpp installs. No token ever crosses this
// interface.
#include <cstdint>
#include <optional>
#include <string>

#include "spotify/spotify_logic.h"
#include "tools/json_read.h"

namespace melange::spotify::host {
// true, or false and "disabled" | "unavailable" | "no-client-id".
bool Available(std::string* reason);
// `mod` receives melange.spotify status events from now on.
void Touch(const std::string& mod);

struct Status {
    std::string state;  // "disconnected" | "connecting" | "connected" | "error"
    std::string user;   // the display name, when connected
    // When connected: true (Premium) or false (Free) once known, from GET /me or learned from a control call's answer
    // (Development-mode apps no longer get the plan from /me); nullopt while unknown.
    std::optional<bool> premium;
    bool linked = false;  // Melange holds a signed-in account (granted to `mod` or not)
    std::string error;    // state "error": "denied" | "timeout" | "network" | "invalid-client" | "exchange" | "revoked"
};
// The state as `mod` sees it ("connected" only once it is granted). An empty `mod` (the console) sees the global state.
Status GetStatus(const std::string& mod);
bool Granted(const std::string& mod);
// The binding checked availability and the click. true: the sign-in started, or the linked account was granted to `mod`
// (no browser). false and "busy" | "port-busy" | "browser" | "no-client-id" | "unavailable".
bool Connect(const std::string& mod, std::string* reason);
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
