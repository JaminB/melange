#include "import/recipe.h"

#include <cctype>
#include <initializer_list>
#include <set>

#include "mods/spice.h"
#include "tools/json_read.h"

namespace melange::import {
namespace {
constexpr const char* kHosts[] = {"mod.worms.pro"};

char Low(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

struct Ctx {
    std::string* err;
    bool Fail(const std::string& path, const std::string& why) {
        if (err && err->empty()) *err = path + ": " + why;
        return false;
    }
};

bool Keys(Ctx& c, const json::Value& v, const std::string& path, std::initializer_list<const char*> allowed,
          std::initializer_list<const char*> required = {}) {
    if (!v.IsObject()) return c.Fail(path, "must be an object");
    for (const auto& [k, _] : v.members) {
        bool ok = false;
        for (const char* a : allowed) ok |= k == a;
        if (!ok) return c.Fail(path, "unknown key '" + k + "'");
    }
    for (const char* r : required)
        if (!v.Get(r)) return c.Fail(path, "'" + std::string(r) + "' is required");
    return true;
}

bool Printable(std::string_view s, size_t min, size_t max) {
    if (s.size() < min || s.size() > max) return false;
    for (unsigned char ch : s)
        if (ch < 0x20 || ch > 0x7e) return false;
    return true;
}

bool Str(Ctx& c, const json::Value& o, const char* k, const std::string& path, std::string* out, size_t min, size_t max,
         bool required = true) {
    const json::Value* v = o.Get(k);
    if (!v) return required ? c.Fail(path + "." + k, "is required") : true;
    if (!v->IsString() || !Printable(v->string, min, max))
        return c.Fail(path + "." + k, "must be " + std::to_string(min) + "-" + std::to_string(max) + " printable ASCII characters");
    *out = v->string;
    return true;
}

bool Int(Ctx& c, const json::Value& o, const char* k, const std::string& path, int* out, int min, int max, bool required = true) {
    const json::Value* v = o.Get(k);
    if (!v) return required ? c.Fail(path + "." + k, "is required") : true;
    if (!v->IsInteger() || v->number < min || v->number > max)
        return c.Fail(path + "." + k, "must be an integer " + std::to_string(min) + "-" + std::to_string(max));
    *out = static_cast<int>(v->number);
    return true;
}

bool Bool(Ctx& c, const json::Value& o, const char* k, const std::string& path, bool* out) {
    const json::Value* v = o.Get(k);
    if (!v) return true;
    if (!v->IsBool()) return c.Fail(path + "." + k, "must be true or false");
    *out = v->boolean;
    return true;
}

bool StrList(Ctx& c, const json::Value& o, const char* k, const std::string& path, std::vector<std::string>* out,
             size_t maxItems, size_t maxLen, bool required = false) {
    const json::Value* v = o.Get(k);
    if (!v) return required ? c.Fail(path + "." + k, "is required") : true;
    if (!v->IsArray() || v->items.size() > maxItems || (required && v->items.empty()))
        return c.Fail(path + "." + k, "must be an array of at most " + std::to_string(maxItems) + " strings");
    for (const auto& it : v->items) {
        if (!it.IsString() || !Printable(it.string, 1, maxLen))
            return c.Fail(path + "." + k, "each item must be 1-" + std::to_string(maxLen) + " printable ASCII characters");
        out->push_back(it.string);
    }
    return true;
}

bool SameI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Low(a[i]) != Low(b[i])) return false;
    return true;
}

bool Hex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char ch : s)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    return true;
}

bool Token(std::string_view s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (char ch : s)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')) return false;
    return true;
}

// Relative, '/'-separated, no "..", no drive, at most 8 segments, '*' only inside a segment.
bool ReaderPath(std::string_view p) {
    if (p.empty() || p.size() > 160 || p.front() == '/' || p.find('\\') != std::string_view::npos ||
        p.find(':') != std::string_view::npos)
        return false;
    size_t i = 0, segs = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string_view::npos) j = p.size();
        const std::string_view seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == ".." || ++segs > 8) return false;
        for (unsigned char ch : seg)
            if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-' || ch == ' ' || ch == '*')) return false;
        i = j + 1;
    }
    return true;
}

bool FileNameOk(std::string_view s) {
    if (s.size() < 5 || s.size() > 68 || !s.ends_with(".zip")) return false;
    for (unsigned char ch : s)
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == ' ' || ch == '-')) return false;
    return s.front() != '.' && s.front() != ' ';
}

bool VanillaPath(std::string_view s) {
    return ReaderPath(s) && s.find('*') == std::string_view::npos && s.size() <= 64;
}

bool ParseSources(Ctx& c, const json::Value& v, Recipe* r) {
    if (!v.IsArray() || v.items.empty() || v.items.size() > 4) return c.Fail("sources", "must hold 1 to 4 sources");
    std::set<std::string> ids;
    for (size_t i = 0; i < v.items.size(); ++i) {
        const json::Value& s = v.items[i];
        const std::string path = "sources[" + std::to_string(i) + "]";
        if (!Keys(c, s, path, {"id", "name", "urls", "fileName", "size", "sha256"}, {"id", "name", "urls", "fileName", "size", "sha256"}))
            return false;
        Source src;
        if (!Str(c, s, "id", path, &src.id, 1, 24) || !Str(c, s, "name", path, &src.name, 1, 64) ||
            !Str(c, s, "fileName", path, &src.fileName, 5, 68) || !Str(c, s, "sha256", path, &src.sha256, 64, 64))
            return false;
        if (!Token(src.id, 24) || !ids.insert(src.id).second) return c.Fail(path + ".id", "must be a unique [a-z0-9_-] token");
        if (!FileNameOk(src.fileName)) return c.Fail(path + ".fileName", "must be a plain .zip file name");
        if (!Hex64(src.sha256)) return c.Fail(path + ".sha256", "must be 64 lowercase hex digits");
        const json::Value* size = s.Get("size");
        if (!size->IsInteger() || size->number < 1 || size->number > 512.0 * 1024 * 1024)
            return c.Fail(path + ".size", "must be 1 byte to 512 MiB");
        src.size = static_cast<uint64_t>(size->number);
        if (!StrList(c, s, "urls", path, &src.urls, 4, 512, true)) return false;
        for (const auto& u : src.urls)
            if (!AllowedHost(HostOf(u))) return c.Fail(path + ".urls", u + " is not an https:// URL on an allowed host");
        r->sources.push_back(std::move(src));
    }
    return true;
}

bool ParseReader(Ctx& c, const json::Value& v, Recipe* r) {
    if (!Keys(c, v, "reader", {"type", "root", "registry", "titles", "descriptors", "maps", "previews"},
              {"type", "root", "registry", "descriptors", "maps"}))
        return false;
    Reader& rd = r->reader;
    if (!Str(c, v, "type", "reader", &rd.type, 1, 32)) return false;
    if (rd.type != "w4-registry") return c.Fail("reader.type", "must be 'w4-registry'");
    if (!Str(c, v, "root", "reader", &rd.root, 1, 64) || !Str(c, v, "registry", "reader", &rd.registry, 1, 64) ||
        !Str(c, v, "descriptors", "reader", &rd.descriptors, 1, 64) || !Str(c, v, "previews", "reader", &rd.previews, 1, 64, false) ||
        !StrList(c, v, "titles", "reader", &rd.titles, 4, 64) || !StrList(c, v, "maps", "reader", &rd.maps, 8, 64, true))
        return false;
    std::vector<std::string> all = {rd.root, rd.registry, rd.descriptors};
    if (!rd.previews.empty()) all.push_back(rd.previews);
    all.insert(all.end(), rd.titles.begin(), rd.titles.end());
    all.insert(all.end(), rd.maps.begin(), rd.maps.end());
    for (const auto& p : all)
        if (!ReaderPath(p)) return c.Fail("reader", "'" + p + "' is not a safe relative path");
    if (rd.root.find('*') != std::string::npos || rd.registry.find('*') != std::string::npos)
        return c.Fail("reader", "root and registry cannot hold '*'");
    for (const auto& t : rd.titles)
        if (t.find('*') != std::string::npos) return c.Fail("reader.titles", "cannot hold '*'");
    if (rd.descriptors.find('/') != std::string::npos || !rd.descriptors.starts_with("*"))
        return c.Fail("reader.descriptors", "must be a pattern for files at the root, like *.XOM");
    bool xan = false;
    for (const auto& m : rd.maps) xan |= m.ends_with(".xan");
    if (!xan) return c.Fail("reader.maps", "must name the .xan files");
    for (const auto& m : rd.maps)
        if (!(m.ends_with(".xan") || m.ends_with(".txt") || m.ends_with(".hmp")))
            return c.Fail("reader.maps", "'" + m + "' must end in .xan, .txt or .hmp");
    if (!rd.previews.empty() && !rd.previews.ends_with(".tga")) return c.Fail("reader.previews", "must end in .tga");
    return true;
}

bool ParseSelect(Ctx& c, const json::Value& v, Recipe* r) {
    if (!Keys(c, v, "select", {"levelType", "skipKeySuffix", "require", "exclude", "vanilla", "expect"}, {"levelType", "expect"}))
        return false;
    Select& s = r->select;
    if (const json::Value* lt = v.Get("levelType"); lt->IsArray()) {
        if (lt->items.empty() || lt->items.size() > 8) return c.Fail("select.levelType", "must list 1 to 8 level types");
        for (const auto& it : lt->items) {
            if (!it.IsInteger() || it.number < 0 || it.number > 64) return c.Fail("select.levelType", "each level type must be an integer 0-64");
            s.levelTypes.push_back(static_cast<int>(it.number));
        }
    } else {
        int one = 0;
        if (!Int(c, v, "levelType", "select", &one, 0, 64)) return false;
        s.levelTypes.push_back(one);
    }
    if (!Str(c, v, "skipKeySuffix", "select", &s.skipKeySuffix, 1, 8, false) ||
        !StrList(c, v, "require", "select", &s.require, 2, 16) || !StrList(c, v, "exclude", "select", &s.exclude, 64, 64))
        return false;
    for (const auto& q : s.require)
        if (q != "descriptor" && q != "xan") return c.Fail("select.require", "only 'descriptor' and 'xan'");
    if (s.require.size() == 1 || (s.require.size() == 2 && s.require[0] == s.require[1]))
        return c.Fail("select.require", "format 1 needs both 'descriptor' and 'xan'");
    if (const json::Value* van = v.Get("vanilla")) {
        if (!van->IsArray() || van->items.size() > 32) return c.Fail("select.vanilla", "must be an array of at most 32 entries");
        std::set<std::string> files;
        for (size_t i = 0; i < van->items.size(); ++i) {
            const json::Value& e = van->items[i];
            const std::string path = "select.vanilla[" + std::to_string(i) + "]";
            if (!Keys(c, e, path, {"file", "sha256"}, {"file", "sha256"})) return false;
            VanillaPin p;
            if (!Str(c, e, "file", path, &p.file, 1, 48)) return false;
            if (!ReaderPath(p.file) || p.file.find_first_of("/*") != std::string::npos || !files.insert(p.file).second)
                return c.Fail(path + ".file", "must be a unique plain file name");
            const json::Value* h = e.Get("sha256");
            if (!h->IsObject() || h->members.empty() || h->members.size() > 4) return c.Fail(path + ".sha256", "must map 1-4 files to hashes");
            bool xom = false, xan = false;
            for (const auto& [k, hv] : h->members) {
                if (!VanillaPath(k) || !hv.IsString() || !Hex64(hv.string)) return c.Fail(path + ".sha256", "'" + k + "' needs a sha256");
                const std::string stem = k.substr(k.rfind('/') == std::string::npos ? 0 : k.rfind('/') + 1);
                const std::string want = stem.substr(0, stem.rfind('.'));
                if (!SameI(want, p.file)) return c.Fail(path + ".sha256", "'" + k + "' is not a file of " + p.file);
                const bool top = k.find('/') == std::string::npos, inMaps = k.starts_with("Maps/") && k.find('/', 5) == std::string::npos;
                if (top && k.ends_with(".XOM")) xom = true;
                else if (inMaps && k.ends_with(".xan")) xan = true;
                else if (!(inMaps && k.ends_with(".hmp"))) return c.Fail(path + ".sha256", "'" + k + "' is not <file>.XOM, Maps/<file>.xan or .hmp");
                p.sha256.emplace_back(k, hv.string);
            }
            if (!xom || !xan) return c.Fail(path + ".sha256", "must pin the .XOM and the .xan");
            s.vanilla.push_back(std::move(p));
        }
    }
    const json::Value& ex = *v.Get("expect");
    if (!Keys(c, ex, "select.expect", {"maps", "fromArchive", "fromGame"}, {"maps", "fromArchive", "fromGame"})) return false;
    if (!Int(c, ex, "maps", "select.expect", &s.expectMaps, 1, 256) ||
        !Int(c, ex, "fromArchive", "select.expect", &s.expectArchive, 0, 256) ||
        !Int(c, ex, "fromGame", "select.expect", &s.expectGame, 0, 32))
        return false;
    if (s.expectArchive + s.expectGame != s.expectMaps) return c.Fail("select.expect", "fromArchive + fromGame must equal maps");
    if (s.expectGame > static_cast<int>(s.vanilla.size())) return c.Fail("select.expect", "fromGame is more than the vanilla list");
    return true;
}

bool ParseCategories(Ctx& c, const json::Value& v, Recipe* r) {
    if (!v.IsArray() || v.items.empty() || v.items.size() > 8) return c.Fail("categories", "must hold 1 to 8 categories");
    std::set<std::string> ids;
    int defaults = 0;
    for (size_t i = 0; i < v.items.size(); ++i) {
        const json::Value& e = v.items[i];
        const std::string path = "categories[" + std::to_string(i) + "]";
        if (!Keys(c, e, path, {"id", "label", "scriptsEqual", "scriptsWithin", "default", "hidden"}, {"id", "label"})) return false;
        Category k;
        if (!Str(c, e, "id", path, &k.id, 1, 16) || !Str(c, e, "label", path, &k.label, 1, 48) ||
            !StrList(c, e, "scriptsEqual", path, &k.scriptsEqual, 16, 32) ||
            !StrList(c, e, "scriptsWithin", path, &k.scriptsWithin, 16, 32) || !Bool(c, e, "default", path, &k.isDefault) ||
            !Bool(c, e, "hidden", path, &k.hidden))
            return false;
        if (!Token(k.id, 16) || !ids.insert(k.id).second) return c.Fail(path + ".id", "must be a unique [a-z0-9_-] token");
        if (!k.scriptsEqual.empty() && !k.scriptsWithin.empty()) return c.Fail(path, "scriptsEqual or scriptsWithin, not both");
        if (k.isDefault) {
            ++defaults;
            if (!k.scriptsEqual.empty() || !k.scriptsWithin.empty()) return c.Fail(path, "the default category has no rule");
        } else if (k.scriptsEqual.empty() && k.scriptsWithin.empty()) {
            return c.Fail(path, "needs scriptsEqual or scriptsWithin");
        }
        r->categories.push_back(std::move(k));
    }
    if (defaults != 1) return c.Fail("categories", "exactly one category must be the default");
    return true;
}

bool ParseGroups(Ctx& c, const json::Value& v, Recipe* r) {
    if (!v.IsArray() || v.items.empty() || v.items.size() > 8) return c.Fail("groups", "must hold 1 to 8 groups");
    std::set<std::string> ids;
    int defaults = 0;
    for (size_t i = 0; i < v.items.size(); ++i) {
        const json::Value& e = v.items[i];
        const std::string path = "groups[" + std::to_string(i) + "]";
        if (!Keys(c, e, path, {"id", "label", "match", "vanilla", "default"}, {"id", "label"})) return false;
        Group g;
        if (!Str(c, e, "id", path, &g.id, 1, 16) || !Str(c, e, "label", path, &g.label, 1, 48) ||
            !StrList(c, e, "match", path, &g.match, 16, 48) || !Bool(c, e, "vanilla", path, &g.vanilla) ||
            !Bool(c, e, "default", path, &g.isDefault))
            return false;
        if (!Token(g.id, 16) || !ids.insert(g.id).second) return c.Fail(path + ".id", "must be a unique [a-z0-9_-] token");
        const int rules = (g.match.empty() ? 0 : 1) + (g.vanilla ? 1 : 0) + (g.isDefault ? 1 : 0);
        if (rules != 1) return c.Fail(path, "needs exactly one of match, vanilla or default");
        defaults += g.isDefault ? 1 : 0;
        r->groups.push_back(std::move(g));
    }
    if (defaults != 1) return c.Fail("groups", "exactly one group must be the default");
    return true;
}

bool ParseTransform(Ctx& c, const json::Value& v, Recipe* r) {
    if (!Keys(c, v, "transform", {"stem", "descriptor", "timeOfDay", "title", "author", "textures", "scripts", "survivor", "previews"},
              {"stem", "descriptor", "textures", "scripts"}))
        return false;
    Transform& t = r->transform;
    const json::Value& st = *v.Get("stem");
    if (!Keys(c, st, "transform.stem", {"slugMax", "hashKeep", "hashHex"}, {"slugMax", "hashKeep", "hashHex"}) ||
        !Int(c, st, "slugMax", "transform.stem", &t.slugMax, 8, 24) || !Int(c, st, "hashKeep", "transform.stem", &t.hashKeep, 0, 23) ||
        !Int(c, st, "hashHex", "transform.stem", &t.hashHex, 1, 16))
        return false;
    if (t.hashKeep + t.hashHex > t.slugMax) return c.Fail("transform.stem", "hashKeep + hashHex must fit in slugMax");
    std::string s;
    if (!Str(c, v, "descriptor", "transform", &s, 1, 16)) return false;
    if (s != "rebuild") return c.Fail("transform.descriptor", "must be 'rebuild' in format 1");
    if (!Str(c, v, "textures", "transform", &s, 1, 16)) return false;
    if (s != "vanilla") return c.Fail("transform.textures", "must be 'vanilla' in format 1");
    if (!Str(c, v, "scripts", "transform", &s, 1, 16)) return false;
    if (s != "drop") return c.Fail("transform.scripts", "must be 'drop' in format 1");
    t.timeOfDay = {"DAY", "EVENING", "NIGHT"};
    if (const json::Value* tod = v.Get("timeOfDay")) {
        if (!Keys(c, *tod, "transform.timeOfDay", {"allowed", "fallback"}, {"allowed", "fallback"})) return false;
        t.timeOfDay.clear();
        if (!StrList(c, *tod, "allowed", "transform.timeOfDay", &t.timeOfDay, 3, 8, true) ||
            !Str(c, *tod, "fallback", "transform.timeOfDay", &t.timeOfDayFallback, 1, 8))
            return false;
        for (const auto& x : t.timeOfDay)
            if (x != "DAY" && x != "EVENING" && x != "NIGHT") return c.Fail("transform.timeOfDay", "only DAY, EVENING and NIGHT");
        bool found = false;
        for (const auto& x : t.timeOfDay) found |= x == t.timeOfDayFallback;
        if (!found) return c.Fail("transform.timeOfDay.fallback", "must be one of allowed");
    }
    if (const json::Value* ti = v.Get("title")) {
        if (!Keys(c, *ti, "transform.title", {"max", "fallback"}, {}) || !Int(c, *ti, "max", "transform.title", &t.titleMax, 8, 40, false))
            return false;
        std::string fb = "fileName";
        if (!Str(c, *ti, "fallback", "transform.title", &fb, 1, 16, false)) return false;
        if (fb != "fileName") return c.Fail("transform.title.fallback", "must be 'fileName'");
    }
    if (!Str(c, v, "author", "transform", &t.author, 1, 48, false) || !Bool(c, v, "survivor", "transform", &t.survivor)) return false;
    if (!t.author.empty() && !t.author.starts_with("Databank.")) return c.Fail("transform.author", "must name a Databank.* key");
    if (v.Get("previews")) {
        if (!Str(c, v, "previews", "transform", &s, 1, 16)) return false;
        if (s != "local" && s != "none") return c.Fail("transform.previews", "must be 'local' or 'none'");
        t.previews = s == "local";
    }
    return true;
}

bool ParseOutput(Ctx& c, const json::Value& v, const std::string& pluginId, Recipe* r) {
    if (!Keys(c, v, "output", {"packPrefix", "version", "perPack", "order", "newPackBefore", "packName", "packDescription"},
              {"packPrefix", "version", "perPack", "packName", "packDescription"}))
        return false;
    Output& o = r->output;
    std::vector<std::string> order;
    if (!Str(c, v, "packPrefix", "output", &o.packPrefix, 1, 62) || !Str(c, v, "version", "output", &o.version, 5, 32) ||
        !Int(c, v, "perPack", "output", &o.perPack, 1, 32) || !StrList(c, v, "order", "output", &order, 2, 16) ||
        !StrList(c, v, "newPackBefore", "output", &o.newPackBefore, 8, 16) ||
        !Str(c, v, "packName", "output", &o.packName, 1, 72) || !Str(c, v, "packDescription", "output", &o.packDescription, 1, 400))
        return false;
    if (o.packPrefix != pluginId) return c.Fail("output.packPrefix", "must equal the plugin id '" + pluginId + "'");
    if (!spice::ValidSemver(o.version)) return c.Fail("output.version", "must be a semver");
    if (!order.empty() && (order.size() != 2 || order[0] != "category" || order[1] != "fileName"))
        return c.Fail("output.order", "must be [\"category\", \"fileName\"] in format 1");
    if (o.packName.find("{n}") == std::string::npos || o.packName.size() > 70)
        return c.Fail("output.packName", "must contain {n}");
    for (const auto& id : o.newPackBefore) {
        bool known = false;
        for (const auto& k : r->categories) known |= k.id == id;
        if (!known) return c.Fail("output.newPackBefore", "'" + id + "' is not a category");
    }
    return true;
}
}  // namespace

bool AllowedHost(std::string_view host) {
    for (const char* h : kHosts)
        if (host == h) return true;
    return false;
}

std::string HostOf(std::string_view u) {
    if (!u.starts_with("https://")) return {};
    std::string_view rest = u.substr(8);
    const size_t end = rest.find_first_of("/?#");
    std::string_view auth = rest.substr(0, end);
    if (end == std::string_view::npos || rest[end] != '/' || auth.find('@') != std::string_view::npos) return {};
    for (unsigned char ch : u)
        if (ch <= 0x20 || ch >= 0x7f || ch == '\\') return {};
    if (const size_t colon = auth.find(':'); colon != std::string_view::npos) {
        if (auth.substr(colon + 1) != "443") return {};
        auth = auth.substr(0, colon);
    }
    std::string h(auth);
    for (char& ch : h) ch = Low(ch);
    if (h.empty() || h.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != std::string::npos) return {};
    const std::string q(rest.substr(end));
    if (q.find('?') != std::string::npos) {
        std::string lq = q;
        for (char& ch : lq) ch = Low(ch);
        for (const char* bad : {"token", "key", "password", "passwd", "secret", "auth", "sig"})
            if (lq.find(bad) != std::string::npos) return {};
    }
    return h;
}

bool ValidRecipeId(std::string_view s) {
    if (s.empty() || s.size() > 48) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char ch = s[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (i && (ch == '.' || ch == '-')))) return false;
    }
    return true;
}

bool GlobMatch(std::string_view p, std::string_view s) {
    size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (pi < p.size() && Low(p[pi]) == Low(s[si])) {
            ++pi;
            ++si;
        } else if (star != std::string_view::npos && s[mark] != '/') {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') ++pi;
    return pi == p.size();
}

bool ParseRecipe(std::string_view text, const std::string& pluginId, Recipe* out, std::string* err, bool* unsupported) {
    *out = Recipe{};
    if (err) err->clear();
    if (unsupported) *unsupported = false;
    Ctx c{err};
    json::Value v;
    json::Error je;
    if (text.size() > 64 * 1024) return c.Fail("recipe", "larger than 64 KiB");
    if (!json::Parse(text, &v, &je)) return c.Fail("recipe", "line " + std::to_string(je.line) + ": " + je.text);
    if (!Keys(c, v, "recipe", {"importVersion", "format", "id", "name", "content", "sources", "reader", "select", "categories", "modes",
                               "groups", "transform", "output"},
              {"importVersion", "format", "id", "name", "content", "sources", "reader", "select", "categories", "groups", "transform",
               "output"}))
        return false;
    int iv = 0;
    if (!Int(c, v, "importVersion", "recipe", &iv, 1, 1)) return false;
    if (!Int(c, v, "format", "recipe", &out->format, 1, 999)) return false;
    if (!Str(c, v, "id", "recipe", &out->id, 1, 48) || !Str(c, v, "name", "recipe", &out->name, 1, 80)) return false;
    if (!ValidRecipeId(out->id)) return c.Fail("id", "must match ^[a-z0-9][a-z0-9.-]{0,47}$");
    const json::Value& ct = *v.Get("content");
    if (!Keys(c, ct, "content", {"title", "publisher", "termsUrl", "credit"}, {"title", "publisher", "termsUrl", "credit"}) ||
        !Str(c, ct, "title", "content", &out->content.title, 1, 64) || !Str(c, ct, "publisher", "content", &out->content.publisher, 1, 64) ||
        !Str(c, ct, "termsUrl", "content", &out->content.termsUrl, 9, 256) || !Str(c, ct, "credit", "content", &out->content.credit, 1, 300))
        return false;
    if (!AllowedHost(HostOf(out->content.termsUrl))) return c.Fail("content.termsUrl", "must be https:// on an allowed host");
    if (out->format != kFormat) {
        if (unsupported) *unsupported = true;
        return c.Fail("format", "importer format " + std::to_string(out->format) + " needs a newer Melange");
    }
    if (!ParseSources(c, *v.Get("sources"), out) || !ParseReader(c, *v.Get("reader"), out) || !ParseSelect(c, *v.Get("select"), out) ||
        !ParseCategories(c, *v.Get("categories"), out) || !ParseGroups(c, *v.Get("groups"), out) ||
        !ParseTransform(c, *v.Get("transform"), out) || !ParseOutput(c, *v.Get("output"), pluginId, out))
        return false;
    if (const json::Value* m = v.Get("modes")) {
        if (!m->IsObject() || m->members.size() > 32) return c.Fail("modes", "must map at most 32 script tokens to labels");
        for (const auto& [k, lv] : m->members) {
            if (!Printable(k, 1, 32) || !lv.IsString() || !Printable(lv.string, 1, 40)) return c.Fail("modes", "bad entry '" + k + "'");
            out->modes.emplace_back(k, lv.string);
        }
    }
    if (out->transform.previews && out->reader.previews.empty()) return c.Fail("transform.previews", "needs reader.previews");
    // Packs needed for the expected count, with the category breaks, must fit kMaxPacks.
    const size_t perPack = static_cast<size_t>(out->output.perPack);
    const size_t minPacks = (static_cast<size_t>(out->select.expectMaps) + perPack - 1) / perPack + out->output.newPackBefore.size();
    if (minPacks > kMaxPacks) return c.Fail("output", "the maps would need more than 9 packs");
    return true;
}
}  // namespace melange::import
