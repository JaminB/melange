#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "import/generate.h"
#include "import/place.h"
#include "import/recipe.h"
#include "import/state.h"

// One import run (acquire, verify, read, select, build, place) and the other lifecycle operations, without the UI.
// Runs on a worker thread of Melange.exe with the game closed.
namespace melange::import {
struct Progress {
    std::string phase;   // downloading copying verifying reading building placing
    uint64_t bytes = 0, total = 0;
    int step = 0, of = 0;
};
struct Plugin {
    std::string id, name, melangeRange;
    Recipe recipe;
};
struct RunSpec {
    std::wstring game;
    Plugin plugin;
    enum class Kind { Download, File } kind = Kind::Download;
    std::string sourceId;
    std::wstring file;            // Kind::File: the zip the user picked
    bool keepZip = true;
    std::vector<std::string> urls;   // tests only: replaces the source's URLs (file:/// allowed)
    MoveFn move = store::install::DefaultMove;
    std::function<void(const Progress&)> progress;
    const std::atomic<bool>* cancel = nullptr;
    uint32_t watchdogMs = 10 * 60 * 1000;
    std::string userAgent = "Melange";
};
struct RunResult {
    bool ok = false;
    std::string reason, message;   // reason: network hash space zip recipe vanilla occupied write internal cancelled
    int maps = 0, skipped = 0;
    std::map<std::string, int> counts;
    std::vector<std::string> packs;
    uint64_t bytes = 0;
    std::string fingerprint;
};
RunResult Run(const RunSpec& spec);

// Loads Mods\<plugin>\<recipe> for a plugin whose spice.json declares an importer.
bool LoadPlugin(const std::wstring& game, const std::string& id, Plugin* out, std::string* err, bool* unsupported = nullptr);
std::vector<std::string> ImporterPlugins(const std::wstring& game);   // installed plugins with an importer key

// "none" | "imported" | "stale" | "damaged"
std::string Status(const Paths& p, const Plugin& pl, const State* st, std::string* reason);
// Remove the plugin's generated packs, their hidden stems, the catalogue, previews and state (and the zip).
bool Uninstall(const Paths& p, const std::string& plugin, bool deleteZip, std::vector<std::string>* removed, std::string* err,
               const MoveFn& mv = store::install::DefaultMove);
// Hide or show maps by original file name; writes state.json and hidden-levels.txt. Returns how many are hidden.
int SetHidden(const Paths& p, const std::vector<std::string>& files, bool hidden, std::string* err);
// The cached zip under dl\ and whether it still verifies.
bool CachedZip(const Paths& p, const Source& s, uint64_t* bytes, bool verify);
}  // namespace melange::import
