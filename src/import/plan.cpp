#include "import/plan.h"

#include <algorithm>
#include <cctype>

#include "erg/names.h"
#include "tools/hash.h"

namespace melange::import {
namespace {
std::string Lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool Contains(const std::vector<std::string>& v, const std::string& s) {
    const std::string l = Lower(s);
    for (const auto& x : v)
        if (Lower(x) == l) return true;
    return false;
}
}  // namespace

bool SelectMaps(const Recipe& r, const Found& f, Selection* out, std::string* err) {
    *out = Selection{};
    const Select& s = r.select;
    const bool needDesc = s.require.empty() || Contains(s.require, "descriptor");
    const bool needXan = s.require.empty() || Contains(s.require, "xan");
    // Candidates in the recipe's level type order, then key order: the first one names a file.
    std::vector<const RegistryEntry*> order;
    for (int t : s.levelTypes)
        for (const RegistryEntry& e : f.registry)
            if (e.levelType == t && !e.fileName.empty()) order.push_back(&e);
    std::set<std::string> seen;
    for (const RegistryEntry* ep : order) {
        const RegistryEntry& e = *ep;
        if (!s.skipKeySuffix.empty() && e.key.ends_with(s.skipKeySuffix)) continue;
        const std::string file = Lower(e.fileName);
        if (!seen.insert(file).second) continue;
        bool vanilla = false;
        for (const auto& v : s.vanilla) vanilla |= Lower(v.file) == file;
        if (Contains(s.exclude, e.fileName)) {
            ++out->skipped;
        } else if (vanilla) {
            out->maps.push_back({e, true});
            ++out->fromGame;
        } else if ((!needDesc || f.descriptors.count(file)) && (!needXan || f.xans.count(file))) {
            out->maps.push_back({e, false});
            ++out->fromArchive;
        } else {
            ++out->skipped;
        }
    }
    const int total = out->fromArchive + out->fromGame;
    if (total != s.expectMaps || out->fromArchive != s.expectArchive || out->fromGame != s.expectGame) {
        *err = "found " + std::to_string(total) + " maps (" + std::to_string(out->fromArchive) + " in the zip, " +
               std::to_string(out->fromGame) + " from the game), expected " + std::to_string(s.expectMaps) + " (" +
               std::to_string(s.expectArchive) + ", " + std::to_string(s.expectGame) + ")";
        return false;
    }
    return true;
}

int Categorize(const Recipe& r, const std::vector<std::string>& scripts) {
    int def = 0;
    std::vector<std::string> have;
    for (const auto& t : scripts) have.push_back(Lower(t));
    std::sort(have.begin(), have.end());
    for (size_t i = 0; i < r.categories.size(); ++i) {
        const Category& c = r.categories[i];
        if (c.isDefault) {
            def = static_cast<int>(i);
            continue;
        }
        if (!c.scriptsEqual.empty()) {
            std::vector<std::string> want;
            for (const auto& t : c.scriptsEqual) want.push_back(Lower(t));
            std::sort(want.begin(), want.end());
            if (want == have) return static_cast<int>(i);
        } else if (!scripts.empty()) {
            bool all = true;
            for (const auto& t : scripts) {
                bool any = false;
                for (const auto& p : c.scriptsWithin) any |= GlobMatch(p, t);
                all &= any;
            }
            if (all) return static_cast<int>(i);
        }
    }
    return def;
}

int GroupOf(const Recipe& r, const std::string& fileName, bool fromGame) {
    int def = 0;
    for (size_t i = 0; i < r.groups.size(); ++i) {
        const Group& g = r.groups[i];
        if (g.isDefault) def = static_cast<int>(i);
        else if (g.vanilla && fromGame) return static_cast<int>(i);
        else if (!fromGame)
            for (const auto& p : g.match)
                if (GlobMatch(p, fileName)) return static_cast<int>(i);
    }
    return def;
}

std::string ModeOf(const Recipe& r, const std::vector<std::string>& scripts) {
    for (const auto& t : scripts)
        for (const auto& [p, label] : r.modes)
            if (GlobMatch(p, t)) return label;
    return {};
}

std::string Slug(const Recipe& r, const std::string& fileName) {
    std::string s;
    for (char c : fileName) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9')) s.push_back(l);
    }
    if (!s.empty() && s.size() <= static_cast<size_t>(r.transform.slugMax)) return s;
    const std::string h = hashutil::Sha256Hex(fileName.data(), fileName.size());
    return s.substr(0, static_cast<size_t>(r.transform.hashKeep)) + h.substr(0, static_cast<size_t>(r.transform.hashHex));
}

std::string PackId(const Recipe& r, int n) { return r.output.packPrefix + "-" + std::to_string(n); }

bool PlanPacks(const Recipe& r, const Selection& s, std::vector<Planned>* out, std::string* err) {
    out->clear();
    for (const Selected& m : s.maps) {
        Planned p;
        p.sel = m;
        p.category = Categorize(r, m.entry.scripts);
        p.group = GroupOf(r, m.entry.fileName, m.fromGame);
        p.mode = ModeOf(r, m.entry.scripts);
        out->push_back(std::move(p));
    }
    std::sort(out->begin(), out->end(), [](const Planned& a, const Planned& b) {
        if (a.category != b.category) return a.category < b.category;
        const std::string la = Lower(a.sel.entry.fileName), lb = Lower(b.sel.entry.fileName);
        if (la != lb) return la < lb;
        return a.sel.entry.fileName < b.sel.entry.fileName;
    });
    int pack = 0, inPack = 0, lastCat = -1;
    std::set<std::string> stems;
    for (Planned& p : *out) {
        const bool brk = p.category != lastCat && lastCat >= 0 &&
                         Contains(r.output.newPackBefore, r.categories[static_cast<size_t>(p.category)].id);
        if (pack == 0 || inPack == r.output.perPack || (brk && inPack > 0)) {
            ++pack;
            inPack = 0;
        }
        lastCat = p.category;
        ++inPack;
        if (pack > static_cast<int>(kMaxPacks)) {
            *err = "the maps need more than 9 packs";
            return false;
        }
        p.pack = pack;
        p.packId = PackId(r, pack);
        p.slug = Slug(r, p.sel.entry.fileName);
        const std::string prefix = erg::names::Prefix(p.packId);
        p.stem = prefix + "_" + p.slug;
        std::string why;
        if (!erg::names::ValidStem(p.stem, prefix, &why)) {
            *err = p.sel.entry.fileName + ": " + why;
            return false;
        }
        if (!stems.insert(p.stem).second) {
            *err = p.sel.entry.fileName + ": the stem " + p.stem + " is used twice";
            return false;
        }
    }
    return true;
}
}  // namespace melange::import
