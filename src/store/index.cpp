#include "store/index.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <set>

#include "mods/spice.h"
#include "tools/json_read.h"

namespace melange::store {
namespace {
using json::Value;

std::string LowerAscii(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

bool HasControl(std::string_view s, bool allowNewlines) {
    for (unsigned char c : s)
        if ((c < 0x20 && !(allowNewlines && (c == '\n' || c == '\t' || c == '\r'))) || c == 0x7f) return true;
    return false;
}

bool IsHex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

struct Reader {
    std::string why;
    bool Fail(std::string w) {
        if (why.empty()) why = std::move(w);
        return false;
    }
    bool Text(const Value& o, const char* key, std::string* out, size_t maxLen, bool required, bool multiline = false) {
        const Value* v = o.Get(key);
        if (!v || v->IsNull()) return !required || Fail(std::string(key) + " is missing");
        if (!v->IsString()) return Fail(std::string(key) + " must be a string");
        if (v->string.size() > maxLen) return Fail(std::string(key) + " is too long");
        if (HasControl(v->string, multiline)) return Fail(std::string(key) + " has control characters");
        *out = v->string;
        return true;
    }
    bool Strings(const Value& o, const char* key, std::vector<std::string>* out, size_t maxCount, size_t maxLen) {
        const Value* v = o.Get(key);
        if (!v || v->IsNull()) return true;
        if (!v->IsArray() || v->items.size() > maxCount) return Fail(std::string(key) + " must be a short list");
        for (const Value& s : v->items) {
            if (!s.IsString() || s.string.empty() || s.string.size() > maxLen || HasControl(s.string, false))
                return Fail(std::string(key) + " has an invalid entry");
            out->push_back(s.string);
        }
        return true;
    }
    bool Size(const Value& o, const char* key, uint64_t* out, uint64_t max) {
        const Value* v = o.Get(key);
        if (!v || !v->IsInteger() || v->number < 1 || v->number > static_cast<double>(max))
            return Fail(std::string(key) + " is out of range");
        *out = static_cast<uint64_t>(v->number);
        return true;
    }
    bool Deps(const Value& o, const char* key, std::vector<Dep>* out) {
        const Value* v = o.Get(key);
        if (!v || v->IsNull()) return true;
        if (!v->IsArray() || v->items.size() > 32) return Fail(std::string(key) + " must be a short list");
        for (const Value& d : v->items) {
            Dep dep;
            if (d.IsString()) {
                std::string_view s = d.string;
                size_t p = 0;
                while (p < s.size() && (std::isalnum(static_cast<unsigned char>(s[p])) || s[p] == '-' || s[p] == '_')) ++p;
                dep.id = std::string(s.substr(0, p));
                size_t q = p;
                while (q < s.size() && s[q] == ' ') ++q;
                dep.range = std::string(s.substr(q));
            } else if (d.IsObject()) {
                if (const Value* i = d.Get("id"); i && i->IsString()) dep.id = i->string;
                if (const Value* r = d.Get("range"); r && r->IsString()) dep.range = r->string;
            }
            if (!spice::ValidModId(dep.id) || dep.range.size() > 100 || !spice::ValidRange(dep.range))
                return Fail(std::string(key) + " has an invalid entry");
            out->push_back(std::move(dep));
        }
        return true;
    }
};

bool ReadVersion(const Value& o, Version* v, std::string* why) {
    Reader r;
    if (!o.IsObject()) {
        *why = "a version is not an object";
        return false;
    }
    bool ok = r.Text(o, "version", &v->version, 64, true) && r.Text(o, "released", &v->released, 32, false) &&
              r.Text(o, "melange", &v->melange, 100, false) && r.Text(o, "kind", &v->kind, 16, true) &&
              r.Text(o, "url", &v->url, 512, true) && r.Text(o, "sha256", &v->sha256, 64, true) &&
              r.Text(o, "changelog", &v->changelog, 2000, false, true) && r.Size(o, "size", &v->size, kMaxZipBytes) &&
              r.Size(o, "unpackedSize", &v->unpackedSize, kMaxUnpackedBytes) && r.Deps(o, "dependencies", &v->dependencies) &&
              r.Deps(o, "conflicts", &v->conflicts);
    if (ok) {
        uint64_t files = 0;
        ok = r.Size(o, "files", &files, kMaxFiles);
        v->files = static_cast<int>(files);
    }
    if (ok && !spice::ValidSemver(v->version)) ok = r.Fail("version is not semver");
    if (ok && !spice::ValidRange(v->melange)) ok = r.Fail("melange is not a version range");
    if (ok && v->kind != "client-only" && v->kind != "content") ok = r.Fail("kind must be client-only or content");
    if (ok && !IsHex64(v->sha256)) ok = r.Fail("sha256 must be 64 lower-case hex digits");
    if (ok) {
        if (const Value* p = o.Get("permissions"); p && !p->IsNull()) {
            if (!p->IsObject()) {
                ok = r.Fail("permissions must be an object");
            } else {
                if (const Value* u = p->Get("unsafe"); u && !u->IsNull()) {
                    if (!u->IsBool()) ok = r.Fail("permissions.unsafe must be true or false");
                    else v->unsafe = u->boolean;
                }
                if (ok && !r.Text(*p, "filesystem", &v->filesystem, 32, false)) ok = false;
                if (ok && v->filesystem.empty()) v->filesystem = "none";
                if (ok && v->filesystem != "none" && v->filesystem != "own-folder" && v->filesystem != "own-folder-write")
                    ok = r.Fail("permissions.filesystem is unknown");
                if (const Value* n = p->Get("network"); ok && n && !(n->IsBool() && !n->boolean) && !n->IsNull())
                    ok = r.Fail("permissions.network is not allowed");
            }
        }
    }
    if (ok) {
        if (const Value* y = o.Get("yanked"); y && !y->IsNull()) {
            if (!y->IsBool()) ok = r.Fail("yanked must be true or false");
            else v->yanked = y->boolean;
        }
    }
    if (!ok) *why = r.why;
    return ok;
}

bool ReadPlugin(const Value& o, Plugin* p, std::string* why) {
    Reader r;
    if (!o.IsObject()) {
        *why = "not an object";
        return false;
    }
    bool ok = r.Text(o, "id", &p->id, 64, true) && r.Text(o, "name", &p->name, 80, true) &&
              r.Text(o, "description", &p->description, 400, false, true) && r.Text(o, "homepage", &p->homepage, 200, false) &&
              r.Text(o, "licence", &p->licence, 64, false) && r.Strings(o, "authors", &p->authors, 16, 80) &&
              r.Strings(o, "categories", &p->categories, 3, 32) && r.Strings(o, "gameBuilds", &p->gameBuilds, 8, 16);
    if (ok && !spice::ValidModId(p->id)) ok = r.Fail("id is not a valid mod id");
    if (ok && p->name.empty()) ok = r.Fail("name is empty");
    if (ok && !p->homepage.empty() && SchemeOf(p->homepage) != Scheme::Https) ok = r.Fail("homepage must be https://");
    if (ok) {
        if (const Value* s = o.Get("screenshots"); s && !s->IsNull()) {
            if (!s->IsArray() || s->items.size() > kMaxShots) {
                ok = r.Fail("screenshots must be a list of at most 6");
            } else {
                for (const Value& e : s->items) {
                    Shot sh;
                    if (!e.IsObject() || !r.Text(e, "path", &sh.path, 200, true) || !r.Text(e, "sha256", &sh.sha256, 64, true) ||
                        !r.Text(e, "caption", &sh.caption, 120, false) || !r.Size(e, "size", &sh.size, kMaxShotBytes) ||
                        !IsHex64(sh.sha256)) {
                        ok = r.Fail("a screenshot entry is invalid");
                        break;
                    }
                    p->screenshots.push_back(std::move(sh));
                }
            }
        }
    }
    if (!ok) {
        *why = r.why;
        return false;
    }
    const Value* vs = o.Get("versions");
    if (!vs || !vs->IsArray()) {
        *why = "versions must be a list";
        return false;
    }
    if (vs->items.size() > kMaxVersions) {
        *why = "more than 50 versions";
        return false;
    }
    std::set<std::string> seen;
    for (const Value& e : vs->items) {
        Version v;
        std::string vw;
        if (!ReadVersion(e, &v, &vw)) continue;
        if (!seen.insert(v.version).second) continue;
        p->versions.push_back(std::move(v));
    }
    if (p->versions.empty()) {
        *why = "no valid version";
        return false;
    }
    std::stable_sort(p->versions.begin(), p->versions.end(),
                     [](const Version& a, const Version& b) { return spice::SemverCompare(a.version, b.version) > 0; });
    return true;
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool SafeRelative(std::string_view ref) {
    if (ref.empty() || ref.size() > 400 || ref.front() == '/') return false;
    size_t p = 0;
    while (p <= ref.size()) {
        size_t q = ref.find('/', p);
        if (q == std::string_view::npos) q = ref.size();
        std::string_view seg = ref.substr(p, q - p);
        if (seg.empty() || seg == "." || seg == "..") return false;
        for (char c : seg)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') return false;
        p = q + 1;
    }
    return true;
}
}  // namespace

const char* const kRollbackText = "This list is older than one seen before; updates are disabled";

bool ParseIndex(std::string_view text, Index* out, std::string* err) {
    *out = Index{};
    if (text.size() > kMaxIndexBytes) {
        *err = "the list is larger than 1 MiB";
        return false;
    }
    Value root;
    json::Error je;
    if (!json::Parse(text, &root, &je, kMaxIndexBytes)) {
        *err = "the list is not valid JSON (" + std::to_string(je.line) + ":" + std::to_string(je.col) + " " + je.text + ")";
        return false;
    }
    if (!root.IsObject()) {
        *err = "the list is not a JSON object";
        return false;
    }
    const Value* ver = root.Get("indexVersion");
    if (!ver || !ver->IsInteger() || ver->number != 1) {
        *err = "unsupported indexVersion (this Melange reads version 1)";
        return false;
    }
    const Value* serial = root.Get("serial");
    if (!serial || !serial->IsInteger() || serial->number < 0) {
        *err = "serial must be a non-negative integer";
        return false;
    }
    out->serial = static_cast<long long>(serial->number);
    const Value* plugins = root.Get("plugins");
    if (!plugins || !plugins->IsArray()) {
        *err = "plugins must be a list";
        return false;
    }
    if (plugins->items.size() > kMaxPlugins) {
        *err = "the list has more than 500 plugins";
        return false;
    }
    std::set<std::string> folded;
    for (const Value& e : plugins->items) {
        Plugin p;
        std::string why;
        if (!ReadPlugin(e, &p, &why)) {
            const Value* id = e.IsObject() ? e.Get("id") : nullptr;
            out->skipped.push_back((id && id->IsString() ? id->string.substr(0, 64) : std::string("?")) + ": " + why);
            continue;
        }
        std::string key = p.id;
        std::replace(key.begin(), key.end(), '_', '-');
        if (!folded.insert(key).second) {
            out->skipped.push_back(p.id + ": duplicate id");
            continue;
        }
        out->plugins.push_back(std::move(p));
    }
    std::sort(out->plugins.begin(), out->plugins.end(), [](const Plugin& a, const Plugin& b) { return a.id < b.id; });
    return true;
}

const Plugin* FindPlugin(const Index& idx, std::string_view id) {
    for (const Plugin& p : idx.plugins)
        if (p.id == id) return &p;
    return nullptr;
}

const Version* FindVersion(const Plugin& p, std::string_view version) {
    for (const Version& v : p.versions)
        if (v.version == version) return &v;
    return nullptr;
}

Scheme SchemeOf(std::string_view url) {
    auto starts = [&](std::string_view pre) {
        return url.size() >= pre.size() && LowerAscii(url.substr(0, pre.size())) == pre;
    };
    if (starts("https://")) return Scheme::Https;
    if (starts("file://")) return Scheme::File;
    return Scheme::Other;
}

bool CheckIndexUrl(std::string_view url, std::string* why) {
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7f || c == '\\') {
            *why = "the URL has spaces, backslashes or control characters";
            return false;
        }
    const Scheme s = SchemeOf(url);
    if (s == Scheme::Https && url.size() > 8 && url[8] != '/') return true;
    std::wstring path;
    if (s == Scheme::File && FileUrlToPath(url, &path)) return true;
    *why = "only https:// and file:/// index URLs are allowed";
    return false;
}

bool ResolveUrl(std::string_view indexUrl, std::string_view ref, std::string* out, std::string* why) {
    for (unsigned char c : ref)
        if (c <= 0x20 || c == 0x7f || c == '\\') {
            *why = "the URL has spaces, backslashes or control characters";
            return false;
        }
    const Scheme base = SchemeOf(indexUrl);
    const Scheme s = SchemeOf(ref);
    if (s == Scheme::Https) {
        if (ref.size() <= 8 || ref[8] == '/') {
            *why = "the URL has no host";
            return false;
        }
        *out = std::string(ref);
        return true;
    }
    if (s == Scheme::File) {
        std::wstring path;
        if (base != Scheme::File) {
            *why = "url scheme not allowed";
            return false;
        }
        if (!FileUrlToPath(ref, &path)) {
            *why = "not a file:/// path";
            return false;
        }
        *out = std::string(ref);
        return true;
    }
    if (ref.find(':') != std::string_view::npos) {
        *why = "url scheme not allowed";
        return false;
    }
    if (!SafeRelative(ref)) {
        *why = "not a plain relative path";
        return false;
    }
    const size_t slash = indexUrl.rfind('/');
    if (slash == std::string_view::npos || base == Scheme::Other) {
        *why = "the index URL has no folder";
        return false;
    }
    *out = std::string(indexUrl.substr(0, slash + 1)) + std::string(ref);
    return true;
}

bool FileUrlToPath(std::string_view url, std::wstring* path) {
    if (SchemeOf(url) != Scheme::File || url.size() < 11 || url.substr(7, 1) != "/") return false;
    std::string_view rest = url.substr(8);   // C:/...
    if (!std::isalpha(static_cast<unsigned char>(rest[0])) || rest[1] != ':' || rest[2] != '/') return false;
    std::string bytes;
    for (size_t i = 0; i < rest.size(); ++i) {
        char c = rest[i];
        if (c == '%') {
            if (i + 2 >= rest.size() || Hex(rest[i + 1]) < 0 || Hex(rest[i + 2]) < 0) return false;
            c = static_cast<char>(Hex(rest[i + 1]) * 16 + Hex(rest[i + 2]));
            i += 2;
        }
        if (c == '?' || c == '#' || static_cast<unsigned char>(c) < 0x20 || c == '\\') return false;
        bytes.push_back(c == '/' ? '\\' : c);
    }
    size_t p = 3;
    while (p <= bytes.size()) {
        size_t q = bytes.find('\\', p);
        if (q == std::string::npos) q = bytes.size();
        std::string_view seg(bytes.data() + p, q - p);
        if (seg == "." || seg == ".." || (seg.empty() && q != bytes.size())) return false;
        p = q + 1;
    }
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0) return false;
    path->assign(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), path->data(), n);
    return true;
}

const char* ActionName(Action a) {
    switch (a) {
        case Action::Install: return "install";
        case Action::Update: return "update";
        case Action::Remove: return "remove";
        case Action::None: break;
    }
    return "none";
}

bool Compatible(const Version& v, const Plugin& p, const Env& env, std::string* why) {
    if (env.gameBuild.empty()) {
        if (why) *why = "this game build is not recognised";
        return false;
    }
    if (std::find(p.gameBuilds.begin(), p.gameBuilds.end(), env.gameBuild) == p.gameBuilds.end()) {
        if (why) *why = "not tested on this game build";
        return false;
    }
    if (!spice::SemverSatisfies(env.melange, v.melange)) {
        if (why) *why = "needs Melange " + v.melange;
        return false;
    }
    return true;
}

Choice Choose(const Plugin& p, const Have& have, const Env& env) {
    Choice c;
    std::string incompatible;
    for (const Version& v : p.versions) {
        if (v.yanked) continue;
        std::string why;
        if (Compatible(v, p, env, &why)) {
            c.compatible = &v;
            break;
        }
        if (incompatible.empty()) incompatible = why;
    }
    if (have.pending) {
        c.state = "pending";
        c.reason = "Applies at the next launch";
        return c;
    }
    const bool offer = c.compatible && !env.rollback;
    if (!have.present) {
        if (offer) {
            c.action = Action::Install;
        } else if (env.rollback) {
            c.reason = kRollbackText;
        } else {
            c.state = "incompatible";
            c.reason = "Incompatible: " + (incompatible.empty() ? std::string("every version was withdrawn") : incompatible);
        }
        return c;
    }
    const bool newer = offer && spice::ValidSemver(have.version) && spice::SemverCompare(c.compatible->version, have.version) > 0;
    if (!have.managed) {
        c.state = "manual";
        c.reason = "Installed manually (version " + have.version + ")";
        if (offer) c.action = newer || !spice::ValidSemver(have.version) ? Action::Update : Action::Install;
        return c;
    }
    c.canRemove = true;
    c.action = newer ? Action::Update : Action::Remove;
    c.state = newer ? "update" : "installed";
    if (const Version* mine = FindVersion(p, have.version); mine && mine->yanked) {
        c.state = "yanked";
        c.reason = "Version " + have.version + " was withdrawn";
    } else if (env.rollback) {
        c.reason = kRollbackText;
    }
    return c;
}

bool PlanInstall(const Index& idx, const std::string& id, const std::string& version,
                 const std::map<std::string, std::string>& installed, const Env& env, std::vector<Step>* steps,
                 std::string* err) {
    std::set<std::string> visiting;
    auto planned = [&](const std::string& d) {
        for (const Step& s : *steps)
            if (s.id == d) return &s;
        return static_cast<const Step*>(nullptr);
    };
    auto visit = [&](auto&& self, const std::string& pid, const std::string& pver) -> bool {
        if (!visiting.insert(pid).second || visiting.size() > 16) {
            *err = "the dependencies of " + pid + " form a loop";
            return false;
        }
        const Plugin* p = FindPlugin(idx, pid);
        const Version* v = p ? FindVersion(*p, pver) : nullptr;
        if (!v) {
            *err = pid + " " + pver + " is not in the list";
            return false;
        }
        for (const Dep& d : v->dependencies) {
            auto it = installed.find(d.id);
            if (it != installed.end() && spice::SemverSatisfies(it->second, d.range)) continue;
            if (const Step* s = planned(d.id); s && spice::SemverSatisfies(s->version, d.range)) continue;
            const Plugin* dp = FindPlugin(idx, d.id);
            const Version* pick = nullptr;
            if (dp)
                for (const Version& dv : dp->versions)
                    if (!dv.yanked && Compatible(dv, *dp, env, nullptr) && spice::SemverSatisfies(dv.version, d.range)) {
                        pick = &dv;
                        break;
                    }
            if (!pick) {
                *err = pid + " needs " + d.id + (d.range.empty() ? "" : " " + d.range) + ", which is neither installed nor available";
                return false;
            }
            if (!self(self, d.id, pick->version)) return false;
        }
        visiting.erase(pid);
        if (!planned(pid)) steps->push_back({pid, pver});
        return true;
    };
    steps->clear();
    return visit(visit, id, version);
}

bool Matches(const Plugin& p, std::string_view query) {
    if (query.empty()) return true;
    const std::string q = LowerAscii(query);
    auto has = [&](std::string_view s) { return LowerAscii(s).find(q) != std::string::npos; };
    if (has(p.name) || has(p.id) || has(p.description)) return true;
    for (const std::string& a : p.authors)
        if (has(a)) return true;
    return false;
}
}  // namespace melange::store
