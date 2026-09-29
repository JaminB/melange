#pragma once
#include <string>
#include <vector>

#include "mods/spice.h"

// The spice.json "levels" array: slugs, stems, titles and limits, and the per-launch set across mods (load order).
// Pure functions.
namespace melange::levels::manifest {
constexpr size_t kMaxPerMod = 32, kMaxTotal = 128;

struct LevelDecl {
    std::string mod, slug, stem, title, type;
    bool chunk = false;
    std::string source;
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
constexpr const char* kNotBuilt = "not built: open Erg and press Build, or run build.ps1";
}  // namespace melange::levels::manifest
