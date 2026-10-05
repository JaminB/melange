#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "launcher/setup/engine.h"

// Restore vanilla: make the game folder stock Worms Ultimate Mayhem again. Every file that isn't part of the game
// (Melange, other mod frameworks, loaders, plugins, leftovers) is deleted for good -- no backup -- except a short
// keep list of files the game itself writes. Replays are moved to Documents\Melange\replays first. Stock files that
// were overwritten are reported so the caller can have Steam verify the game. Plan first, then apply; the plan id
// guards against the folder changing in between. UI-free; the self-test drives it on a fake folder.
//
// The stock list (res/wum-1077-stock.tsv, embedded in Melange.exe as WUM_STOCK) is "relative\path<TAB>size" for every
// file of a clean Steam install of build #1077 (size 0 = not checked). It was taken from a freshly Steam-verified
// install, *.csh shader caches left out; scripts/gen-stock-manifest.ps1 regenerates it.
namespace melange::launcher::setup {
struct StockList {
    std::unordered_map<std::wstring, uint64_t> files;   // lower-case relative path -> size (0 = unknown)
    std::unordered_set<std::wstring> dirs;              // lower-case relative folders that hold stock files
};
bool ParseStockList(std::string_view tsv, StockList* out, std::string* err);
// The list embedded in this exe (WUM_STOCK resource); empty when the resource is missing.
const StockList& EmbeddedStockList();

struct VanillaContext {
    Context base;                       // gameDir, profiles, protect, running, loaded, storeOf, selfExe, progress
    const StockList* stock = nullptr;   // default: EmbeddedStockList()
    std::wstring replaysDir;            // where replays go (default: Documents\Melange\replays)
    std::function<unsigned long(const std::wstring& path)> remove;   // default: DeleteFileW (RemoveDirectoryW for a link)
};

struct VanillaGroup {
    std::string id;      // melange | loader | asi | wumpatch | renewation | mousefix | reshade | specialk | dgvoodoo | other
    std::string label;   // "Renewation HD 0.2A2", "Ultimate ASI Loader 9.7.4", "ASI plugins", ...
    int files = 0;
};
struct VanillaPlan {
    std::string planId;
    std::string refused;   // user copy, "" when the plan can run
    int code = 0;          // with refused: -32000
    std::vector<VanillaGroup> groups;   // what was found, in display order; "other" last
    std::vector<std::string> remove;    // relative paths of every file to delete (replays excluded)
    uint64_t removeBytes = 0;
    std::vector<std::string> replays;   // relative paths moved to replaysDir
    std::wstring replaysDir;
    std::vector<std::string> modified, missing;   // stock files whose size differs / that are gone
    bool overwrites = false;   // a framework known to overwrite stock files (Renewation, WUMPatch) was found
    bool verify = false;       // modified, missing or overwrites: the game files need verifying afterwards
    std::string store;         // steam | gog | unknown
    bool selfInGame = false;   // the running Melange.exe is one of the files to delete
};
VanillaPlan MakeVanillaPlan(const VanillaContext& ctx);
std::string VanillaPlanJson(const VanillaPlan& p);

struct VanillaOutcome {
    Outcome outcome;   // ok, code (-32000 refused, -32010 access denied, -32012 incomplete, -32013 plan changed)
    int deleted = 0, dirsRemoved = 0;
    std::vector<std::pair<std::string, std::string>> moved;   // replay: relative path -> new full path
    std::vector<std::string> failed;                           // relative paths that could not be deleted
    std::vector<std::wstring> pending;   // files in use by this process (its own exe): delete after it exits
    bool verifyStarted = false;          // set by the caller: Steam was asked to verify the game files
    VanillaPlan plan;                    // the plan that ran
};
// Recomputes the plan and refuses when its id differs from `planId` ("" skips the check).
VanillaOutcome ApplyVanilla(const VanillaContext& ctx, const std::string& planId);
std::string VanillaOutcomeJson(const VanillaOutcome& o);
}  // namespace melange::launcher::setup
