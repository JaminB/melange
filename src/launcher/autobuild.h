#pragma once
#include <functional>
#include <string>

// Melange.exe builds map packs by itself: every enabled plugin whose levels ship as Erg source (spice.json "levels"
// entries with a "source" patch) and are unbuilt or stale (erg/stamp.h) is built through the same level.build the Erg
// Build button calls, with the game closed: when Melange.exe starts, after a Store install or update, after a plugin
// is switched on, and before the game is launched. [Erg] AutoBuildMaps=0 turns it off (the Build button still works).
// Progress, results and failures go to the "maps" channel and maps.status; a failed pack stays not built with its
// error and never blocks the launch.
namespace melange::launcher::autobuild {
void Install();                  // maps.status and the "maps" channel; a check is due at start
void Request(const char* why);   // any thread: check (and build what needs it) on the next Tick
void Tick();                     // the launcher loop (main thread): starts a due check, runs a launch that waited
bool Busy();                     // any thread: a build batch is running
// launcher.launch (main thread): true when map packs need building first. The game then starts (`start`, on the main
// thread) once the builds end, whatever their outcome, or after a short wait when they cannot start.
bool DeferLaunch(std::function<void()> start);
// The deferred start's outcome, for the page (published on the "maps" channel as {launch: {ok, error}}).
void LaunchDone(bool ok, const std::string& error);
std::string StatusJson();        // any thread
}  // namespace melange::launcher::autobuild
