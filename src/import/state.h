#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// The importer's work folder (Mods\.import\<plugin>\) and its state.json.
namespace melange::import {
struct Paths {
    std::wstring game, mods, plugin;
    std::wstring Work() const { return mods + L"\\.import\\" + plugin; }
    std::wstring Dl() const { return Work() + L"\\dl"; }
    std::wstring Stage() const { return Work() + L"\\stage"; }
    std::wstring Old() const { return Work() + L"\\old"; }
    std::wstring Previews() const { return Work() + L"\\previews"; }
    std::wstring Catalogue() const { return Work() + L"\\maps.json"; }
    std::wstring StateFile() const { return Work() + L"\\state.json"; }
    std::wstring Journal() const { return Work() + L"\\place.json"; }
};
Paths MakePaths(const std::wstring& gameDir, const std::string& plugin);
bool EnsureWork(const Paths& p);

struct PackRecord { std::string id; int levels = 0; std::vector<std::string> categories; };
struct State {
    std::string recipe, recipeVersion, sourceSha256, fingerprint, importedAt;
    int format = 0, maps = 0, skipped = 0;
    std::vector<PackRecord> packs;
    std::map<std::string, int> counts;   // per category id
    uint64_t bytes = 0;
    bool zipKept = false;
    std::vector<std::string> hiddenByFile;
};
bool LoadState(const Paths& p, State* out);
bool SaveState(const Paths& p, const State& s);
std::string StateJson(const State& s);
}  // namespace melange::import
