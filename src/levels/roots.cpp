#include "levels/roots.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <system_error>

#include "erg/names.h"

namespace melange::levels::roots {
namespace fs = std::filesystem;
namespace {
char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string LowerStr(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = Lower(c);
    return o;
}

bool StartsWithI(std::string_view s, std::string_view p) {
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (Lower(s[i]) != Lower(p[i])) return false;
    return true;
}

bool EqualsI(std::string_view a, std::string_view b) { return a.size() == b.size() && StartsWithI(a, b); }

bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool Ascii(const std::wstring& w, std::string* out) {
    out->clear();
    for (wchar_t c : w) {
        if (c < 0x20 || c >= 0x7f) return false;
        out->push_back(static_cast<char>(c));
    }
    return true;
}

void Walk(const fs::path& d, const std::string& relPrefix, int depth, Listing& l) {
    std::error_code ec;
    for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name;
        if (!Ascii(it->path().filename().wstring(), &name)) name = "?";
        const std::string rel = relPrefix + name;
        std::error_code e2;
        const auto st = it->symlink_status(e2);
        if (e2 || fs::is_symlink(st)) {
            l.other.push_back(rel);
        } else if (fs::is_directory(st)) {
            l.dirs.push_back(rel);
            if (depth > 0) Walk(it->path(), rel + "/", depth - 1, l);
        } else if (fs::is_regular_file(st)) {
            l.files.push_back(rel);
        } else {
            l.other.push_back(rel);
        }
    }
}

bool CheckFile(std::string_view prefix, const std::string& rel, const std::vector<assets::crcsafe::Entry>& crc,
               std::string* err) {
    const size_t slash = rel.find('/');
    std::string name = rel;
    if (slash != std::string::npos) {
        if (!EqualsI(rel.substr(0, slash), "Maps") || rel.find('/', slash + 1) != std::string::npos)
            return Fail(err, "levels/" + rel + " is in a folder other than Maps");
        name = rel.substr(slash + 1);
    }
    const std::string want = std::string(prefix) + "_";
    if (!StartsWithI(name, want))
        return Fail(err, "levels/" + rel + " is not named '" + (slash == std::string::npos ? "" : "Maps/") + want + "*'");
    const size_t dot = name.rfind('.');
    const std::string stem = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : name.substr(dot + 1);
    if (EqualsI(ext, "csh")) return Fail(err, "levels/" + rel + " is a shadow cache (.csh), which a pack may not ship");
    if (stem.find('.') != std::string::npos) return Fail(err, "levels/" + rel + " has a '.' in its stem");
    if (stem.size() > erg::names::kMaxStem) return Fail(err, "levels/" + rel + " has a stem longer than 48 characters");
    if (assets::crcsafe::Collides(crc, name)) return Fail(err, "levels/" + rel + " collides with a protected file");
    if (erg::names::CollidesWithVanilla(stem)) return Fail(err, "levels/" + rel + " uses a vanilla level name");
    return true;
}
}  // namespace

Listing ListLevelRoot(const fs::path& dir) {
    Listing l;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return l;
    l.exists = true;
    Walk(dir, "", 1, l);
    std::sort(l.files.begin(), l.files.end());
    std::sort(l.dirs.begin(), l.dirs.end());
    std::sort(l.other.begin(), l.other.end());
    return l;
}

bool CheckLevelRoot(std::string_view prefix, const Listing& l, const std::vector<assets::crcsafe::Entry>& crcTable,
                    std::string* err) {
    if (!l.other.empty()) return Fail(err, "levels/" + l.other.front() + " is not a plain file or folder");
    for (const auto& d : l.dirs)
        if (!EqualsI(d, "Maps")) return Fail(err, "levels/" + d + " is a folder other than Maps");
    for (const auto& f : l.files)
        if (!CheckFile(prefix, f, crcTable, err)) return false;
    return true;
}

bool CheckBuilt(const std::vector<manifest::LevelDecl>& decls, const Listing& l, std::string* err) {
    std::set<std::string> have;
    for (const auto& f : l.files) have.insert(LowerStr(f));
    for (const auto& d : decls)
        for (const auto& f : manifest::RequiredFiles(d))
            if (!have.count(LowerStr(f))) return Fail(err, std::string(manifest::kNotBuilt) + " (levels/" + f + " is missing)");
    return true;
}

std::vector<PackVerdict> CheckPacks(const std::vector<PackInput>& inLoadOrder,
                                    const std::vector<assets::crcsafe::Entry>& crcTable, bool crcAvailable,
                                    const Lister& list) {
    std::vector<PackVerdict> out;
    std::vector<std::vector<manifest::LevelDecl>> accepted;
    for (const PackInput& in : inLoadOrder) {
        if (!in.manifest || in.manifest->levels.empty()) continue;
        const spice::Manifest& m = *in.manifest;
        PackVerdict v;
        v.mod = m.id;
        std::vector<manifest::Error> errs;
        v.levels = manifest::Parse(m, &errs);
        if (!errs.empty() || v.levels.empty()) {
            v.ok = false;
            for (const auto& e : errs) v.reason += (v.reason.empty() ? "" : "; ") + e.text;
            if (v.reason.empty()) v.reason = "levels refused";
        } else if (!crcAvailable) {
            v.ok = false;
            v.reason = "the CRC table could not be verified; map packs are refused";
        } else {
            const Listing l = list(in.dir / fs::path(m.assetsRoot) / kLevelDir);
            const std::string prefix = erg::names::Prefix(m.id);
            std::string why;
            if (!l.exists) {
                v.ok = false;
                v.reason = std::string(manifest::kNotBuilt) + " (" + m.assetsRoot + "/levels is missing)";
            } else if (!CheckLevelRoot(prefix, l, crcTable, &why) || !CheckBuilt(v.levels, l, &why)) {
                v.ok = false;
                v.reason = why;
            }
        }
        if (!v.ok) v.levels.clear();
        accepted.push_back(v.levels);
        out.push_back(std::move(v));
    }
    std::vector<manifest::Error> refused;
    const auto assigned = manifest::Assign(accepted, &refused);
    for (const auto& r : refused)
        for (auto& v : out)
            if (v.mod == r.mod && v.ok) {
                v.ok = false;
                v.reason = r.text;
                v.levels.clear();
            }
    return out;
}

std::vector<std::string> AddOrder(const std::vector<std::string>& packRoots, bool testRoot, bool cacheRoot) {
    std::vector<std::string> o = packRoots;
    if (testRoot) o.push_back(kTestRel);
    if (cacheRoot) o.push_back(kCacheRel);
    return o;
}

std::string GameRelative(const fs::path& gameDir, const fs::path& dir) {
    std::error_code ec;
    const fs::path rel = fs::relative(fs::weakly_canonical(dir, ec), fs::weakly_canonical(gameDir, ec), ec);
    if (ec || rel.empty()) return "";
    std::string out;
    for (const auto& part : rel) {
        std::string s;
        if (!Ascii(part.wstring(), &s) || s.empty() || s == "." || s == ".." || s.find_first_of(".:") != std::string::npos)
            return "";
        out += (out.empty() ? "" : "/") + s;
    }
    return out;
}

bool IsShadowOf(std::string_view fileName, std::string_view stem) {
    if (!StartsWithI(fileName, stem)) return false;
    std::string_view rest = fileName.substr(stem.size());
    if (rest.size() < 4 || !EqualsI(rest.substr(rest.size() - 4), ".csh")) return false;
    rest.remove_suffix(4);
    for (const char* tod : kTodSuffixes)
        if (EqualsI(rest, tod)) return true;
    return false;
}
}  // namespace melange::levels::roots
