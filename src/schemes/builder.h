#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xom/xom.h"

// The schemes and factory-weapon banks: DATA.LockedSchemes (the "Game Style" list) and DATA.LockedWeapons (the team
// editor's custom weapon presets) rebuilt from the game's own LOCAL.XOM with the entries of enabled mods appended.
// Pure functions over a parsed document; the module in schemes.cpp reads the files and talks to the game.
namespace melange::schemes {
constexpr size_t kMaxSchemes = 32, kMaxFactoryWeapons = 64;   // across all enabled mods

// One enabled mod's declared files (spice.json "schemes" or "factoryWeapons"), relative to its folder.
struct Source {
    std::string mod;
    std::filesystem::path dir;
    std::vector<std::string> files;
};
struct Text {
    std::string mod, key, title;   // the FETXT string resource to add for an accepted entry
};
struct Error {
    std::string mod, file, text;
};
struct Built {
    std::vector<uint8_t> bank;     // empty when no entry was accepted (unless emitEmpty) or LOCAL.XOM has no such resource
    int builtIn = 0;               // entries LOCAL.XOM registers
    std::vector<Text> added;
    std::vector<Error> errors;
    std::string fatal;             // why the base could not be used ("" when it could)
};

// `mods` in load order: a key already taken (by LOCAL.XOM or an earlier entry) refuses the later entry.
// With `emitEmpty` the bank is built even when no entry was accepted (LOCAL.XOM's own list again).
Built BuildSchemes(const xom::Document& local, const std::vector<Source>& mods, bool emitEmpty = false);
Built BuildFactoryWeapons(const xom::Document& local, const std::vector<Source>& mods, bool emitEmpty = false);

bool ValidSchemeKey(const std::string& key);    // ^FETXT\.Scheme\.[A-Za-z][A-Za-z0-9]{1,31}$
bool ValidFactoryKey(const std::string& key);   // FETXT.<Name>[.<Name>...], 6..40 characters
}  // namespace melange::schemes
