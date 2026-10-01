#pragma once
#include <string>
#include <string_view>
#include <vector>

#include "mods/spice.h"

// The spice.json "levels" array: slugs, stems, titles and limits, and the per-launch set across mods (load order).
// Pure functions.
namespace melange::levels::manifest {
constexpr size_t kMaxPerMod = 32, kMaxTotal = 128, kMaxSimBytes = 256 * 1024;
// A level may also be registered as a Survivor map (Multi.<stem>.S, the "Survivor copy").
constexpr bool kSurvivorTwins = true;
// The Survivor script runs the level's own chunk (the deferred form) when it is appended to the script list.
constexpr bool kSurvivorRunsChunk = true;

struct LevelDecl {
    std::string mod, slug, stem, title, type;
    bool chunk = false, survivor = false;
    std::string source, sim;                          // sim: "sim/<name>.lua", the level's sandboxed script
};
struct Error {
    std::string mod, text;
};

std::vector<LevelDecl> Parse(const spice::Manifest& m, std::vector<Error>* errs);   // empty when refused
// Mods in load order, each with its own Parse() result. A mod whose prefix another enabled mod already uses, or whose
// levels would pass the total limit, is refused as a whole (its reason in *refused).
std::vector<LevelDecl> Assign(const std::vector<std::vector<LevelDecl>>& perModInLoadOrder, std::vector<Error>* refused);

// The built files a level needs under assets/levels ("<stem>.XOM", "Maps/<stem>.xan", and "<stem>.lub" for a chunk).
std::vector<std::string> RequiredFiles(const LevelDecl& d);
std::vector<std::string> Scripts(const LevelDecl& d);   // stdvs, wormpot (+ the stem for a chunk)
std::string TwinKey(const std::string& stem);           // "Multi.<stem>.S"
std::vector<std::string> SurvivorScripts(const LevelDecl& d);   // Survivor (+ the stem when the chunk runs there)
bool ValidSimPath(const std::string& p);                // relative, under sim/, .lua, no "..", at most 200 characters
bool CheckSimText(std::string_view bytes, std::string* why);   // <= 256 KB, UTF-8 without a BOM or ESC
constexpr const char* kNotBuilt = "not built: open Erg and press Build, or run build.ps1";
}  // namespace melange::levels::manifest
