#pragma once
#include <string>

#include "mods/spice.h"

// The build stamp of a map pack: Mods\<id>\.ergbuild.json records the inputs its built levels were made from (the
// sha256 of the Melange version, the game exe's size, spice.json, every level's source patch and every mesh bank), so
// Melange.exe can tell a built pack from one that is unbuilt or stale and build it before the game starts. It sits in
// the mod folder's root, outside the assets root and sim\, so it never changes the pack's multiplayer content hash
// (mods/handshake.cpp); a Store update replaces the whole folder, and with it the stamp.
namespace melange::erg::stamp {
constexpr const wchar_t* kFile = L".ergbuild.json";

struct Record {
    bool ok = false;
    std::string inputs, melange, error, at;   // at: local "YYYY-MM-DD HH:MM:SS"
};

enum class State {
    None,      // no level of this mod is built from a source patch: nothing to build
    Built,     // the stamp's inputs are today's and every level file is there
    Missing,   // never built here (no stamp) or a level file is missing
    Stale,     // built from other inputs (the pack, Melange or the game changed)
    Failed,    // the last build from today's inputs failed (Record::error)
};
const char* Name(State s);   // "none", "built", "missing", "stale", "failed"

// True when some level declares a source patch.
bool HasSources(const spice::Manifest& m);
// The inputs hash; false (with *err) when a declared file cannot be read.
bool Inputs(const std::wstring& modDir, const spice::Manifest& m, const std::string& melangeVersion, const std::wstring& gameDir,
            std::string* hash, std::string* err);
bool Read(const std::wstring& modDir, Record* out);
bool Write(const std::wstring& modDir, const Record& r);
std::string Now();

// The pack's state; *inputs gets today's inputs hash (for the stamp a build writes), *why the reason it needs a build
// or the failure. An unreadable source counts as Failed with that reason.
State Check(const std::wstring& modDir, const spice::Manifest& m, const std::string& melangeVersion, const std::wstring& gameDir,
            std::string* inputs, std::string* why);
}  // namespace melange::erg::stamp
