#pragma once
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "melange/mods.h"

// spice.json parsing and mod resolution as pure functions (offline self-test).
namespace melange::spice {
struct Dep { std::string id, range; };
struct Setting { std::string key, type, label, def; double min = 0, max = 0; std::vector<std::string> options; };
struct Manifest {
    std::string id, version, name, description, website, melangeRange; std::vector<std::string> authors;
    bool content = false, unsafe = false, defaultEnabled = true, implicit = false;
    std::string filesystem = "none", entryClient, entrySim, assetsRoot = "assets", shaders = "shaders", effects = "effects";
    std::vector<Dep> dependencies, optional, conflicts; std::vector<std::string> loadAfter, messages, hashInclude;
    std::vector<Setting> settings; std::wstring dir;
};
struct Error { std::string field; int line = 0, col = 0; std::string text; };
bool Parse(const std::wstring& dir, Manifest* out, std::vector<Error>* errs);  // synthesises the implicit manifest
struct Resolved { std::string id; mods::State state; std::string reason; int order; };
std::vector<Resolved> Resolve(const std::vector<Manifest>& all, const std::set<std::string>& userEnabled,
                              const std::string& melangeVersion, const std::vector<std::pair<std::string,std::string>>& pins);
bool SemverSatisfies(const std::string& version, const std::string& range);
}
