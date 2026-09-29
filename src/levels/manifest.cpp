#include "levels/manifest.h"

#include <set>

#include "erg/names.h"
#include "erg/scene.h"

namespace melange::levels::manifest {
namespace {
bool SafeRel(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find_first_of("/\\", i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        i = j + 1;
    }
    return true;
}

void Add(std::vector<Error>* errs, const std::string& mod, std::string text) {
    if (errs) errs->push_back({mod, std::move(text)});
}
}  // namespace

std::vector<LevelDecl> Parse(const spice::Manifest& m, std::vector<Error>* errs) {
    std::vector<LevelDecl> out;
    if (m.levels.empty()) return out;
    const size_t before = errs ? errs->size() : 0;
    bool bad = false;
    auto fail = [&](std::string text) {
        Add(errs, m.id, std::move(text));
        bad = true;
    };
    if (!m.content) fail("levels requires kind: \"content\"");
    if (m.levels.size() > kMaxPerMod) fail("at most 32 levels per mod");
    const std::string prefix = erg::names::Prefix(m.id);
    std::string why;
    if (!erg::names::ValidPrefix(prefix, &why)) fail(why);
    std::set<std::string> slugs;
    for (const spice::Level& l : m.levels) {
        if (!erg::names::ValidSlug(l.slug)) {
            fail("level slug '" + l.slug + "' must match [a-z0-9]{1,24}");
            continue;
        }
        if (!slugs.insert(l.slug).second) fail("level slug '" + l.slug + "' is used twice");
        if (!erg::PrintableAscii(l.title, 1, 40)) fail("level '" + l.slug + "': the title must be 1-40 printable ASCII characters");
        if (l.type != "multi") fail("level '" + l.slug + "': type '" + l.type + "' is not supported (multi only)");
        if (!l.source.empty() && !SafeRel(l.source))
            fail("level '" + l.slug + "': source must be a relative path inside the mod folder");
        LevelDecl d;
        d.mod = m.id;
        d.slug = l.slug;
        d.stem = prefix + "_" + l.slug;
        d.title = l.title;
        d.type = l.type;
        d.chunk = l.chunk;
        d.source = l.source;
        if (!erg::names::ValidStem(d.stem, prefix, &why)) fail("level '" + l.slug + "': " + why);
        out.push_back(std::move(d));
    }
    if (bad) {
        out.clear();
        if (errs && errs->size() == before) Add(errs, m.id, "levels refused");
    }
    return out;
}

std::vector<LevelDecl> Assign(const std::vector<std::vector<LevelDecl>>& perModInLoadOrder, std::vector<Error>* refused) {
    std::vector<LevelDecl> out;
    std::set<std::string> prefixes, stems;
    for (const auto& mod : perModInLoadOrder) {
        if (mod.empty()) continue;
        const std::string& id = mod.front().mod;
        const std::string prefix = erg::names::Prefix(id);
        if (prefixes.count(prefix)) {
            Add(refused, id, "another enabled mod already uses the level prefix '" + prefix + "'");
            continue;
        }
        if (out.size() + mod.size() > kMaxTotal) {
            Add(refused, id, "more than 128 levels across all enabled mods");
            continue;
        }
        bool clash = false;
        for (auto& d : mod) clash |= stems.count(d.stem) > 0;
        if (clash) {
            Add(refused, id, "a level stem is already used by another enabled mod");
            continue;
        }
        prefixes.insert(prefix);
        for (auto& d : mod) {
            stems.insert(d.stem);
            out.push_back(d);
        }
    }
    return out;
}

std::vector<std::string> RequiredFiles(const LevelDecl& d) {
    std::vector<std::string> f = {d.stem + ".XOM", "Maps/" + d.stem + ".xan"};
    if (d.chunk) f.push_back(d.stem + ".lub");
    return f;
}

std::vector<std::string> Scripts(const LevelDecl& d) {
    std::vector<std::string> s = {"stdvs", "wormpot"};
    if (d.chunk) s.push_back(d.stem);
    return s;
}
}  // namespace melange::levels::manifest
