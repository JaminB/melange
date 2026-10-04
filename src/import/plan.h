#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "import/recipe.h"
#include "import/w4reader.h"

// Selection, classification, order, packs and stems: pure functions of the recipe and what the reader found.
namespace melange::import {
struct Found {
    std::vector<RegistryEntry> registry;            // sorted by key
    std::set<std::string> descriptors, xans;        // lower-case file names (no extension) present in the zip
};
struct Selected {
    RegistryEntry entry;
    bool fromGame = false;
};
struct Selection {
    std::vector<Selected> maps;
    int fromArchive = 0, fromGame = 0, skipped = 0;
};
// Registry entries of the recipe's level types (key suffix and exclude rules applied) with their files in the zip or
// on the vanilla list. A file named by several entries takes the first in level type order, then key order. Fails
// "recipe" when the counts differ from expect.
bool SelectMaps(const Recipe& r, const Found& f, Selection* out, std::string* err);

struct Planned {
    Selected sel;
    int category = 0, group = 0;   // indices into the recipe lists
    std::string mode;              // mode label, "" for none
    int pack = 0;                  // 1-based
    std::string packId, slug, stem;
};
int Categorize(const Recipe& r, const std::vector<std::string>& scripts);
int GroupOf(const Recipe& r, const std::string& fileName, bool fromGame);
std::string ModeOf(const Recipe& r, const std::vector<std::string>& scripts);
std::string Slug(const Recipe& r, const std::string& fileName);
// Sorted (category, case-folded name, raw name), split into packs with the newPackBefore breaks, named. Fails on a
// stem collision, an invalid stem or more than kMaxPacks packs.
bool PlanPacks(const Recipe& r, const Selection& s, std::vector<Planned>* out, std::string* err);
std::string PackId(const Recipe& r, int n);   // "<prefix>-<n>"
}  // namespace melange::import
