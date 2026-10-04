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
        if (l.type != "multi") fail("level '" + l.slug + "': type '" + l.type + "' is not supported in this version");
        if (l.survivor && !kSurvivorTwins) fail("level '" + l.slug + "': survivor maps are not supported in this version");
        if (!l.sim.empty() && !ValidSimPath(l.sim))
            fail("level '" + l.slug + "': sim must be a relative path under sim/ ending in .lua");
        if (!l.source.empty() && !SafeRel(l.source))
            fail("level '" + l.slug + "': source must be a relative path inside the mod folder");
        LevelDecl d;
        d.mod = m.id;
        d.slug = l.slug;
        d.stem = prefix + "_" + l.slug;
        d.title = l.title;
        d.type = l.type;
        d.chunk = l.chunk;
        d.survivor = l.survivor;
        d.source = l.source;
        d.sim = l.sim;
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
            Add(refused, id, "more than 256 levels across all enabled mods");
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

std::string TwinKey(const std::string& stem) { return "Multi." + stem + ".S"; }

std::vector<std::string> SurvivorScripts(const LevelDecl& d) {
    std::vector<std::string> s = {"Survivor"};
    if (d.chunk && kSurvivorRunsChunk) s.push_back(d.stem);
    return s;
}

bool ValidSimPath(const std::string& p) {
    if (!SafeRel(p) || p.find('\\') != std::string::npos || p.size() < 9 || p.compare(0, 4, "sim/") != 0) return false;
    if (p.compare(p.size() - 4, 4, ".lua") != 0) return false;
    for (unsigned char c : p)
        if (c < 0x21 || c > 0x7e) return false;
    return true;
}

bool CheckSimText(std::string_view b, std::string* why) {
    auto fail = [&](const char* w) {
        if (why) *why = w;
        return false;
    };
    if (b.size() > kMaxSimBytes) return fail("the level script is larger than 256 KB");
    if (b.size() >= 3 && static_cast<unsigned char>(b[0]) == 0xef && static_cast<unsigned char>(b[1]) == 0xbb &&
        static_cast<unsigned char>(b[2]) == 0xbf)
        return fail("the level script starts with a byte order mark");
    size_t i = 0;
    while (i < b.size()) {
        const unsigned char c = static_cast<unsigned char>(b[i]);
        if (c == 0x1b) return fail("the level script holds an ESC byte (compiled Lua is refused)");
        if (c == 0) return fail("the level script holds a NUL byte");
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > b.size() || (n == 2 && c < 0xc2)) return fail("the level script is not valid UTF-8");
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(b[i + k]) & 0xc0) != 0x80) return fail("the level script is not valid UTF-8");
        i += n;
    }
    return true;
}
}  // namespace melange::levels::manifest
