#include "erg/service.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>

#include "erg/build.h"
#include "erg/jsonio.h"
#include "erg/load.h"
#include "erg/names.h"
#include "erg/objects.h"
#include "erg/pack.h"
#include "erg/patch.h"
#include "erg/preview.h"
#include "erg/project.h"
#include "erg/voxels.h"
#include "erg/xomutil.h"
#include "levels/manifest.h"
#include "mods/spice.h"
#include "tools/hash.h"
#include "xom/json.h"

namespace melange::erg::service {
namespace {
using jsonio::Int;
using jsonio::Str;
using xom::Json;

constexpr size_t kMaxCached = 4;
constexpr uint64_t kMaxCachedBytes = 160u << 20;   // estimated parsed size of the cached bases
constexpr uint64_t kMaxBlobBytes = 16u << 20;
constexpr int64_t kMaxRef = 1 << 24;
const PatchRules kRules{.voxels = voxels::kAccepted, .anyFrame = true, .objects = true, .script = true, .hmpPaint = true};   // adds may target any frame; newFrames stays off

// A level script's problems for the editor: the line of the first byte CheckSimText refuses (1 for the size limit).
Json ScriptProblems(const std::string& text) {
    Json out = Json::Arr();
    std::string why;
    if (levels::manifest::CheckSimText(text, &why)) return out;
    int line = 1;
    if (text.size() <= levels::manifest::kMaxSimBytes)
        for (size_t start = 0, n = 1;; ++n) {
            const size_t end = std::min(text.find('\n', start), text.size());
            std::string w;
            if (!levels::manifest::CheckSimText(std::string_view(text).substr(start, end - start), &w)) {
                line = static_cast<int>(n);
                why = std::move(w);
                break;
            }
            if (end == text.size()) break;
            start = end + 1;
        }
    Json p = Json::Obj();
    p.set("line", Int(line));
    p.set("message", Str(why));
    out.arr.push_back(std::move(p));
    return out;
}

ScriptMeta MetaOf(const std::string& script) {
    ScriptMeta m;
    m.present = !script.empty();
    if (m.present) m.sha256 = hashutil::Sha256Hex(script.data(), script.size());
    return m;
}

Reply Err(int code, std::string msg) {
    Reply r;
    r.ok = false;
    r.code = code;
    r.message = std::move(msg);
    return r;
}

Reply Ok(const Json& v) {
    Reply r;
    r.json = jsonio::Compact(v);
    return r;
}

Json Parsed(const std::string& text) {
    Json v;
    xom::ParseJson(text, v);
    return v;
}

bool GetStr(const Json& p, const char* key, std::string* out, Reply* r, bool required = true) {
    const Json* v = p.find(key);
    if (!v || v->kind == Json::Kind::Null) {
        if (required) *r = Err(kBadParams, std::string(key) + " is required");
        return !required;
    }
    if (v->kind != Json::Kind::String) {
        *r = Err(kBadParams, std::string(key) + " must be a string");
        return false;
    }
    *out = v->str;
    return true;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(sep, i);
        if (j == std::string::npos) j = s.size();
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

bool ValidModId(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    auto edge = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!edge(s.front()) || !edge(s.back())) return false;
    return std::all_of(s.begin(), s.end(), [&](char c) { return edge(c) || c == '_' || c == '-'; });
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring Slashes(std::string rel) {
    std::replace(rel.begin(), rel.end(), '/', '\\');
    return install::Widen(rel);
}

using Resolved = BaseSource;

std::string InSession(const std::string& modId) {
    return "'" + modId + "' is loaded in this game and you are in a lobby; leave the lobby to change its files";
}

void DeleteStale(const std::wstring& path) {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(path), ec);
}

struct Cached {
    std::string key;
    Sha256Set sha;
    std::shared_ptr<const load::Loaded> loaded;
    uint64_t used = 0;
    uint64_t bytes = 0;
};

struct PaletteEntry { std::string name, resource, role; };

// A pack base's per-map material file travels with the level as Maps\<stem>.txt; a game base's resolves from Data.
build::Options Options(const load::Loaded& L, const BaseSource& res, const Scene& scene) {
    build::Options opt;
    const std::string& mf = scene.databank.materialFile;
    if (!res.packRoot.empty() && mf == L.scene.databank.materialFile && Lower(mf).rfind("maps\\", 0) == 0) {
        std::vector<uint8_t> txt;
        if (install::ReadFile(res.packRoot + L"\\" + Slashes(mf), 1u << 20, &txt, nullptr)) opt.materialTxt = std::move(txt);
    }
    return opt;
}
}  // namespace

bool ReadBase(const std::wstring& gameDir, const std::vector<install::RegistryEntry>& registry,
              const std::map<std::string, std::string>& strings, const std::vector<install::Pack>& packs,
              const std::string& key, const std::string& source, BaseSource* out, int* code, std::string* err) {
    auto fail = [&](int c, std::string why) {
        if (code) *code = c;
        if (err) *err = std::move(why);
        return false;
    };
    std::string e;
    auto readHmp = [&](const std::wstring& path, load::BaseFiles& f) {
        if (!install::Exists(path)) return true;
        std::vector<uint8_t> h;
        if (!install::ReadFile(path, load::kHmpBytes, &h, &e)) return false;
        f.hmp = std::move(h);
        return true;
    };
    if (source.empty() || source == "game") {
        const install::RegistryEntry* entry = nullptr;
        for (const auto& r : registry)
            if (r.key == key && r.levelType == 0 && install::ValidFileStem(r.file)) entry = &r;
        if (entry) {
            const std::wstring data = install::DataDir(gameDir), stem = install::Widen(entry->file);
            load::BaseFiles& f = out->files;
            f.key = key;
            f.source = "game";
            f.file = entry->file;
            auto t = strings.find(entry->frontendName);
            f.title = t != strings.end() ? t->second : key;
            f.registry.levelType = entry->levelType;
            f.registry.themeType = entry->themeType;
            f.registry.previewType = entry->previewType;
            f.registry.scripts = Split(entry->scripts, ',');
            if (f.registry.scripts.empty() || f.registry.scripts.size() > 8) f.registry.scripts = {"stdvs", "wormpot"};
            out->packRoot.clear();
            if (!install::ReadFile(data + L"\\Maps\\" + stem + L".xan", load::kMaxXanBytes, &f.xan, &e) ||
                !install::ReadFile(data + L"\\" + stem + L".XOM", load::kMaxXomBytes, &f.xom, &e) ||
                !readHmp(data + L"\\Maps\\" + stem + L".hmp", f))
                return fail(kPolicy, "base '" + key + "': " + e);
            return true;
        }
        if (source == "game") return fail(kBadParams, "no multiplayer level '" + key + "' in the install");
    }
    if (source.empty() || source == "pack") {
        for (const auto& p : packs)
            for (const auto& d : p.levels) {
                if (names::Key(d.stem) != key) continue;
                load::BaseFiles& f = out->files;
                f.key = key;
                f.source = "pack";
                f.file = d.stem;
                f.title = d.title;
                f.registry = RegistryInfo{};
                f.registry.scripts = levels::manifest::Scripts(d);
                out->packRoot = install::LevelRoot(p);
                const std::wstring stem = install::Widen(d.stem);
                if (!install::ReadFile(out->packRoot + L"\\Maps\\" + stem + L".xan", load::kMaxXanBytes, &f.xan, &e) ||
                    !install::ReadFile(out->packRoot + L"\\" + stem + L".XOM", load::kMaxXomBytes, &f.xom, &e) ||
                    !readHmp(out->packRoot + L"\\Maps\\" + stem + L".hmp", f))
                    return fail(kPolicy, "base '" + key + "' (" + p.modId + "): " + e);
                return true;
            }
    }
    return fail(kBadParams, "no base level '" + key + "'");
}

bool BuildPatch(const std::wstring& gameDir, const std::vector<install::Pack>& packs, const Patch& p, const std::string& stem,
                std::vector<build::File>* out, int* code, std::string* err) {
    std::vector<install::RegistryEntry> registry;
    std::string e;
    if (p.base.source == "game" && !install::ReadRegistry(gameDir, &registry, &e)) {
        if (code) *code = kPolicy;
        if (err) *err = "the install's registry: " + e;
        return false;
    }
    BaseSource res;
    if (!ReadBase(gameDir, registry, {}, packs, p.base.key, p.base.source, &res, code, err)) return false;
    load::Loaded L;
    Scene scene;
    build::VoxelEdits voxels;
    if (!load::LoadScene(res.files, &L, &e)) {
        if (code) *code = kPolicy;
        if (err) *err = "base '" + p.base.key + "': " + e;
        return false;
    }
    if (!build::Apply(L, p, kRules, &scene, &voxels, &e)) {
        const bool policy = e.rfind("base.sha256", 0) == 0 || e.rfind("base:", 0) == 0;
        if (code) *code = policy ? kPolicy : kBadParams;
        if (err) *err = policy ? e + " (the base level no longer matches the patch)" : e;
        return false;
    }
    if (!stem.empty()) scene.stem = stem;
    if (!build::Build(L, scene, voxels, Options(L, res, scene), out, &e)) {
        if (code) *code = kBadParams;
        if (err) *err = "build: " + e;
        return false;
    }
    return true;
}

const std::vector<std::string>& Methods() {
    static const std::vector<std::string> m = {"level.list",   "level.new",   "level.load",    "level.save",  "level.export",
                                               "level.build",  "level.themes", "level.palette", "level.close",  "level.objects",
                                               "level.script.get", "level.script.put"};
    return m;
}

bool Mutating(std::string_view method) {
    return method == "level.new" || method == "level.save" || method == "level.export" || method == "level.build" ||
           method == "level.script.put";
}

struct Service::Impl {
    Env env;
    project::Store store;
    std::mutex mx;
    bool installRead = false;
    std::vector<install::RegistryEntry> registry;
    std::map<std::string, std::string> strings, themeOfFile;
    std::vector<Cached> cache;
    uint64_t tick = 0;
    int64_t refBase = 0;
    std::map<std::string, std::vector<PaletteEntry>> scenery;
    bool catalogRead = false;
    objects::Catalog catalog;
    std::string catalogErr;

    explicit Impl(Env e) : env(std::move(e)), store(env.projectsDir) {}

    void ReadInstall() {
        if (installRead) return;
        installRead = true;
        std::string err;
        install::ReadRegistry(env.gameDir, &registry, &err);
        strings = install::ReadFrontendStrings(env.gameDir);
    }

    std::vector<install::Pack> Packs() { return env.packs ? env.packs() : std::vector<install::Pack>{}; }

    std::string ThemeOf(const std::wstring& xomPath, const std::string& cacheKey) {
        if (auto it = themeOfFile.find(cacheKey); it != themeOfFile.end()) return it->second;
        std::string theme;
        std::vector<uint8_t> b;
        xom::Document d;
        xom::ParseOptions opt;
        opt.strict = true;
        if (install::ReadFile(xomPath, 1u << 20, &b, nullptr) && xom::parse(b.data(), b.size(), d, nullptr, opt))
            for (const auto& o : d.objects)
                if (o.type == "XStringResourceDetails" && xomutil::Str(o, "Name") == "Databank.Theme") theme = xomutil::Str(o, "Value");
        return themeOfFile[cacheKey] = theme;
    }

    bool Resolve(const std::string& key, const std::string& source, Resolved* out, Reply* r) {
        ReadInstall();
        int code = 0;
        std::string err;
        if (ReadBase(env.gameDir, registry, strings, Packs(), key, source, out, &code, &err)) return true;
        *r = Err(code, err);
        return false;
    }

    std::shared_ptr<const load::Loaded> Load(const std::string& key, const std::string& source, Resolved* res, Reply* r) {
        if (!Resolve(key, source, res, r)) return nullptr;
        const load::BaseFiles& f = res->files;
        const Sha256Set sha{load::Sha256(f.xan), load::Sha256(f.xom), f.hmp ? load::Sha256(*f.hmp) : std::string()};
        for (auto& c : cache)
            if (c.key == key && c.loaded->scene.base.source == f.source && c.sha.xan == sha.xan && c.sha.xom == sha.xom &&
                c.sha.hmp == sha.hmp) {
                c.used = ++tick;
                return c.loaded;
            }
        auto L = std::make_shared<load::Loaded>();
        std::string err;
        if (!load::LoadScene(f, L.get(), &err)) {
            *r = Err(kPolicy, "base '" + key + "': " + err);
            return nullptr;
        }
        uint64_t blobBytes = 0;
        for (const auto& [ref, b] : L->blobs) blobBytes += b.size();
        if (blobBytes > kMaxBlobBytes) {
            *r = Err(kPolicy, "base '" + key + "': the level's voxel data is larger than 16 MB");
            return nullptr;
        }
        const uint64_t bytes = 2 * static_cast<uint64_t>(f.xan.size()) + f.xom.size() + blobBytes;
        std::erase_if(cache, [&](const Cached& c) { return c.key == key; });
        auto total = [&] {
            uint64_t t = 0;
            for (const auto& c : cache) t += c.bytes;
            return t;
        };
        while (!cache.empty() && (cache.size() >= kMaxCached || total() + bytes > kMaxCachedBytes))
            cache.erase(std::min_element(cache.begin(), cache.end(), [](const Cached& a, const Cached& b) { return a.used < b.used; }));
        cache.push_back({key, sha, L, ++tick, bytes});
        return L;
    }

    bool LockProject(const std::string& id, uint64_t conn, Reply* r) {
        if (!project::ValidId(id)) {
            *r = Err(kBadParams, "project must be [a-z0-9]{1,24}");
            return false;
        }
        switch (store.Lock(id, conn)) {
            case project::LockResult::Ok: return true;
            case project::LockResult::Busy: *r = Err(kBusy, "project '" + id + "' is open in another Oasis server"); return false;
            case project::LockResult::Missing: *r = Err(kBadParams, "no project '" + id + "'"); return false;
            default: *r = Err(kPolicy, "project '" + id + "' could not be locked"); return false;
        }
    }

    // A project's saved patch against its base: the loaded base, the parsed patch and the edited scene.
    bool OpenProject(const std::string& id, std::shared_ptr<const load::Loaded>* L, Patch* p, Scene* scene,
                     build::VoxelEdits* voxels, Resolved* res, Reply* r, bool lock = true, uint64_t conn = 0) {
        if (lock && !LockProject(id, conn, r)) return false;
        if (!lock && !project::ValidId(id)) {
            *r = Err(kBadParams, "project must be [a-z0-9]{1,24}");
            return false;
        }
        std::string text, err;
        if (!store.ReadPatch(id, &text, &err)) {
            *r = Err(kPolicy, err);
            return false;
        }
        if (!ParsePatch(text, p, &err)) {
            *r = Err(kPolicy, "project '" + id + "' holds an invalid patch: " + err);
            return false;
        }
        *L = Load(p->base.key, p->base.source, res, r);
        if (!*L) return false;
        return ApplyTo(**L, *p, scene, voxels, r);
    }

    bool ApplyTo(const load::Loaded& L, const Patch& p, Scene* scene, build::VoxelEdits* voxels, Reply* r) {
        std::string err;
        if (!build::Apply(L, p, kRules, scene, voxels, &err)) {
            const bool policy = err.rfind("base.sha256", 0) == 0 || err.rfind("base:", 0) == 0;
            *r = Err(policy ? kPolicy : kBadParams, policy ? err + " (the base level no longer matches the patch)" : err);
            return false;
        }
        if (!scene->databank.materialFile.empty() && scene->databank.materialFile != L.scene.databank.materialFile &&
            !install::MaterialFileExists(env.gameDir, scene->databank.materialFile)) {
            *r = Err(kBadParams, "databank.materialFile: '" + scene->databank.materialFile + "' is not in the install");
            return false;
        }
        return true;
    }

    const objects::Catalog* Catalog() {
        if (!catalogRead) {
            catalogRead = true;
            if (!objects::LoadCatalog(std::filesystem::path(env.gameDir), &catalog, &catalogErr)) catalog = {};
        }
        return catalogErr.empty() ? &catalog : nullptr;
    }

    // Crate contents the install does not know; checked only when its weapon table can be read.
    bool CheckObjects(const Scene& scene, std::vector<std::string>* warnings, std::string* err) {
        if (scene.objects.empty()) return true;
        const objects::Catalog* c = Catalog();
        return !c || objects::Validate(scene, *c, warnings, err);
    }

    bool BuildFiles(const load::Loaded& L, const Resolved& res, const Scene& scene, const build::VoxelEdits& voxels,
                    std::vector<build::File>* files, Reply* r, bool checkObjects = true) {
        std::string err;
        if (checkObjects && !CheckObjects(scene, nullptr, &err)) {
            *r = Err(kBadParams, err);
            return false;
        }
        if (!build::Build(L, scene, voxels, Options(L, res, scene), files, &err)) {
            *r = Err(kBadParams, "build: " + err);
            return false;
        }
        for (const auto& f : *files) {
            const std::string name = f.rel.substr(f.rel.find_last_of('/') + 1);
            if (env.crcCollides && env.crcCollides(name)) {
                *r = Err(kPolicy, "'" + name + "' has the name of a CRC-checked game file");
                return false;
            }
        }
        return true;
    }

    Json ProjectJson(const project::Info& i) {
        Json o = Json::Obj();
        o.set("id", Str(i.id));
        o.set("title", Str(i.title));
        o.set("stem", Str(i.stem));
        o.set("base", Str(i.base));
        o.set("modified", Str(i.modified));
        o.set("built", Json::Bool(i.built));
        return o;
    }

    project::Info InfoOf(const std::string& id) {
        for (auto& i : store.List())
            if (i.id == id) return i;
        return {};
    }

    // ---------------------------------------------------------------- methods
    Reply List() {
        ReadInstall();
        Json bases = Json::Arr();
        const std::wstring data = install::DataDir(env.gameDir);
        for (const auto& e : registry) {
            if (e.levelType != 0 || !install::ValidFileStem(e.file) || !install::Exists(data + L"\\Maps\\" + install::Widen(e.file) + L".xan"))
                continue;
            Json o = Json::Obj();
            o.set("key", Str(e.key));
            o.set("stem", Str(e.file));
            auto t = strings.find(e.frontendName);
            o.set("title", Str(t != strings.end() ? t->second : e.key));
            o.set("source", Str("game"));
            o.set("theme", Str(ThemeOf(data + L"\\" + install::Widen(e.file) + L".XOM", "game:" + e.file)));
            bases.arr.push_back(std::move(o));
        }
        for (const auto& p : Packs())
            for (const auto& d : p.levels) {
                const std::wstring x = install::LevelRoot(p) + L"\\" + install::Widen(d.stem) + L".XOM";
                Json o = Json::Obj();
                o.set("key", Str(names::Key(d.stem)));
                o.set("stem", Str(d.stem));
                o.set("title", Str(d.title));
                o.set("source", Str("pack"));
                o.set("theme", Str(install::Exists(x) ? ThemeOf(x, "pack:" + d.stem) : ""));
                o.set("mod", Str(p.modId));
                o.set("built", Json::Bool(install::Exists(install::LevelRoot(p) + L"\\Maps\\" + install::Widen(d.stem) + L".xan")));
                bases.arr.push_back(std::move(o));
            }
        Json projects = Json::Arr();
        for (const auto& i : store.List()) projects.arr.push_back(ProjectJson(i));
        Json out = Json::Obj();
        out.set("bases", std::move(bases));
        out.set("projects", std::move(projects));
        return Ok(out);
    }

    Reply New(const Json& p) {
        Reply r;
        std::string base, slug, title, source;
        if (!GetStr(p, "base", &base, &r) || !GetStr(p, "slug", &slug, &r) || !GetStr(p, "title", &title, &r) ||
            !GetStr(p, "source", &source, &r, false))
            return r;
        if (!project::ValidId(slug)) return Err(kBadParams, "slug must be [a-z0-9]{1,24}");
        if (!PrintableAscii(title, 1, 40)) return Err(kBadParams, "title must be 1-40 printable ASCII characters");
        Resolved res;
        auto L = Load(base, source, &res, &r);
        if (!L) return r;
        const std::string id = store.FreeId(slug);
        if (id.empty()) return Err(kPolicy, "no free project id for '" + slug + "'");
        Patch patch;
        patch.stem = std::string(names::kTestPrefix) + "_" + id;
        patch.title = title;
        patch.base = L->scene.base;
        patch.base.file.clear();
        patch.hmp = L->scene.hmp;
        const std::string text = WritePatch(patch);
        std::string err;
        project::Meta meta;
        meta.title = title;
        meta.created = project::NowIso();
        if (!store.Create(id, text, meta, &err)) return Err(kPolicy, err);
        Json out = ProjectJson(InfoOf(id));
        out.set("patch", Parsed(text));
        return Ok(out);
    }

    Reply LoadMethod(const Json& p, uint64_t conn) {
        Reply r;
        std::string id, base, source;
        if (!GetStr(p, "project", &id, &r, false) || !GetStr(p, "base", &base, &r, false) || !GetStr(p, "source", &source, &r, false))
            return r;
        if (id.empty() == base.empty()) return Err(kBadParams, "pass exactly one of project and base");
        std::shared_ptr<const load::Loaded> L;
        Scene scene;
        build::VoxelEdits voxels;
        Resolved res;
        if (!id.empty()) {
            Patch patch;
            if (!OpenProject(id, &L, &patch, &scene, &voxels, &res, &r, !(env.readOnly && env.readOnly()), conn)) return r;
        } else {
            L = Load(base, source, &res, &r);
            if (!L) return r;
            scene = L->scene;
            // {surround: true}: the base's .hmp as an hmp blob, the start the editor paints from.
            const Json* sur = p.find("surround");
            if (sur && sur->kind != Json::Kind::Bool) return Err(kBadParams, "surround must be a boolean");
            if (sur && sur->boolean && L->hmp && scene.hmp == HmpMode::Copy) {
                int64_t ref = 0;
                for (const auto& b : scene.blobs) ref = std::max(ref, b.ref + 1);
                scene.hmpRef = ref;
                scene.blobs.push_back({ref, "hmp", 0, kHmpBytes});
                voxels.hmp = *L->hmp;
            }
        }
        uint64_t total = 0;
        int64_t maxRef = 0;
        for (const auto& b : scene.blobs) {
            total += b.bytes;
            maxRef = std::max(maxRef, b.ref);
        }
        if (total > kMaxBlobBytes) return Err(kPolicy, "the level's voxel data is larger than 16 MB");
        // Refs run on across loads, so a frame left over from an earlier load never matches this one's refs.
        if (refBase + maxRef >= kMaxRef) refBase = 0;
        const int64_t shift = refBase;
        refBase += maxRef;
        std::vector<int64_t> original;
        for (auto& b : scene.blobs) {
            original.push_back(b.ref);
            b.ref += shift;
        }
        for (auto& f : scene.frames) {
            if (f.voxels >= 0) f.voxels += shift;
            if (f.heightMap >= 0) f.heightMap += shift;
        }
        if (scene.hmpRef >= 0) scene.hmpRef += shift;
        std::set<std::string> resources;
        for (const auto& d : scene.details) resources.insert(d.resource);
        Json previews = Json::Obj();
        for (const auto& resource : resources) {
            const std::string key = preview::DetailKey(scene.databank.theme, resource);
            if (!key.empty()) previews.set(resource, Str(key));
        }
        Json withPreviews = Parsed(WriteScene(scene));
        withPreviews.set("previews", std::move(previews));
        r.json = jsonio::Compact(withPreviews);
        for (size_t i = 0; i < scene.blobs.size(); ++i) {
            const auto& b = scene.blobs[i];
            Blob out;
            out.ref = static_cast<uint32_t>(b.ref);
            Json meta = Json::Obj();
            meta.set("ref", Int(b.ref));
            meta.set("kind", Str(b.kind));
            meta.set("frame", Int(b.frame));
            out.meta = jsonio::Compact(meta);
            auto ed = b.kind == "voxels" ? voxels.find(b.frame) : voxels.end();
            if (b.kind == "hmp") {
                if (!voxels.hmp) return Err(kPolicy, "the surround's blob is missing");
                out.bytes.assign(voxels.hmp->begin(), voxels.hmp->end());
            } else if (ed != voxels.end()) {
                out.bytes.resize(ed->second.size() * 4);
                for (size_t k2 = 0; k2 < ed->second.size(); ++k2)
                    for (int k = 0; k < 4; ++k) out.bytes[k2 * 4 + k] = static_cast<char>(ed->second[k2] >> (8 * k));
            } else {
                const auto& src = L->blobs.at(original[i]);
                out.bytes.assign(src.begin(), src.end());
            }
            r.blobs.push_back(std::move(out));
        }
        return r;
    }

    Reply Save(const Json& p, uint64_t conn) {
        Reply r;
        std::string id;
        if (!GetStr(p, "project", &id, &r)) return r;
        const Json* pj = p.find("patch");
        if (!pj || pj->kind != Json::Kind::Object) return Err(kBadParams, "patch must be an erg-patch/1 object");
        if (!LockProject(id, conn, &r)) return r;
        Patch patch, saved;
        std::string err, text;
        if (!ParsePatch(jsonio::Compact(*pj), &patch, &err)) return Err(kBadParams, err);
        if (!store.ReadPatch(id, &text, &err) || !ParsePatch(text, &saved, &err)) return Err(kPolicy, "project '" + id + "': " + err);
        if (patch.stem != saved.stem) return Err(kBadParams, "patch.stem: must stay '" + saved.stem + "'");
        if (patch.base.key != saved.base.key || patch.base.source != saved.base.source)
            return Err(kBadParams, "base: the patch is for another level than the project");
        std::string script;
        if (!ProjectScript(id, &script, &r)) return r;
        patch.script = MetaOf(script);   // script.lua is saved by level.script.put, never by the patch
        Resolved res;
        auto L = Load(patch.base.key, patch.base.source, &res, &r);
        if (!L) return r;
        Scene scene;
        build::VoxelEdits voxels;
        if (!ApplyTo(*L, patch, &scene, &voxels, &r)) return r;
        std::vector<build::File> files;
        if (!BuildFiles(*L, res, scene, voxels, &files, &r, false)) return r;
        const std::string canon = WritePatch(patch);
        if (!store.WritePatch(id, canon, &err)) return Err(kPolicy, err);
        project::Meta meta;
        store.ReadMeta(id, &meta);
        if (meta.title != patch.title) {
            meta.title = patch.title;
            store.WriteMeta(id, meta, nullptr);
        }
        Json warnings = Json::Arr();
        if (patch.spawns == SpawnMode::Knots)
            for (int i = 0; i < 8; ++i) {
                const std::string k = "WORM" + std::to_string(i);
                if (std::none_of(scene.details.begin(), scene.details.end(), [&](const Detail& d) { return d.name == k; }))
                    warnings.arr.push_back(Str("spawns.mode is knots but there is no detail named " + k + "; export will refuse"));
            }
        std::vector<std::string> notes;
        if (!CheckObjects(scene, &notes, &err)) notes.push_back(err + "; export will refuse");
        for (auto& n : notes) warnings.arr.push_back(Str(n));
        Json out = Json::Obj();
        out.set("saved", Json::Bool(true));
        out.set("warnings", std::move(warnings));
        out.set("modified", Str(InfoOf(id).modified));
        return Ok(out);
    }

    // The project's script.lua, checked as a pack's level script is ("" when it has none).
    bool ProjectScript(const std::string& id, std::string* text, Reply* r) {
        std::string err;
        if (!store.ReadScript(id, text, &err)) {
            *r = Err(kPolicy, "project '" + id + "': script.lua: " + (err.empty() ? "cannot be read" : err));
            return false;
        }
        if (!levels::manifest::CheckSimText(*text, &err)) {
            *r = Err(kPolicy, "project '" + id + "': script.lua: " + err);
            return false;
        }
        return true;
    }

    Reply ScriptGet(const Json& p) {
        Reply r;
        std::string id, text, err;
        if (!GetStr(p, "project", &id, &r)) return r;
        if (!project::ValidId(id)) return Err(kBadParams, "project must be [a-z0-9]{1,24}");
        if (!store.Exists(id)) return Err(kBadParams, "no project '" + id + "'");
        if (!store.ReadScript(id, &text, &err)) return Err(kPolicy, "script.lua: " + err);
        Json out = Json::Obj();
        out.set("text", Str(text));
        return Ok(out);
    }

    Reply ScriptPut(const Json& p, uint64_t conn) {
        Reply r;
        std::string id, text, err, patchText;
        if (!GetStr(p, "project", &id, &r) || !GetStr(p, "text", &text, &r)) return r;
        if (!LockProject(id, conn, &r)) return r;
        Json out = Json::Obj();
        Json problems = ScriptProblems(text);
        if (!problems.arr.empty()) {
            out.set("saved", Json::Bool(false));
            out.set("problems", std::move(problems));
            return Ok(out);
        }
        Patch patch;
        if (!store.ReadPatch(id, &patchText, &err) || !ParsePatch(patchText, &patch, &err))
            return Err(kPolicy, "project '" + id + "': " + err);
        if (!store.WriteScript(id, text, &err)) return Err(kPolicy, err);
        patch.script = MetaOf(text);
        if (!store.WritePatch(id, WritePatch(patch), &err)) return Err(kPolicy, err);
        out.set("saved", Json::Bool(true));
        out.set("problems", std::move(problems));
        return Ok(out);
    }

    Reply Close(const Json& p, uint64_t conn) {
        Reply r;
        std::string id;
        if (!GetStr(p, "project", &id, &r)) return r;
        if (!project::ValidId(id)) return Err(kBadParams, "project must be [a-z0-9]{1,24}");
        std::string text;
        Patch patch;
        if (store.Locked(id) && store.ReadPatch(id, &text, nullptr) && ParsePatch(text, &patch, nullptr))
            std::erase_if(cache, [&](const Cached& c) { return c.key == patch.base.key; });
        store.Unlock(id, conn);
        Json out = Json::Obj();
        out.set("closed", Json::Bool(true));
        return Ok(out);
    }

    Reply Themes() {
        static const char* const kThemes[] = {"ARABIAN", "WILDWEST", "CAMELOT", "PREHISTORIC", "BUILDING", "ARCTIC",
                                              "ENGLAND", "HORROR",  "LUNAR",   "PIRATE",      "WAR"};
        Json themes = Json::Arr(), times = Json::Arr(), files = Json::Arr();
        for (auto t : kThemes) themes.arr.push_back(Str(t));
        for (auto t : {"DAY", "EVENING", "NIGHT"}) times.arr.push_back(Str(t));
        for (const auto& f : install::MaterialFiles(env.gameDir)) files.arr.push_back(Str(f));
        Json out = Json::Obj();
        out.set("themes", std::move(themes));
        out.set("timesOfDay", std::move(times));
        out.set("materialFiles", std::move(files));
        return Ok(out);
    }

    const std::vector<PaletteEntry>& Scenery(const std::string& theme) {
        if (auto it = scenery.find(theme); it != scenery.end()) return it->second;
        ReadInstall();
        std::vector<PaletteEntry> out;
        std::set<std::string> seen;
        const std::wstring data = install::DataDir(env.gameDir);
        for (const auto& e : registry) {
            if (e.levelType != 0 || !install::ValidFileStem(e.file)) continue;
            const std::wstring stem = install::Widen(e.file);
            if (ThemeOf(data + L"\\" + stem + L".XOM", "game:" + e.file) != theme) continue;
            std::vector<uint8_t> b;
            xom::Document x;
            xom::ParseOptions opt;
            opt.strict = true;
            if (!install::ReadFile(data + L"\\Maps\\" + stem + L".xan", load::kMaxXanBytes, &b, nullptr) ||
                !xom::parse(b.data(), b.size(), x, nullptr, opt))
                continue;
            for (const auto& o : x.objects) {
                if (o.type != "DetailEntityStore") continue;
                const std::string name = xomutil::Str(o, "Name"), res = xomutil::Str(o, "ResourceName");
                if (DeriveRole(name, res) != Role::Scenery || !PrintableAscii(name, 1, 63) || !PrintableAscii(res, 1, 63)) continue;
                if (seen.insert(Lower(res)).second) out.push_back({name, res, "scenery"});
            }
        }
        std::sort(out.begin(), out.end(), [](const PaletteEntry& a, const PaletteEntry& b) { return Lower(a.resource) < Lower(b.resource); });
        return scenery[theme] = std::move(out);
    }

    Reply Palette(const Json& p) {
        Reply r;
        std::string theme;
        if (!GetStr(p, "theme", &theme, &r)) return r;
        if (!ValidTheme(theme)) return Err(kBadParams, "theme must be one of the eleven themes");
        std::vector<PaletteEntry> entries;
        for (int i = 0; i < 8; ++i) entries.push_back({"WORM" + std::to_string(i), "CheesyGrinWorm", "spawn"});
        entries.push_back({"mine", "Landmine", "object"});
        entries.push_back({"oildrum", "OilDrum", "object"});
        for (const auto& e : Scenery(theme)) entries.push_back(e);
        Json arr = Json::Arr();
        for (const auto& e : entries) {
            Json o = Json::Obj();
            o.set("name", Str(e.name));
            o.set("resource", Str(e.resource));
            o.set("role", Str(e.role));
            const std::string key = preview::DetailKey(theme, e.resource);
            o.set("preview", key.empty() ? Json::Null_() : Str(key));
            arr.arr.push_back(std::move(o));
        }
        Json out = Json::Obj();
        out.set("theme", Str(theme));
        out.set("entries", std::move(arr));
        const std::string atlas = preview::ThemeAtlasKey(theme);
        out.set("atlas", atlas.empty() ? Json::Null_() : Str(atlas));
        return Ok(out);
    }

    Reply Objects() {
        const objects::Catalog* c = Catalog();
        Json kinds = Json::Arr(), weapons = Json::Arr(), utilities = Json::Arr();
        for (auto k : {CrateKind::Weapon, CrateKind::Health, CrateKind::Utility}) kinds.arr.push_back(Str(CrateKindName(k)));
        if (c) {
            for (const auto& n : c->weapons) weapons.arr.push_back(Str(n));
            for (const auto& n : c->utilities) utilities.arr.push_back(Str(n));
        }
        Json limits = Json::Obj();
        limits.set("objects", Int(static_cast<int64_t>(kMaxObjects)));
        limits.set("telepadGroups", Int(static_cast<int64_t>(kMaxTelepadGroups)));
        Json out = Json::Obj();
        out.set("crateKinds", std::move(kinds));
        out.set("weapons", std::move(weapons));
        out.set("utilities", std::move(utilities));
        out.set("limits", std::move(limits));
        out.set("error", c ? Json::Null_() : Str(catalogErr));
        return Ok(out);
    }

    Reply Export(const Json& p) {
        Reply r;
        std::string id, modId, name, version, mode;
        if (!GetStr(p, "project", &id, &r) || !GetStr(p, "modId", &modId, &r) || !GetStr(p, "name", &name, &r) ||
            !GetStr(p, "version", &version, &r) || !GetStr(p, "mode", &mode, &r))
            return r;
        if (mode != "install" && mode != "source") return Err(kBadParams, "mode must be \"install\" or \"source\"");
        std::string why;
        const std::string prefix = names::Prefix(modId);
        if (!ValidModId(modId) || !names::ValidPrefix(prefix, &why))
            return Err(kBadParams, "modId: " + (why.empty() ? "must match ^[a-z0-9](?:[a-z0-9_-]*[a-z0-9])?$" : why));
        if (!PrintableAscii(name, 1, 64) || !PrintableAscii(version, 1, 32)) return Err(kBadParams, "name and version must be printable");
        if (mode == "install" && env.modsReadOnly && env.modsReadOnly())
            return Err(kReadOnly, "the game is running; Mods is read-only here (export as source, or use the in-game Oasis)");
        if (env.inSession && env.inSession(modId)) return Err(kReadOnly, InSession(modId));
        std::shared_ptr<const load::Loaded> L;
        Patch patch;
        Scene scene;
        build::VoxelEdits voxels;
        Resolved res;
        if (!OpenProject(id, &L, &patch, &scene, &voxels, &res, &r)) return r;
        std::string script;
        if (!ProjectScript(id, &script, &r)) return r;
        patch.script = MetaOf(script);
        const std::string stem = prefix + "_" + id;
        if (!names::ValidStem(stem, prefix, &why)) return Err(kBadParams, why);
        patch.stem = stem;
        scene.stem = stem;
        pack::PackSpec spec;
        spec.modId = modId;
        spec.name = name;
        spec.version = version;
        spec.slug = id;
        spec.title = patch.title;
        spec.source = mode == "source";
        spec.chunk = luagen::Needed(scene);
        spec.patchJson = WritePatch(patch);
        spec.script = std::move(script);
        if (!spec.source) {
            std::vector<build::File> files;
            if (!BuildFiles(*L, res, scene, voxels, &files, &r)) return r;
            for (auto& f : files) spec.levelFiles.push_back({"assets/levels/" + f.rel, std::move(f.bytes)});
        }
        const std::wstring dir = env.gameDir + L"\\Mods\\" + install::Widen(modId);
        std::vector<std::string> written;
        std::string err;
        if (!pack::WritePack(spec, dir, &written, &err)) return Err(kPolicy, "export: " + err);
        project::Meta meta;
        store.ReadMeta(id, &meta);
        meta.lastExport = project::NowIso();
        store.WriteMeta(id, meta, nullptr);
        Json files = Json::Arr();
        for (const auto& f : written) files.arr.push_back(Str(f));
        Json out = Json::Obj();
        out.set("dir", Str(install::Narrow(dir)));
        out.set("files", std::move(files));
        out.set("restartRequired", Json::Bool(!(env.modActive && env.modActive(modId))));
        return Ok(out);
    }

    Reply BuildMod(const Json& p) {
        Reply r;
        std::string modId;
        if (!GetStr(p, "modId", &modId, &r)) return r;
        if (!ValidModId(modId)) return Err(kBadParams, "modId: not a mod id");
        if (env.modsReadOnly && env.modsReadOnly()) return Err(kReadOnly, "the game is running; Mods is read-only here");
        if (env.inSession && env.inSession(modId)) return Err(kReadOnly, InSession(modId));
        const std::wstring dir = env.gameDir + L"\\Mods\\" + install::Widen(modId);
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (!spice::Parse(dir, &m, &errs) || m.id != modId)
            return Err(kBadParams, "no mod '" + modId + "' with a valid spice.json" + (errs.empty() ? "" : ": " + errs.front().text));
        install::Pack pack;
        if (!install::PackFromManifest(m, dir, &pack)) return Err(kBadParams, "mod '" + modId + "' declares no valid levels");
        const std::wstring root = install::LevelRoot(pack);
        // Every level is built once to check it, then built again and written one at a time, so only one level's
        // files are held at once and nothing is written unless all of them build.
        auto buildLevel = [&](const levels::manifest::LevelDecl& d, std::vector<build::File>* files, Reply* rr) {
            std::vector<uint8_t> bytes;
            std::string err;
            if (!install::ReadFile(dir + L"\\" + Slashes(d.source), kMaxPatchBytes, &bytes, &err)) {
                *rr = Err(kBadParams, d.slug + ": " + err);
                return false;
            }
            Patch patch;
            if (!ParsePatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &patch, &err)) {
                *rr = Err(kBadParams, d.slug + ": " + err);
                return false;
            }
            if (patch.stem != d.stem) {
                *rr = Err(kBadParams, d.slug + ": the patch's stem must be '" + d.stem + "'");
                return false;
            }
            Resolved res;
            auto L = Load(patch.base.key, patch.base.source, &res, rr);
            if (!L) return false;
            Scene scene;
            build::VoxelEdits voxels;
            if (!ApplyTo(*L, patch, &scene, &voxels, rr) || !BuildFiles(*L, res, scene, voxels, files, rr)) {
                rr->message = d.slug + ": " + rr->message;
                return false;
            }
            if (d.chunk && std::none_of(files->begin(), files->end(), [&](const build::File& f) { return f.rel == d.stem + ".lub"; })) {
                const std::string stub = luagen::Stub(d.stem);
                files->push_back({d.stem + ".lub", std::vector<uint8_t>(stub.begin(), stub.end())});
            }
            return true;
        };
        Json skipped = Json::Arr();
        for (const auto& d : pack.levels) {
            if (d.source.empty()) {
                skipped.arr.push_back(Str(d.slug));
                continue;
            }
            std::vector<build::File> files;
            if (!buildLevel(d, &files, &r)) return r;
        }
        Json levels = Json::Arr();
        for (const auto& d : pack.levels) {
            if (d.source.empty()) continue;
            std::vector<build::File> built;
            if (!buildLevel(d, &built, &r)) return r;
            Json files = Json::Arr();
            for (const auto& f : built) {
                const std::wstring path = root + L"\\" + Slashes(f.rel);
                std::string err;
                const std::wstring folder = path.substr(0, path.find_last_of(L'\\'));
                if (!install::Inside(path, root) || !install::NoReparse(dir, folder) || !install::MakeDirs(folder) ||
                    !install::WriteAtomic(path, f.bytes.data(), f.bytes.size(), &err))
                    return Err(kPolicy, d.slug + ": " + (err.empty() ? "cannot write " + f.rel : err));
                files.arr.push_back(Str(pack.assetsRoot + "/levels/" + f.rel));
            }
            for (const auto& rel : build::Stale(d.stem, built)) {
                const std::wstring path = root + L"\\" + Slashes(rel);
                if (install::Inside(path, root) && install::NoReparse(dir, path.substr(0, path.find_last_of(L'\\'))))
                    DeleteStale(path);
            }
            Json o = Json::Obj();
            o.set("slug", Str(d.slug));
            o.set("stem", Str(d.stem));
            o.set("files", std::move(files));
            levels.arr.push_back(std::move(o));
        }
        Json out = Json::Obj();
        out.set("modId", Str(modId));
        out.set("dir", Str(install::Narrow(dir)));
        out.set("levels", std::move(levels));
        out.set("skipped", std::move(skipped));
        return Ok(out);
    }
};

Service::Service(Env env) : impl_(std::make_unique<Impl>(std::move(env))) {}
Service::~Service() = default;

Reply Service::BuildTest(const std::string& id, const std::wstring& root, const std::string& tod) {
    if (!tod.empty() && !ValidTimeOfDay(tod)) return Err(kBadParams, "tod must be DAY, EVENING or NIGHT");
    std::lock_guard lk(impl_->mx);
    Reply r;
    std::shared_ptr<const load::Loaded> L;
    Patch patch;
    Scene scene;
    build::VoxelEdits voxels;
    Resolved res;
    if (!impl_->OpenProject(id, &L, &patch, &scene, &voxels, &res, &r)) return r;
    const std::string stem = std::string(names::kTestPrefix) + "_" + id;
    scene.stem = stem;
    if (!tod.empty()) scene.databank.timeOfDay = tod;
    std::vector<build::File> files;
    if (!impl_->BuildFiles(*L, res, scene, voxels, &files, &r)) return r;
    if (std::none_of(files.begin(), files.end(), [&](const build::File& f) { return f.rel == stem + ".lub"; })) {
        const std::string none = luagen::Stub(stem);
        files.push_back({stem + ".lub", std::vector<uint8_t>(none.begin(), none.end())});
    }
    for (const auto& rel : build::Stale(stem, files)) DeleteStale(root + L"\\" + Slashes(rel));
    Json list = Json::Arr();
    for (const auto& f : files) {
        const std::wstring path = root + L"\\" + Slashes(f.rel);
        const std::wstring folder = path.substr(0, path.find_last_of(L'\\'));
        std::string err;
        if (!install::Inside(path, root) || !install::MakeDirs(folder) || !install::NoReparse(root, folder) ||
            !install::WriteAtomic(path, f.bytes.data(), f.bytes.size(), &err))
            return Err(kPolicy, "test build: " + (err.empty() ? "cannot write " + f.rel : err));
        list.arr.push_back(Str(f.rel));
    }
    project::Meta meta;
    impl_->store.ReadMeta(id, &meta);
    meta.lastTest = project::NowIso();
    impl_->store.WriteMeta(id, meta, nullptr);
    Json out = Json::Obj();
    out.set("stem", Str(stem));
    out.set("title", Str(patch.title));
    out.set("files", std::move(list));
    return Ok(out);
}

Reply Service::Call(std::string_view method, std::string_view paramsJson, uint64_t conn) {
    Json p;
    std::string perr;
    if (!xom::ParseJson(paramsJson.empty() ? std::string_view("{}") : paramsJson, p, &perr) || p.kind != Json::Kind::Object)
        return Err(kBadParams, "params must be a JSON object");
    std::lock_guard lk(impl_->mx);
    if (method == "level.list") return impl_->List();
    if (method == "level.new") return impl_->New(p);
    if (method == "level.load") return impl_->LoadMethod(p, conn);
    if (method == "level.save") return impl_->Save(p, conn);
    if (method == "level.close") return impl_->Close(p, conn);
    if (method == "level.themes") return impl_->Themes();
    if (method == "level.palette") return impl_->Palette(p);
    if (method == "level.export") return impl_->Export(p);
    if (method == "level.build") return impl_->BuildMod(p);
    if (method == "level.objects") return impl_->Objects();
    if (method == "level.script.get") return impl_->ScriptGet(p);
    if (method == "level.script.put") return impl_->ScriptPut(p, conn);
    return Err(-32601, "unknown method");
}

void Service::ClientClosed(uint64_t conn) {
    std::lock_guard lk(impl_->mx);
    impl_->store.ReleaseConn(conn);
}
}  // namespace melange::erg::service
