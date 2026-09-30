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
#include "erg/pack.h"
#include "erg/patch.h"
#include "erg/preview.h"
#include "erg/project.h"
#include "erg/voxels.h"
#include "erg/xomutil.h"
#include "mods/spice.h"
#include "xom/json.h"

namespace melange::erg::service {
namespace {
using jsonio::Int;
using jsonio::Str;
using xom::Json;

constexpr size_t kMaxCached = 4;
constexpr uint64_t kMaxBlobBytes = 16u << 20;
const PatchRules kRules{voxels::kAccepted, true};   // adds may target any frame

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

struct Cached {
    std::string key;
    Sha256Set sha;
    std::shared_ptr<const load::Loaded> loaded;
    uint64_t used = 0;
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
                out->packRoot = p.dir + L"\\assets\\levels";
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
                                               "level.build",  "level.themes", "level.palette", "level.close"};
    return m;
}

bool Mutating(std::string_view method) {
    return method == "level.new" || method == "level.save" || method == "level.export" || method == "level.build";
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
    std::map<std::string, std::vector<PaletteEntry>> scenery;

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
        std::erase_if(cache, [&](const Cached& c) { return c.key == key; });
        if (cache.size() >= kMaxCached)
            cache.erase(std::min_element(cache.begin(), cache.end(), [](const Cached& a, const Cached& b) { return a.used < b.used; }));
        cache.push_back({key, sha, L, ++tick});
        return L;
    }

    bool LockProject(const std::string& id, Reply* r) {
        if (!project::ValidId(id)) {
            *r = Err(kBadParams, "project must be [a-z0-9]{1,24}");
            return false;
        }
        switch (store.Lock(id)) {
            case project::LockResult::Ok: return true;
            case project::LockResult::Busy: *r = Err(kBusy, "project '" + id + "' is open in another Oasis server"); return false;
            case project::LockResult::Missing: *r = Err(kBadParams, "no project '" + id + "'"); return false;
            default: *r = Err(kPolicy, "project '" + id + "' could not be locked"); return false;
        }
    }

    // A project's saved patch against its base: the loaded base, the parsed patch and the edited scene.
    bool OpenProject(const std::string& id, std::shared_ptr<const load::Loaded>* L, Patch* p, Scene* scene,
                     build::VoxelEdits* voxels, Resolved* res, Reply* r) {
        if (!LockProject(id, r)) return false;
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

    bool BuildFiles(const load::Loaded& L, const Resolved& res, const Scene& scene, const build::VoxelEdits& voxels,
                    std::vector<build::File>* files, Reply* r) {
        std::string err;
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
                const std::wstring x = p.dir + L"\\assets\\levels\\" + install::Widen(d.stem) + L".XOM";
                Json o = Json::Obj();
                o.set("key", Str(names::Key(d.stem)));
                o.set("stem", Str(d.stem));
                o.set("title", Str(d.title));
                o.set("source", Str("pack"));
                o.set("theme", Str(install::Exists(x) ? ThemeOf(x, "pack:" + d.stem) : ""));
                o.set("mod", Str(p.modId));
                o.set("built", Json::Bool(install::Exists(p.dir + L"\\assets\\levels\\Maps\\" + install::Widen(d.stem) + L".xan")));
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

    Reply LoadMethod(const Json& p) {
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
            if (!OpenProject(id, &L, &patch, &scene, &voxels, &res, &r)) return r;
        } else {
            L = Load(base, source, &res, &r);
            if (!L) return r;
            scene = L->scene;
        }
        uint64_t total = 0;
        for (const auto& b : scene.blobs) total += b.bytes;
        if (total > kMaxBlobBytes) return Err(kPolicy, "the level's voxel data is larger than 16 MB");
        r.json = WriteScene(scene);
        for (const auto& b : scene.blobs) {
            Blob out;
            out.ref = static_cast<uint32_t>(b.ref);
            Json meta = Json::Obj();
            meta.set("ref", Int(b.ref));
            meta.set("kind", Str(b.kind));
            meta.set("frame", Int(b.frame));
            out.meta = jsonio::Compact(meta);
            auto ed = b.kind == "voxels" ? voxels.find(b.frame) : voxels.end();
            if (ed != voxels.end()) {
                out.bytes.resize(ed->second.size() * 4);
                for (size_t i = 0; i < ed->second.size(); ++i)
                    for (int k = 0; k < 4; ++k) out.bytes[i * 4 + k] = static_cast<char>(ed->second[i] >> (8 * k));
            } else {
                const auto& src = L->blobs.at(b.ref);
                out.bytes.assign(src.begin(), src.end());
            }
            r.blobs.push_back(std::move(out));
        }
        return r;
    }

    Reply Save(const Json& p) {
        Reply r;
        std::string id;
        if (!GetStr(p, "project", &id, &r)) return r;
        const Json* pj = p.find("patch");
        if (!pj || pj->kind != Json::Kind::Object) return Err(kBadParams, "patch must be an erg-patch/1 object");
        if (!LockProject(id, &r)) return r;
        Patch patch, saved;
        std::string err, text;
        if (!ParsePatch(jsonio::Compact(*pj), &patch, &err)) return Err(kBadParams, err);
        if (!store.ReadPatch(id, &text, &err) || !ParsePatch(text, &saved, &err)) return Err(kPolicy, "project '" + id + "': " + err);
        if (patch.stem != saved.stem) return Err(kBadParams, "patch.stem: must stay '" + saved.stem + "'");
        if (patch.base.key != saved.base.key || patch.base.source != saved.base.source)
            return Err(kBadParams, "base: the patch is for another level than the project");
        Resolved res;
        auto L = Load(patch.base.key, patch.base.source, &res, &r);
        if (!L) return r;
        Scene scene;
        build::VoxelEdits voxels;
        if (!ApplyTo(*L, patch, &scene, &voxels, &r)) return r;
        std::vector<build::File> files;
        if (!BuildFiles(*L, res, scene, voxels, &files, &r)) return r;
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
        Json out = Json::Obj();
        out.set("saved", Json::Bool(true));
        out.set("warnings", std::move(warnings));
        out.set("modified", Str(InfoOf(id).modified));
        return Ok(out);
    }

    Reply Close(const Json& p) {
        Reply r;
        std::string id;
        if (!GetStr(p, "project", &id, &r)) return r;
        if (!project::ValidId(id)) return Err(kBadParams, "project must be [a-z0-9]{1,24}");
        std::string text;
        Patch patch;
        if (store.Locked(id) && store.ReadPatch(id, &text, nullptr) && ParsePatch(text, &patch, nullptr))
            std::erase_if(cache, [&](const Cached& c) { return c.key == patch.base.key; });
        store.Unlock(id);
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
        std::shared_ptr<const load::Loaded> L;
        Patch patch;
        Scene scene;
        build::VoxelEdits voxels;
        Resolved res;
        if (!OpenProject(id, &L, &patch, &scene, &voxels, &res, &r)) return r;
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
        const std::wstring dir = env.gameDir + L"\\Mods\\" + install::Widen(modId);
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (!spice::Parse(dir, &m, &errs) || m.id != modId)
            return Err(kBadParams, "no mod '" + modId + "' with a valid spice.json" + (errs.empty() ? "" : ": " + errs.front().text));
        install::Pack pack;
        if (!install::PackFromManifest(m, dir, &pack)) return Err(kBadParams, "mod '" + modId + "' declares no valid levels");
        const std::wstring root = dir + L"\\assets\\levels";
        struct Planned { const levels::manifest::LevelDecl* decl; std::vector<build::File> files; };
        std::vector<Planned> plan;
        Json skipped = Json::Arr();
        for (const auto& d : pack.levels) {
            if (d.source.empty()) {
                skipped.arr.push_back(Str(d.slug));
                continue;
            }
            std::vector<uint8_t> bytes;
            std::string err;
            if (!install::ReadFile(dir + L"\\" + Slashes(d.source), kMaxPatchBytes, &bytes, &err)) return Err(kBadParams, d.slug + ": " + err);
            Patch patch;
            if (!ParsePatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &patch, &err))
                return Err(kBadParams, d.slug + ": " + err);
            if (patch.stem != d.stem) return Err(kBadParams, d.slug + ": the patch's stem must be '" + d.stem + "'");
            Resolved res;
            auto L = Load(patch.base.key, patch.base.source, &res, &r);
            if (!L) return r;
            Scene scene;
            build::VoxelEdits voxels;
            if (!ApplyTo(*L, patch, &scene, &voxels, &r)) {
                r.message = d.slug + ": " + r.message;
                return r;
            }
            Planned pl{&d, {}};
            if (!BuildFiles(*L, res, scene, voxels, &pl.files, &r)) {
                r.message = d.slug + ": " + r.message;
                return r;
            }
            plan.push_back(std::move(pl));
        }
        Json levels = Json::Arr();
        for (auto& pl : plan) {
            Json files = Json::Arr();
            for (const auto& f : pl.files) {
                const std::wstring path = root + L"\\" + Slashes(f.rel);
                std::string err;
                const std::wstring folder = path.substr(0, path.find_last_of(L'\\'));
                if (!install::Inside(path, root) || !install::NoReparse(dir, folder) || !install::MakeDirs(folder) ||
                    !install::WriteAtomic(path, f.bytes.data(), f.bytes.size(), &err))
                    return Err(kPolicy, pl.decl->slug + ": " + (err.empty() ? "cannot write " + f.rel : err));
                files.arr.push_back(Str("assets/levels/" + f.rel));
            }
            Json o = Json::Obj();
            o.set("slug", Str(pl.decl->slug));
            o.set("stem", Str(pl.decl->stem));
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

Reply Service::BuildTest(const std::string& id, const std::wstring& root) {
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
    std::vector<build::File> files;
    if (!impl_->BuildFiles(*L, res, scene, voxels, &files, &r)) return r;
    if (std::none_of(files.begin(), files.end(), [&](const build::File& f) { return f.rel == stem + ".lub"; })) {
        const std::string none = "-- generated by Erg for " + stem + "; no changes\n";
        files.push_back({stem + ".lub", std::vector<uint8_t>(none.begin(), none.end())});
    }
    for (const char* rel : {"Maps/%s.hmp", "Maps/%s.txt"}) {
        std::string name = rel;
        name.replace(name.find("%s"), 2, stem);
        if (std::any_of(files.begin(), files.end(), [&](const build::File& f) { return f.rel == name; })) continue;
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(root + L"\\" + Slashes(name)), ec);
    }
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

Reply Service::Call(std::string_view method, std::string_view paramsJson) {
    Json p;
    std::string perr;
    if (!xom::ParseJson(paramsJson.empty() ? std::string_view("{}") : paramsJson, p, &perr) || p.kind != Json::Kind::Object)
        return Err(kBadParams, "params must be a JSON object");
    std::lock_guard lk(impl_->mx);
    if (method == "level.list") return impl_->List();
    if (method == "level.new") return impl_->New(p);
    if (method == "level.load") return impl_->LoadMethod(p);
    if (method == "level.save") return impl_->Save(p);
    if (method == "level.close") return impl_->Close(p);
    if (method == "level.themes") return impl_->Themes();
    if (method == "level.palette") return impl_->Palette(p);
    if (method == "level.export") return impl_->Export(p);
    if (method == "level.build") return impl_->BuildMod(p);
    return Err(-32601, "unknown method");
}
}  // namespace melange::erg::service
