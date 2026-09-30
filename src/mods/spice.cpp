// spice.json parsing, semver and load-order resolution: pure functions, no game or filesystem state
// beyond reading one manifest file per Parse() call. Safe to unit-test offline (tests/thumper_selftest.cpp).
#include "mods/spice.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "core/log.h"
#include "tools/json_read.h"

namespace melange::spice {
namespace {

std::string Narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(static_cast<char>(c < 128 ? c : '?'));
    return s;
}

std::string ToLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring FolderName(std::wstring dir) {
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    size_t p = dir.find_last_of(L"\\/");
    return p == std::wstring::npos ? dir : dir.substr(p + 1);
}

bool IsIdChar(unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }

// ^[a-z0-9](?:[a-z0-9_-]{0,62}[a-z0-9])?$
bool ValidId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (!std::isalnum(static_cast<unsigned char>(s.front())) || !std::isalnum(static_cast<unsigned char>(s.back())))
        return false;
    for (unsigned char c : s)
        if (!IsIdChar(c) || (std::isalpha(c) && !std::islower(c))) return false;
    return true;
}

// A manifest's own id must already satisfy ValidId, but a folder with no manifest takes its folder name as
// its id unchanged, which may hold a '.' (or other characters ValidId rejects). Oasis channel and method
// names allow '.' in the part after "mod.<id>.", so a dotted id would make that prefix ambiguous: mod "foo"
// registering "mod.foo.bar.x" would be indistinguishable from mod "foo.bar" registering "mod.foo.bar.x" for
// itself, letting one mod's panel reach another's channels. Folding every character ValidId disallows to '-'
// keeps ids collision-free without breaking existing folders (real Mods\ names are already alnum/-/_).
std::string SanitizeId(std::string s) {
    for (char& c : s)
        if (!IsIdChar(static_cast<unsigned char>(c))) c = '-';
    while (!s.empty() && (s.front() == '-' || s.front() == '_')) s.erase(s.begin());
    while (!s.empty() && (s.back() == '-' || s.back() == '_')) s.pop_back();
    if (s.size() > 64) s.resize(64);
    return s.empty() ? "mod" : s;
}

// ---------------------------------------------------------------------------------------------
// Semantic versions and the npm/Cargo-style comparator grammar (^, ~, >=, <=, >, <, =, space = AND).
// ---------------------------------------------------------------------------------------------
struct SemVer {
    long long major = 0, minor = 0, patch = 0;
    std::vector<std::string> pre;  // dot-separated identifiers; empty = no prerelease
    bool ok = false;
};

bool ParseUInt(std::string_view s, long long* out) {
    if (s.empty() || s.size() > 18) return false;
    if (s.size() > 1 && s[0] == '0') return false;  // no leading zeros in a numeric field
    long long v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

// Parses "MAJOR.MINOR.PATCH[-prerelease][+build]"; build metadata is read but not stored (ignored
// for comparison per semver.org).
SemVer ParseSemVer(std::string_view s) {
    SemVer v;
    size_t plus = s.find('+');
    std::string_view core = plus == std::string_view::npos ? s : s.substr(0, plus);
    size_t dash = core.find('-');
    std::string_view nums = dash == std::string_view::npos ? core : core.substr(0, dash);
    size_t d1 = nums.find('.');
    if (d1 == std::string_view::npos) return v;
    size_t d2 = nums.find('.', d1 + 1);
    if (d2 == std::string_view::npos) return v;
    if (!ParseUInt(nums.substr(0, d1), &v.major)) return v;
    if (!ParseUInt(nums.substr(d1 + 1, d2 - d1 - 1), &v.minor)) return v;
    if (!ParseUInt(nums.substr(d2 + 1), &v.patch)) return v;
    if (dash != std::string_view::npos) {
        std::string_view pre = core.substr(dash + 1);
        if (pre.empty()) return v;
        size_t p = 0;
        while (p <= pre.size()) {
            size_t q = pre.find('.', p);
            if (q == std::string_view::npos) q = pre.size();
            std::string_view id = pre.substr(p, q - p);
            if (id.empty()) return v;
            for (char c : id)
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') return v;
            v.pre.emplace_back(id);
            p = q + 1;
        }
    }
    v.ok = true;
    return v;
}

bool IsNumericId(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

// -1, 0, 1. Per semver.org §11: no prerelease outranks any prerelease; otherwise identifiers compare
// left to right, numeric identifiers numerically, a numeric identifier always lower than alphanumeric.
int ComparePre(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return 1;   // a has no prerelease: a > b
    if (b.empty()) return -1;
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const std::string &x = a[i], &y = b[i];
        bool nx = IsNumericId(x), ny = IsNumericId(y);
        if (nx && ny) {
            long long vx = 0, vy = 0;
            ParseUInt(x, &vx);
            ParseUInt(y, &vy);
            if (vx != vy) return vx < vy ? -1 : 1;
        } else if (nx != ny) {
            return nx ? -1 : 1;
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

int Compare(const SemVer& a, const SemVer& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return ComparePre(a.pre, b.pre);
}

struct Comparator {
    char op = '=';  // one of = > < ^ ~ with G/L meaning >= / <=
    SemVer ver;
};

// "^1.2.0" -> [">=1.2.0", "<2.0.0"], "~1.2.0" -> [">=1.2.0", "<1.3.0"], expanded into plain comparators.
bool ExpandComparator(char op, const SemVer& v, std::vector<Comparator>* out) {
    if (op == '^') {
        out->push_back({'G', v});
        SemVer hi = v;
        hi.pre.clear();
        if (v.major > 0) {
            hi.major += 1;
            hi.minor = 0;
            hi.patch = 0;
        } else if (v.minor > 0) {
            hi.minor += 1;
            hi.patch = 0;
        } else {
            hi.patch += 1;
        }
        out->push_back({'<', hi});
        return true;
    }
    if (op == '~') {
        out->push_back({'G', v});
        SemVer hi = v;
        hi.pre.clear();
        hi.minor += 1;
        hi.patch = 0;
        out->push_back({'<', hi});
        return true;
    }
    out->push_back({op, v});
    return true;
}

bool ParseRange(std::string_view range, std::vector<Comparator>* out) {
    size_t p = 0;
    while (p < range.size()) {
        while (p < range.size() && std::isspace(static_cast<unsigned char>(range[p]))) ++p;
        if (p >= range.size()) break;
        size_t q = p;
        while (q < range.size() && !std::isspace(static_cast<unsigned char>(range[q]))) ++q;
        std::string_view tok = range.substr(p, q - p);
        p = q;
        char op = '=';
        size_t skip = 0;
        if (tok.substr(0, 2) == ">=") {
            op = 'G';
            skip = 2;
        } else if (tok.substr(0, 2) == "<=") {
            op = 'L';
            skip = 2;
        } else if (tok[0] == '>') {
            op = '>';
            skip = 1;
        } else if (tok[0] == '<') {
            op = '<';
            skip = 1;
        } else if (tok[0] == '^') {
            op = '^';
            skip = 1;
        } else if (tok[0] == '~') {
            op = '~';
            skip = 1;
        } else if (tok[0] == '=') {
            op = '=';
            skip = 1;
        }
        SemVer v = ParseSemVer(tok.substr(skip));
        if (!v.ok) return false;
        if (!ExpandComparator(op, v, out)) return false;
    }
    return true;
}
}  // namespace

bool SemverSatisfies(const std::string& version, const std::string& range) {
    SemVer v = ParseSemVer(version);
    if (!v.ok) return false;
    std::string r = Trim(range);
    if (r.empty()) return true;  // no comparator: any version
    std::vector<Comparator> cmps;
    if (!ParseRange(r, &cmps)) return false;
    bool rangeHasPre = false;
    for (const Comparator& c : cmps)
        if (!c.ver.pre.empty()) rangeHasPre = true;
    // "Prereleases only match a range that names a prerelease" (npm's own rule): a prerelease version
    // is invisible to a range unless at least one comparator shares its exact major.minor.patch triple
    // and also carries a prerelease tag.
    if (!v.pre.empty() && !rangeHasPre) return false;
    for (const Comparator& c : cmps) {
        int cmp = Compare(v, c.ver);
        bool ok;
        switch (c.op) {
            case 'G': ok = cmp >= 0; break;
            case 'L': ok = cmp <= 0; break;
            case '>': ok = cmp > 0; break;
            case '<': ok = cmp < 0; break;
            default: ok = cmp == 0; break;
        }
        if (!ok) return false;
    }
    return true;
}

namespace {
bool ParseDepString(const std::string& raw, Dep* out) {
    std::string s = Trim(raw);
    size_t p = 0;
    while (p < s.size() && IsIdChar(static_cast<unsigned char>(s[p]))) ++p;
    out->id = s.substr(0, p);
    out->range = Trim(s.substr(p));
    return !out->id.empty();
}

// ---------------------------------------------------------------------------------------------
// spice.json -> Manifest
// ---------------------------------------------------------------------------------------------
void AddError(std::vector<Error>* errs, const json::Value* v, const char* field, const std::string& text) {
    if (!errs) return;
    Error e;
    e.field = field;
    e.line = v ? v->line : 0;
    e.col = v ? v->col : 0;
    e.text = text;
    errs->push_back(std::move(e));
}

bool GetStr(const json::Value& obj, const char* key, std::string* out, const char* def = nullptr) {
    const json::Value* v = obj.Get(key);
    if (!v) {
        if (def) *out = def;
        return false;
    }
    if (!v->IsString()) return false;
    *out = v->string;
    return true;
}

// No leading slash or drive letter, and no "." or ".." segment: a value can never point outside the mod folder it
// is joined to. Mirrors weapons/manifest.cpp's SafeRel (the same check on the same kind of field).
bool SafeRelRoot(const std::string& p) {
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

bool ValidMessageName(const std::string& s) {
    // ^[A-Z][A-Za-z0-9]*(\.[A-Za-z0-9_]+){1,5}$
    size_t p = 0;
    if (p >= s.size() || !std::isupper(static_cast<unsigned char>(s[p]))) return false;
    ++p;
    while (p < s.size() && std::isalnum(static_cast<unsigned char>(s[p]))) ++p;
    int segments = 0;
    while (p < s.size() && s[p] == '.') {
        ++p;
        size_t start = p;
        while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '_')) ++p;
        if (p == start) return false;
        ++segments;
    }
    return p == s.size() && segments >= 1 && segments <= 5;
}

bool ParseWeapons(const json::Value& a, Manifest* out, std::vector<Error>* errs) {
    if (!a.IsArray()) {
        AddError(errs, &a, "weapons", "weapons must be an array");
        return false;
    }
    if (a.items.size() > 3) {
        AddError(errs, &a, "weapons", "at most 3 weapons (the free panel cells)");
        return false;
    }
    bool ok = true;
    for (const json::Value& item : a.items) {
        if (!item.IsObject()) {
            AddError(errs, &item, "weapons", "each weapon must be an object");
            ok = false;
            continue;
        }
        Weapon w;
        w.line = item.line;
        bool good = true;
        auto str = [&](const json::Value& m, const char* key, std::string* into, size_t max) {
            if (!m.IsString() || m.string.empty() || m.string.size() > max) {
                AddError(errs, &m, "weapons", std::string("weapons.") + key + " must be a string of 1-" + std::to_string(max) +
                                                  " characters");
                good = false;
                return;
            }
            *into = m.string;
        };
        for (const auto& [key, m] : item.members) {
            if (key == "name") {
                str(m, "name", &w.name, 48);
            } else if (key == "base") {
                str(m, "base", &w.base, 48);
            } else if (key == "bank") {
                str(m, "bank", &w.bank, 200);
            } else if (key == "panelIcon") {
                str(m, "panelIcon", &w.panelIcon, 200);
            } else if (key == "hudIcon") {
                str(m, "hudIcon", &w.hudIcon, 120);
            } else if (key == "cell") {
                if (!m.IsInteger() || m.number < 0 || m.number > 41) {
                    AddError(errs, &m, "weapons.cell", "weapons.cell must be 29, 39 or 40");
                    good = false;
                } else {
                    w.cell = static_cast<int>(m.number);
                }
            } else if (key == "set") {
                if (!m.IsObject() || m.members.size() > 64) {
                    AddError(errs, &m, "weapons.set", "weapons.set must be an object of at most 64 fields");
                    good = false;
                    continue;
                }
                for (const auto& [fk, fv] : m.members) {
                    WeaponSet s;
                    s.field = fk;
                    s.line = fv.line;
                    if (fv.IsNumber()) {
                        s.kind = WeaponSet::Number;
                        s.number = fv.number;
                    } else if (fv.IsBool()) {
                        s.kind = WeaponSet::Boolean;
                        s.boolean = fv.boolean;
                    } else if (fv.IsString() && fv.string.size() <= 120) {
                        s.kind = WeaponSet::String;
                        s.string = fv.string;
                    } else {
                        AddError(errs, &fv, "weapons.set", "weapons.set." + fk + " must be a number, a boolean or a string");
                        good = false;
                        continue;
                    }
                    w.set.push_back(std::move(s));
                }
            } else if (key == "text") {
                if (!m.IsObject()) {
                    AddError(errs, &m, "weapons.text", "weapons.text must be an object");
                    good = false;
                    continue;
                }
                for (const auto& [tk, tv] : m.members) {
                    if (tk == "name" && tv.IsString() && tv.string.size() <= 48) {
                        w.textName = tv.string;
                    } else if (tk == "help" && tv.IsString() && tv.string.size() <= 400) {
                        w.textHelp = tv.string;
                    } else {
                        AddError(errs, &tv, "weapons.text", "weapons.text takes name (<= 48) and help (<= 400) strings");
                        good = false;
                    }
                }
            } else {
                AddError(errs, &m, "weapons", "unknown weapons key '" + key + "'");
                good = false;
            }
        }
        if (w.name.empty() || w.base.empty()) {
            AddError(errs, &item, "weapons", "each weapon needs a name and a base");
            good = false;
        }
        if (good) out->weapons.push_back(std::move(w));
        ok &= good;
    }
    return ok;
}

bool ParseLevels(const json::Value& a, Manifest* out, std::vector<Error>* errs) {
    if (!a.IsArray()) {
        AddError(errs, &a, "levels", "levels must be an array");
        return false;
    }
    if (a.items.size() > 32) {
        AddError(errs, &a, "levels", "at most 32 levels per mod");
        return false;
    }
    bool ok = true;
    for (const json::Value& item : a.items) {
        if (!item.IsObject()) {
            AddError(errs, &item, "levels", "each level must be an object");
            ok = false;
            continue;
        }
        Level l;
        l.line = item.line;
        bool good = true;
        for (const auto& [key, m] : item.members) {
            if (key == "slug" || key == "title" || key == "type" || key == "source" || key == "sim") {
                const size_t max = key == "source" || key == "sim" ? 200 : key == "title" ? 40 : 24;
                if (!m.IsString() || m.string.empty() || m.string.size() > max) {
                    AddError(errs, &m, "levels", "levels." + key + " must be a string of 1-" + std::to_string(max) + " characters");
                    good = false;
                    continue;
                }
                (key == "slug" ? l.slug : key == "title" ? l.title : key == "type" ? l.type : key == "sim" ? l.sim : l.source) = m.string;
            } else if (key == "chunk" || key == "survivor") {
                if (!m.IsBool()) {
                    AddError(errs, &m, key == "chunk" ? "levels.chunk" : "levels.survivor", "levels." + key + " must be a boolean");
                    good = false;
                } else {
                    (key == "chunk" ? l.chunk : l.survivor) = m.boolean;
                }
            } else {
                AddError(errs, &m, "levels", "unknown levels key '" + key + "'");
                good = false;
            }
        }
        if (l.slug.empty() || l.title.empty()) {
            AddError(errs, &item, "levels", "each level needs a slug and a title");
            good = false;
        }
        if (good) out->levels.push_back(std::move(l));
        ok &= good;
    }
    return ok;
}

bool ParseManifestJson(const json::Value& v, const std::string& folderId, Manifest* out, std::vector<Error>* errs) {
    if (!v.IsObject()) {
        AddError(errs, &v, "", "spice.json must be a JSON object");
        return false;
    }
    bool ok = true;
    const json::Value* sv = v.Get("spiceVersion");
    if (!sv || !sv->IsInteger() || sv->number != 1) {
        AddError(errs, sv, "spiceVersion", "unknown or missing spiceVersion (Thumper understands 1)");
        ok = false;
    }
    const json::Value* idv = v.Get("id");
    if (!idv || !idv->IsString() || !ValidId(idv->string)) {
        AddError(errs, idv, "id", "id must be lowercase alphanumeric with '_'/'-', 1-64 characters");
        ok = false;
    } else if (idv->string != folderId) {
        AddError(errs, idv, "id", "id '" + idv->string + "' does not match its folder name '" + folderId + "'");
        ok = false;
    } else {
        out->id = idv->string;
    }
    const json::Value* verv = v.Get("version");
    if (!verv || !verv->IsString() || !ParseSemVer(verv->string).ok) {
        AddError(errs, verv, "version", "version must be a semver MAJOR.MINOR.PATCH");
        ok = false;
    } else {
        out->version = verv->string;
    }
    const json::Value* namev = v.Get("name");
    if (!namev || !namev->IsString() || namev->string.empty() || namev->string.size() > 80) {
        AddError(errs, namev, "name", "name must be 1-80 characters");
        ok = false;
    } else {
        out->name = namev->string;
    }
    if (const json::Value* d = v.Get("description")) {
        if (!d->IsString() || d->string.size() > 400) {
            AddError(errs, d, "description", "description must be at most 400 characters");
            ok = false;
        } else {
            out->description = d->string;
        }
    }
    if (const json::Value* w = v.Get("website")) {
        if (!w->IsString()) {
            AddError(errs, w, "website", "website must be a string");
            ok = false;
        } else {
            out->website = w->string;
        }
    }
    if (const json::Value* a = v.Get("authors")) {
        if (!a->IsArray()) {
            AddError(errs, a, "authors", "authors must be an array of strings");
            ok = false;
        } else {
            for (const json::Value& item : a->items) {
                if (!item.IsString() || item.string.empty() || item.string.size() > 80) {
                    AddError(errs, &item, "authors", "each author must be 1-80 characters");
                    ok = false;
                } else {
                    out->authors.push_back(item.string);
                }
            }
        }
    }
    const json::Value* mel = v.Get("melange");
    if (!mel || !mel->IsObject() || !mel->Get("range") || !mel->Get("range")->IsString()) {
        AddError(errs, mel, "melange.range", "melange.range is required");
        ok = false;
    } else {
        out->melangeRange = mel->Get("range")->string;
    }
    const json::Value* kindv = v.Get("kind");
    if (!kindv || !kindv->IsString() || (kindv->string != "client-only" && kindv->string != "content")) {
        AddError(errs, kindv, "kind", "kind must be 'client-only' or 'content'");
        ok = false;
    } else {
        out->content = kindv->string == "content";
    }
    auto readDeps = [&](const char* key, std::vector<Dep>* into) {
        const json::Value* a = v.Get(key);
        if (!a) return;
        if (!a->IsArray()) {
            AddError(errs, a, key, std::string(key) + " must be an array");
            ok = false;
            return;
        }
        for (const json::Value& item : a->items) {
            Dep d;
            if (!item.IsString() || !ParseDepString(item.string, &d) || !ValidId(d.id)) {
                AddError(errs, &item, key, std::string("bad dependency string in ") + key);
                ok = false;
                continue;
            }
            into->push_back(std::move(d));
        }
    };
    readDeps("dependencies", &out->dependencies);
    readDeps("optional", &out->optional);
    readDeps("conflicts", &out->conflicts);
    if (const json::Value* la = v.Get("loadAfter")) {
        if (!la->IsArray()) {
            AddError(errs, la, "loadAfter", "loadAfter must be an array");
            ok = false;
        } else {
            for (const json::Value& item : la->items) {
                if (!item.IsString() || !ValidId(item.string)) {
                    AddError(errs, &item, "loadAfter", "loadAfter entries must be plain mod ids");
                    ok = false;
                    continue;
                }
                out->loadAfter.push_back(item.string);
            }
        }
    }
    if (const json::Value* perm = v.Get("permissions")) {
        if (!perm->IsObject()) {
            AddError(errs, perm, "permissions", "permissions must be an object");
            ok = false;
        } else {
            if (const json::Value* u = perm->Get("unsafe")) {
                if (!u->IsBool()) {
                    AddError(errs, u, "permissions.unsafe", "permissions.unsafe must be a boolean");
                    ok = false;
                } else {
                    out->unsafe = u->boolean;
                }
            }
            if (const json::Value* fs = perm->Get("filesystem")) {
                if (!fs->IsString() || (fs->string != "none" && fs->string != "own-folder" && fs->string != "own-folder-write")) {
                    AddError(errs, fs, "permissions.filesystem", "permissions.filesystem must be none|own-folder|own-folder-write");
                    ok = false;
                } else {
                    out->filesystem = fs->string;
                }
            }
            if (const json::Value* net = perm->Get("network")) {
                if (net->IsBool() && net->boolean)
                    LOG_WARN("[spice] %s: permissions.network is reserved for a later milestone and is ignored", out->id.c_str());
            }
        }
    }
    if (const json::Value* entry = v.Get("entry")) {
        if (!entry->IsObject()) {
            AddError(errs, entry, "entry", "entry must be an object");
            ok = false;
        } else {
            if (const json::Value* c = entry->Get("client")) {
                if (!c->IsString()) {
                    AddError(errs, c, "entry.client", "entry.client must be a string path");
                    ok = false;
                } else {
                    out->entryClient = c->string;
                }
            }
            if (const json::Value* s = entry->Get("sim")) {
                if (!s->IsString()) {
                    AddError(errs, s, "entry.sim", "entry.sim must be a string path");
                    ok = false;
                } else {
                    out->entrySim = s->string;
                }
            }
        }
    }
    if (!out->entrySim.empty() && !out->content) {
        AddError(errs, v.Get("entry"), "entry.sim", "entry.sim requires kind: \"content\"");
        ok = false;
    }
    if (const json::Value* assets = v.Get("assets")) {
        if (!assets->IsObject()) {
            AddError(errs, assets, "assets", "assets must be an object");
            ok = false;
        } else {
            GetStr(*assets, "root", &out->assetsRoot, "assets");
            if (!SafeRelRoot(out->assetsRoot)) {
                AddError(errs, assets->Get("root"), "assets.root",
                          "assets.root must be a relative path with no '..' or drive letter");
                ok = false;
            }
            GetStr(*assets, "shaders", &out->shaders, "shaders");
            GetStr(*assets, "effects", &out->effects, "effects");
        }
    }
    if (const json::Value* ch = v.Get("contentHash")) {
        if (const json::Value* inc = ch->Get("include")) {
            if (!inc->IsArray()) {
                AddError(errs, inc, "contentHash.include", "contentHash.include must be an array");
                ok = false;
            } else {
                for (const json::Value& item : inc->items)
                    if (item.IsString()) out->hashInclude.push_back(item.string);
            }
        }
    }
    if (out->hashInclude.empty()) out->hashInclude = {"entry.sim", "assets/**", "*.spice.json"};
    if (const json::Value* msgs = v.Get("messages")) {
        if (!msgs->IsArray()) {
            AddError(errs, msgs, "messages", "messages must be an array");
            ok = false;
        } else if (msgs->items.size() > 16) {
            AddError(errs, msgs, "messages", "at most 16 message names per mod");
            ok = false;
        } else {
            for (const json::Value& item : msgs->items) {
                if (!item.IsString() || !ValidMessageName(item.string)) {
                    AddError(errs, &item, "messages", "'" + (item.IsString() ? item.string : std::string()) +
                                                           "' is not a valid message name");
                    ok = false;
                    continue;
                }
                out->messages.push_back(item.string);
            }
        }
    }
    if (!out->messages.empty() && !out->content) {
        AddError(errs, v.Get("messages"), "messages", "messages requires kind: \"content\"");
        ok = false;
    }
    if (const json::Value* w = v.Get("weapons")) {
        if (!ParseWeapons(*w, out, errs)) ok = false;
        if (!out->weapons.empty() && !out->content) {
            AddError(errs, w, "weapons", "weapons requires kind: \"content\"");
            ok = false;
        }
    }
    if (const json::Value* lv = v.Get("levels")) {
        if (!ParseLevels(*lv, out, errs)) ok = false;
        if (!out->levels.empty() && !out->content) {
            AddError(errs, lv, "levels", "levels requires kind: \"content\"");
            ok = false;
        }
    }
    if (const json::Value* settings = v.Get("settings")) {
        if (!settings->IsArray()) {
            AddError(errs, settings, "settings", "settings must be an array");
            ok = false;
        } else {
            std::unordered_set<std::string> keys;
            for (const json::Value& item : settings->items) {
                if (!item.IsObject()) {
                    AddError(errs, &item, "settings", "each setting must be an object");
                    ok = false;
                    continue;
                }
                Setting s;
                const json::Value* keyv = item.Get("key");
                if (!keyv || !keyv->IsString() || keyv->string.empty() ||
                    keyv->string.find_first_of("\r\n") != std::string::npos || !keys.insert(keyv->string).second) {
                    AddError(errs, keyv, "settings.key", "setting key must be a unique, non-empty string with no line breaks");
                    ok = false;
                    continue;
                }
                s.key = keyv->string;
                const json::Value* typev = item.Get("type");
                static const std::set<std::string> kTypes = {"bool", "int", "float", "string", "enum"};
                if (!typev || !typev->IsString() || !kTypes.count(typev->string)) {
                    AddError(errs, typev, "settings.type", "setting type must be bool|int|float|string|enum");
                    ok = false;
                    continue;
                }
                s.type = typev->string;
                GetStr(item, "label", &s.label, s.key.c_str());
                if (const json::Value* opts = item.Get("options")) {
                    if (opts->IsArray())
                        for (const json::Value& o : opts->items)
                            if (o.IsString() && o.string.find_first_of("\r\n") == std::string::npos) s.options.push_back(o.string);
                }
                if (s.type == "enum" && s.options.empty()) {
                    AddError(errs, typev, "settings.options", "an enum setting needs a non-empty options list");
                    ok = false;
                    continue;
                }
                if (const json::Value* mn = item.Get("min"); mn && mn->IsNumber()) s.min = mn->number;
                if (const json::Value* mx = item.Get("max"); mx && mx->IsNumber()) s.max = mx->number;
                if (const json::Value* def = item.Get("default")) {
                    if (def->IsString()) s.def = def->string;
                    else if (def->IsBool()) s.def = def->boolean ? "true" : "false";
                    else if (def->IsNumber()) s.def = std::to_string(def->number);
                }
                out->settings.push_back(std::move(s));
            }
        }
    }
    if (const json::Value* de = v.Get("defaultEnabled")) {
        if (!de->IsBool()) {
            AddError(errs, de, "defaultEnabled", "defaultEnabled must be a boolean");
            ok = false;
        } else {
            out->defaultEnabled = de->boolean;
        }
    }
    return ok;
}
}  // namespace

bool Parse(const std::wstring& dir, Manifest* out, std::vector<Error>* errs) {
    if (errs) errs->clear();
    *out = Manifest{};
    out->dir = dir;
    std::string folderId = ToLower(Narrow(FolderName(dir)));

    std::wstring path = dir;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    path += L"\\spice.json";

    json::Value v;
    json::Error jerr;
    if (!json::ParseFile(path, &v, &jerr)) {
        if (jerr.line == 0 && jerr.col == 0 && jerr.text == "cannot open the file") {
            // M1-era folder with no manifest: synthesise the implicit one so it keeps working unchanged.
            out->id = SanitizeId(folderId);
            out->version = "0.0.0";
            out->name = folderId;
            out->content = false;
            out->implicit = true;
            out->hashInclude = {"entry.sim", "assets/**", "*.spice.json"};
            return true;
        }
        Error e;
        e.field = "";
        e.line = jerr.line;
        e.col = jerr.col;
        e.text = jerr.text;
        if (errs) errs->push_back(std::move(e));
        return false;
    }
    return ParseManifestJson(v, folderId, out, errs);
}

// ---------------------------------------------------------------------------------------------
// Resolution: parse/validate (by the caller, via Parse) is already done; this stage builds the
// dependency graph, topo-sorts with a stable tie-break, then applies conflicts and consent.
// ---------------------------------------------------------------------------------------------
namespace {
struct Node {
    const Manifest* m;
    bool present = true;   // false for a dependency target that was never discovered
    mods::State state = mods::State::Disabled;
    std::string reason;
    int order = -1;
};

bool VersionOk(const Node& target, const Dep& dep) {
    return target.present && SemverSatisfies(target.m->version, dep.range);
}
}  // namespace

std::vector<Resolved> Resolve(const std::vector<Manifest>& all, const std::set<std::string>& userEnabled,
                               const std::string& melangeVersion, const std::vector<std::pair<std::string, std::string>>& pins) {
    std::map<std::string, Node> nodes;  // sorted by id: gives the map itself a deterministic iteration order
    for (const Manifest& m : all) nodes[m.id] = Node{&m};

    // Pass 1: schema validity (assumed already checked by Parse) plus melange-range / spiceVersion gating.
    // A manifest that reaches Resolve() already passed Parse(); here we only gate the engine-version range,
    // since that depends on the running build, not on the manifest alone.
    std::vector<std::string> order;  // ids still in the graph after pass 1, insertion order irrelevant
    for (auto& [id, n] : nodes) {
        if (!SemverSatisfies(melangeVersion, n.m->melangeRange)) {
            n.state = mods::State::Incompatible;
            n.reason = "needs melange " + n.m->melangeRange + ", running " + melangeVersion;
            continue;
        }
        order.push_back(id);
    }

    // Pass 2: edges. required/optional -> ordering + existence (required only); loadAfter/pins -> ordering only.
    std::unordered_map<std::string, int> indeg;
    std::unordered_map<std::string, std::vector<std::string>> adj;  // value -> [key...] (value must precede key)
    auto addEdge = [&](const std::string& before, const std::string& afterId) {
        if (before == afterId) return;
        adj[before].push_back(afterId);
        indeg[afterId]++;
    };
    for (const std::string& id : order) {
        indeg.try_emplace(id, 0);
        adj.try_emplace(id);
    }
    for (const std::string& id : order) {
        Node& n = nodes[id];
        if (n.state == mods::State::Incompatible) continue;
        for (const Dep& d : n.m->dependencies) {
            auto it = nodes.find(d.id);
            if (it == nodes.end() || it->second.state == mods::State::Incompatible || !VersionOk(it->second, d)) {
                n.state = mods::State::Blocked;
                if (it == nodes.end())
                    n.reason = "needs " + d.id + (d.range.empty() ? "" : " " + d.range) + " (not installed)";
                else
                    n.reason = "needs " + d.id + (d.range.empty() ? "" : " " + d.range) + ", have " + it->second.m->version;
                continue;
            }
            addEdge(d.id, id);
        }
        for (const Dep& d : n.m->optional) {
            auto it = nodes.find(d.id);
            if (it != nodes.end() && it->second.state != mods::State::Incompatible && VersionOk(it->second, d))
                addEdge(d.id, id);
        }
        for (const std::string& after_id : n.m->loadAfter) {
            if (nodes.count(after_id)) addEdge(after_id, id);
        }
    }
    for (const auto& [before, afterId] : pins) {
        if (nodes.count(before) && nodes.count(afterId) && nodes[before].state != mods::State::Incompatible &&
            nodes[afterId].state != mods::State::Incompatible)
            addEdge(before, afterId);
    }
    // A required-dependency failure just set above may need to propagate to things that depend on it; do that
    // as a closure once the direct edges are known below Kahn's sort, per pass 4.

    // Pass 3: Kahn's algorithm with ties broken by id ascending, over every node not yet Incompatible/Blocked.
    std::set<std::string> ready;
    std::unordered_map<std::string, int> remaining;
    for (const std::string& id : order) {
        if (nodes[id].state == mods::State::Blocked) continue;  // hard dep failure: excluded from the sort
        remaining[id] = indeg.count(id) ? indeg[id] : 0;
        if (remaining[id] == 0) ready.insert(id);
    }
    int nextOrder = 0;
    std::vector<std::string> sorted;
    while (!ready.empty()) {
        std::string id = *ready.begin();
        ready.erase(ready.begin());
        sorted.push_back(id);
        nodes[id].order = nextOrder++;
        for (const std::string& dep : adj[id]) {
            if (!remaining.count(dep)) continue;
            if (--remaining[dep] == 0) ready.insert(dep);
        }
    }
    if (sorted.size() != remaining.size()) {
        // Whatever never reached zero in-degree is part of (or blocked behind) a cycle.
        std::vector<std::string> stuck;
        for (const auto& [id, r] : remaining)
            if (nodes[id].order < 0) stuck.push_back(id);
        std::sort(stuck.begin(), stuck.end());
        std::string chain;
        for (size_t i = 0; i < stuck.size(); ++i) chain += (i ? " -> " : "") + stuck[i];
        if (!stuck.empty()) chain += " -> " + stuck.front();
        for (const std::string& id : stuck) {
            nodes[id].state = mods::State::Blocked;
            nodes[id].reason = "part of a dependency cycle: " + chain;
        }
    }

    // Pass 4: conflicts (symmetric) and required-dependency-block propagation, as one fixed point: either
    // can create a newly-blocked mod that the other pass then has to react to.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const std::string& id : order) {
            Node& n = nodes[id];
            if (n.state == mods::State::Blocked || n.state == mods::State::Incompatible) continue;
            for (const Dep& d : n.m->dependencies) {
                auto it = nodes.find(d.id);
                bool bad = it == nodes.end() || it->second.state == mods::State::Blocked ||
                           it->second.state == mods::State::Incompatible;
                if (bad) {
                    n.state = mods::State::Blocked;
                    n.reason = "blocked because " + d.id + " is blocked";
                    changed = true;
                    break;
                }
            }
        }
        for (const std::string& id : order) {
            Node& n = nodes[id];
            if (n.state == mods::State::Blocked || n.state == mods::State::Incompatible) continue;
            for (const Dep& d : n.m->conflicts) {
                auto it = nodes.find(d.id);
                if (it == nodes.end()) continue;
                Node& other = it->second;
                if (other.state == mods::State::Blocked || other.state == mods::State::Incompatible) continue;
                if (!d.range.empty() && !VersionOk(other, d)) continue;
                n.state = mods::State::Blocked;
                n.reason = "conflicts with " + d.id;
                other.state = mods::State::Blocked;
                other.reason = "conflicts with " + id;
                changed = true;
            }
        }
    }
    for (const std::string& id : order) {
        Node& n = nodes[id];
        if (n.state == mods::State::Blocked || n.state == mods::State::Incompatible) continue;
        bool wantsOn = userEnabled.count(id) != 0;
        if (!wantsOn) {
            n.state = mods::State::Disabled;
            continue;
        }
        if (n.m->unsafe) {
            n.state = mods::State::PendingConsent;  // the caller flips this to Enabled once granted
            continue;
        }
        n.state = mods::State::Enabled;
    }

    std::vector<Resolved> out;
    out.reserve(nodes.size());
    for (const auto& [id, n] : nodes) out.push_back({id, n.state, n.reason, n.order});
    // Deterministic, order-independent of `all`'s input order: unplaced (incompatible / never sorted) entries
    // sort after every placed one, then everyone ties by id ascending.
    std::sort(out.begin(), out.end(), [](const Resolved& a, const Resolved& b) {
        bool ap = a.order >= 0, bp = b.order >= 0;
        if (ap != bp) return ap;
        if (ap) return a.order < b.order;
        return a.id < b.id;
    });
    return out;
}
}  // namespace melange::spice
