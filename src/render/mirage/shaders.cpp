// Module "MirageShaders": Cg source overrides and patches from mod folders, hot reload, shader parameters, the
// built-in FXAA fix and a runtime FXAA switch.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/jlog.h"
#include "melange/overlay.h"
#include "melange/shaders.h"
#include "melange/testcmd.h"
#include "render/mirage/engine.h"
#include "render/mirage/modfs.h"
#include "render/mirage/shaders_internal.h"

namespace melange::mirage::shaders {
namespace src = shadersrc;
namespace {
using src::CGcontext;
using CreateFn = CGprogram(__cdecl*)(CGcontext, int, const char*, int, const char*, const char**);
using ProgFn = void(__cdecl*)(CGprogram);

struct Config {
    bool fixFxaa = true, glsl = true, glslProfile = false, testCompile = true;
} g_cfg;
src::Cg g_cg;
CreateFn g_create = nullptr;
ProgFn g_bind = nullptr, g_update = nullptr;
bool g_bindHooked = false, g_updateHooked = false;
CGcontext g_testCtx = nullptr;
std::recursive_mutex g_mx;

struct CodeRoot {
    int handle;
    std::wstring dir;
    std::string owner;
};
std::vector<CodeRoot> g_codeRoots;
int g_nextRoot = 1;

struct Record {
    const char* file;
    const char* entry;
    const char* owner;
    int stage;
    bool overridden;
    CGprogram program;
};
std::map<std::string, Record> g_records;  // lower(file)|entry|stage
std::map<CGprogram, std::string> g_keyOf;

struct Err {
    const char* file;
    int line;
    const char* text;
    const char* owner;
};
std::deque<Err> g_errors;
std::map<std::string, uint64_t> g_recentIssues;
uint32_t g_passthrough = 0, g_reloads = 0, g_compileErrors = 0;
std::string g_toast;
uint64_t g_toastTick = 0;

struct Param {
    std::string file, entry, name, owner;
    int n;
    float v[16];
    bool user;
    const src::ParamSpec* spec;
};
std::vector<Param> g_params;
std::vector<std::pair<src::ParamSpec, std::string>> g_specs;
struct Bound {
    src::CGparameter param;
    size_t index;
    int size;
};
std::map<CGprogram, std::vector<Bound>> g_bindCache;
volatile bool g_paramsLive = false;

std::string SafeStr(const char* p, size_t max = 260) {
    std::string s;
    char c = 0;
    while (p && s.size() < max && mem::SafeRead(reinterpret_cast<uintptr_t>(p + s.size()), &c, 1) && c) s += c;
    return s;
}

std::string Key(std::string_view file, std::string_view entry, int stage) {
    return src::Lower(file) + "|" + std::string(entry) + "|" + std::to_string(stage);
}

std::wstring VanillaDir(const std::string& path) {
    size_t p = path.find_last_of("/\\");
    return p == std::string::npos ? L"." : src::Widen(path.substr(0, p));
}

int StageOf(int profile, const std::string& name) {
    if (profile == engine::CgProfile(1)) return 1;
    if (profile == engine::CgProfile(0)) return 0;
    std::string l = src::Lower(name);
    return l.find("fp") != std::string::npos || l.find("glslf") != std::string::npos ? 1 : 0;
}

const char* ProfileName(int profile) {
    const char* s = g_cg.ok ? g_cg.ProfileString(profile) : nullptr;
    return s ? s : "";
}

void SetToast(const std::string& s) {
    std::lock_guard lk(g_mx);
    g_toast = s;
    g_toastTick = GetTickCount64();
}

bool Fresh(const std::string& what) {
    uint64_t now = GetTickCount64();
    std::lock_guard lk(g_mx);
    auto it = g_recentIssues.find(what);
    if (it != g_recentIssues.end() && now - it->second < 10000) return false;
    g_recentIssues[what] = now;
    return true;
}

void AddError(const std::string& file, int line, const std::string& text, const std::string& owner) {
    std::lock_guard lk(g_mx);
    g_errors.push_back({Intern(file), line, Intern(text), Intern(owner)});
    while (g_errors.size() > 64) g_errors.pop_front();
    ++g_compileErrors;
}

// Returns the number of error lines.
int ReportListing(const char* listing, const std::string& mainFile, const std::string& owner) {
    int errors = 0;
    for (const src::Diag& d : src::ParseListing(listing ? listing : "", mainFile)) {
        if (!d.error) continue;
        ++errors;
        std::string where = d.file + "(" + std::to_string(d.line) + ")";
        if (!Fresh(where + d.text + owner)) continue;
        AddError(d.file, d.line, d.text, owner);
        LOG_WARN("[shaders] %s: %s%s%s", where.c_str(), d.text.c_str(), owner.empty() ? "" : " [", owner.empty() ? "" : (owner + "]").c_str());
        jlog::Rec("shader", jlog::Level::Error, "compile error")
            .Str("where", where).Str("file", d.file).Int("line", d.line).Str("text", d.text).Str("owner", owner);
    }
    return errors;
}

void Remember(const std::string& file, const std::string& entry, int stage, CGprogram p, const std::string& owner, bool overridden) {
    std::lock_guard lk(g_mx);
    std::string key = Key(file, entry, stage);
    Record& r = g_records[key];
    r = {Intern(file), Intern(entry), Intern(owner), stage, overridden, p};
    for (auto it = g_keyOf.begin(); it != g_keyOf.end();)
        it = it->second == key ? g_keyOf.erase(it) : std::next(it);
    if (p) g_keyOf[p] = key;
    g_bindCache.erase(p);
}

// ---------------------------------------------------------------- parameters
std::vector<Bound> BindingsFor(CGprogram p) {
    std::vector<Bound> out;
    auto k = g_keyOf.find(p);
    if (k == g_keyOf.end()) return out;
    const Record& r = g_records[k->second];
    for (size_t i = 0; i < g_params.size(); ++i) {
        const Param& pa = g_params[i];
        if (!src::IEquals(pa.file, r.file) || !src::Glob(pa.entry, r.entry)) continue;
        src::CGparameter cp = g_cg.GetNamedParameter(p, pa.name.c_str());
        float tmp[16];
        int size = cp ? g_cg.GetParameterValuefr(cp, 16, tmp) : 0;
        if (size > 0 && size <= 16) out.push_back({cp, i, size});
    }
    g_cg.Drain();
    return out;
}

void ApplyTo(CGprogram p) {
    auto it = g_bindCache.find(p);
    if (it == g_bindCache.end()) it = g_bindCache.emplace(p, BindingsFor(p)).first;
    for (const Bound& b : it->second) {
        const Param& pa = g_params[b.index];
        float v[16] = {};
        std::copy(pa.v, pa.v + std::min(pa.n, b.size), v);
        g_cg.SetParameterValuefr(b.param, b.size, v);
    }
}

void __cdecl HkBind(CGprogram p) {
    if (g_paramsLive && p) {
        std::lock_guard lk(g_mx);
        ApplyTo(p);
    }
    g_bind(p);
    if (glsl::Installed()) glsl::OnBind(p);
}

void __cdecl HkUpdate(CGprogram p) {
    if (g_paramsLive && p) {
        std::lock_guard lk(g_mx);
        ApplyTo(p);
    }
    g_update(p);
}

bool HookBind() {
    if (!g_bindHooked)
        g_bindHooked = mem::HookIAT("cgGL.dll", "cgGLBindProgram", reinterpret_cast<void*>(&HkBind), reinterpret_cast<void**>(&g_bind));
    return g_bindHooked;
}

// Engine values are set before Bind flushes them (cgGLBindProgram on a switch, else cgUpdateProgramParameters), so
// re-setting ours in front of both makes them win.
void EnsureParamHooks() {
    HookBind();
    if (!g_updateHooked)
        g_updateHooked = mem::HookIAT("cg.dll", "cgUpdateProgramParameters", reinterpret_cast<void*>(&HkUpdate),
                                      reinterpret_cast<void**>(&g_update));
    g_paramsLive = g_bindHooked && g_updateHooked && !g_params.empty();
}

void ParamsChanged() {
    std::lock_guard lk(g_mx);
    g_bindCache.clear();
    if (!g_params.empty()) EnsureParamHooks();
    g_paramsLive = g_bindHooked && g_updateHooked && !g_params.empty();
    for (auto& [p, key] : g_keyOf) ApplyTo(p);
    g_cg.Drain();
}

Param* FindParam(std::string_view file, std::string_view entry, std::string_view name) {
    for (Param& p : g_params)
        if (src::IEquals(p.file, file) && p.entry == entry && p.name == name) return &p;
    return nullptr;
}

bool StoreParam(const char* file, const char* entry, const char* name, const float* v, int n, bool user) {
    if (!file || !entry || !name || !v || n < 1 || n > 16 || !g_cg.ok) return false;
    std::lock_guard lk(g_mx);
    Param* p = FindParam(file, entry, name);
    if (!p) {
        g_params.push_back({file, entry, name, "", n, {}, user, nullptr});
        p = &g_params.back();
    }
    p->n = n;
    std::copy(v, v + n, p->v);
    p->user = p->user || user;
    ParamsChanged();
    return true;
}

void LoadParamsIni() {
    std::vector<std::pair<src::ParamSpec, std::string>> specs;
    modfs::Root roots[128];
    int n = modfs::Roots(roots, 128);
    for (int i = 0; i < n; ++i) {
        std::string text, err;
        if (!src::ReadFile(std::wstring(roots[i].dir) + L"\\shaders\\params.ini", &text)) continue;
        std::vector<src::ParamSpec> list;
        if (!src::ParseParams(text, &list, &err)) {
            ReportIssue({"params.ini", 0, err, roots[i].id, true});
            continue;
        }
        for (const src::ParamSpec& s : list) specs.emplace_back(s, roots[i].id);
    }
    std::lock_guard lk(g_mx);
    g_specs = std::move(specs);
    std::erase_if(g_params, [](const Param& p) { return !p.user; });
    for (Param& p : g_params) p.spec = nullptr;
    for (const auto& [s, owner] : g_specs) {
        Param* p = FindParam(s.file, s.entryGlob, s.name);
        if (!p) {
            g_params.push_back({s.file, s.entryGlob, s.name, owner, s.n, {}, false, nullptr});
            p = &g_params.back();
            std::copy(s.def, s.def + s.n, p->v);
        }
        p->owner = owner;
    }
    for (Param& p : g_params)
        for (const auto& [s, owner] : g_specs)
            if (src::IEquals(p.file, s.file) && p.entry == s.entryGlob && p.name == s.name) p.spec = &s;
    if (!g_specs.empty()) LOG_INFO("[shaders] %zu shader parameters from params.ini", g_specs.size());
    ParamsChanged();
}

// ---------------------------------------------------------------- loads
CGprogram __cdecl HkCreate(CGcontext ctx, int type, const char* file, int profile, const char* entry, const char** args) {
    if (type != src::kCgSource || !file || !entry || !g_cg.ok) return g_create(ctx, type, file, profile, entry, args);
    std::string path(file), base = src::BaseName(path), prof = ProfileName(profile);
    std::wstring vdir = VanillaDir(path);
    int stage = StageOf(profile, prof);
    src::Sources s = MakeSources(vdir);
    bool replaced = glsl::Has(base, entry);
    src::Loaded main;
    std::vector<src::Issue> issues;
    if (!s.roots.empty() || s.AnyBuiltin(entry, prof) || replaced) main = s.Load(base, entry, prof, &issues);
    if (!main.found) {
        {
            std::lock_guard lk(g_mx);
            ++g_passthrough;
        }
        CGprogram p = g_create(ctx, type, file, profile, entry, args);
        Remember(base, entry, stage, p, "", false);
        return p;
    }
    src::Job job;
    job.src = &s;
    job.cg = &g_cg;
    job.mainName = base;
    job.entry = entry;
    job.profile = prof;
    job.owner = main.owner;
    job.overridden = main.fromRoot || main.patched || main.builtin;
    g_cg.Drain();
    // The engine reads cgGetError right after this call to decide whether the program failed, so the error the
    // compile raised must still be pending when we return.
    CGprogram p = src::Compile(job, ctx, main.text, profile, args);
    issues.insert(issues.end(), job.issues.begin(), job.issues.end());
    for (const src::Issue& i : issues) ReportIssue(i);
    if (!p) ReportListing(g_cg.GetLastListing(ctx), base, job.owner);
    Remember(base, entry, stage, p, job.owner, job.overridden);
    if (job.overridden)
        LOG_INFO("[shaders] %s:%s (%s) %s from %s", base.c_str(), entry, prof.c_str(), p ? "compiled" : "FAILED",
                 job.owner.c_str());
    if (p) {
        glsl::OnCreate(p, base, entry, stage, vdir);
        std::lock_guard lk(g_mx);
        ApplyTo(p);
        g_cg.Drain();
    }
    return p;
}

// ---------------------------------------------------------------- reload
struct Target {
    uintptr_t self;
    std::string file, entry;
    int type;
    std::wstring vdir;
};

bool TestCompile(const Target& t, int* errors) {
    *errors = 0;
    if (!g_cfg.testCompile) return true;
    if (!g_testCtx) g_testCtx = g_cg.CreateContext();
    if (!g_testCtx) return true;
    int profile = engine::CgProfile(t.type);
    std::string prof = ProfileName(profile);
    src::Sources s = MakeSources(t.vdir);
    std::vector<src::Issue> issues;
    src::Loaded main = s.Load(t.file, t.entry, prof, &issues);
    for (const src::Issue& i : issues) ReportIssue(i);
    if (!main.found) {
        ReportIssue({t.file, 0, "file not found", "", true});
        *errors = 1;
        return false;
    }
    src::Job job;
    job.src = &s;
    job.cg = &g_cg;
    job.mainName = t.file;
    job.entry = t.entry;
    job.profile = prof;
    job.owner = main.owner;
    g_cg.Drain();
    CGprogram p = src::Compile(job, g_testCtx, main.text, profile, nullptr);
    for (const src::Issue& i : job.issues) ReportIssue(i);
    if (!p) *errors = std::max(1, ReportListing(g_cg.GetLastListing(g_testCtx), t.file, job.owner));
    else g_cg.DestroyProgram(p);
    g_cg.Drain();
    return p != nullptr;
}

using Pred = std::function<bool(const std::string& file, const std::string& entry, const std::vector<std::string>& deps)>;

int ReloadWhere(const Pred& pred, const std::string& what) {
    if (!g_cg.ok) return 0;
    std::vector<Target> all;
    engine::ForEachCgProg(
        [](const engine::CgProg& c, void* u) {
            std::string path = SafeStr(c.path), entry = SafeStr(c.entry);
            if (path.empty() || entry.empty()) return;
            static_cast<std::vector<Target>*>(u)->push_back({c.self, src::BaseName(path), entry, c.type, VanillaDir(path)});
        },
        &all);
    std::map<std::string, std::vector<std::string>> deps;
    std::vector<const Target*> hits;
    for (const Target& t : all) {
        std::string lf = src::Lower(t.file);
        if (!deps.count(lf)) deps[lf] = MakeSources(t.vdir).Dependencies(t.file);
        if (pred(t.file, t.entry, deps[lf])) hits.push_back(&t);
    }
    if (hits.empty()) return 0;
    {
        std::lock_guard lk(g_mx);
        g_recentIssues.clear();
    }
    uint64_t t0 = GetTickCount64();
    std::map<std::string, bool> tested;
    std::map<std::string, int> failed;  // file -> error lines
    int marked = 0;
    for (const Target* t : hits) {
        std::string key = Key(t->file, t->entry, t->type);
        if (!tested.count(key)) {
            int errors = 0;
            tested[key] = TestCompile(*t, &errors);
            if (!tested[key]) failed[t->file] += errors;
        }
        if (tested[key] && engine::MarkReload(t->self)) ++marked;
    }
    {
        std::lock_guard lk(g_mx);
        g_reloads += static_cast<uint32_t>(marked);
    }
    uint64_t ms = GetTickCount64() - t0;
    std::string toast;
    for (const auto& [f, n] : failed) {
        char b[160];
        snprintf(b, sizeof b, "%s%s: %d error%s (kept previous)", toast.empty() ? "" : "; ", f.c_str(), n, n == 1 ? "" : "s");
        toast += b;
    }
    if (toast.empty()) toast = what + ": reloaded " + std::to_string(marked) + " program" + (marked == 1 ? "" : "s");
    SetToast(toast);
    LOG_INFO("[shaders] reload %s: %zu programs matched, %d marked, %zu files kept previous (%llu ms test compile)",
             what.c_str(), hits.size(), marked, failed.size(), static_cast<unsigned long long>(ms));
    jlog::Rec("shader", failed.empty() ? jlog::Level::Info : jlog::Level::Warn, "reload")
        .Str("match", what).Uint("matched", hits.size()).Int("marked", marked).Uint("failedFiles", failed.size())
        .Uint("testMs", ms).Str("toast", toast);
    return marked;
}

void OnShaderFileChanged(const wchar_t* path, void*) {
    std::wstring p(path);
    std::wstring low = p;
    for (wchar_t& c : low) c = static_cast<wchar_t>(towlower(c));
    size_t at = low.find(L"\\shaders\\");
    if (at == std::wstring::npos) return;
    std::string rel = src::Narrow(p.substr(at + 9)), name = src::BaseName(rel), lname = src::Lower(name);
    auto endsWith = [&](const char* suf) {
        size_t n = strlen(suf);
        return lname.size() > n && lname.compare(lname.size() - n, n, suf) == 0;
    };
    LOG_INFO("[shaders] changed: %s", rel.c_str());
    if (lname == "params.ini") return LoadParamsIni();
    if (endsWith(".glsl")) return glsl::OnFileChanged(p);
    if (endsWith(".patch")) name.resize(name.size() - 6);
    std::string target = src::Lower(name);
    ReloadWhere(
        [&](const std::string& file, const std::string&, const std::vector<std::string>& deps) {
            return src::IEquals(file, target) || std::find(deps.begin(), deps.end(), target) != deps.end();
        },
        name);
}

// ---------------------------------------------------------------- verbs
std::vector<std::string> Words(std::string_view s) {
    std::vector<std::string> w;
    size_t p = 0;
    while (p < s.size()) {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
        size_t q = p;
        while (q < s.size() && s[q] != ' ' && s[q] != '\t') ++q;
        if (q > p) w.emplace_back(s.substr(p, q - p));
        p = q;
    }
    return w;
}

bool VerbList(std::string_view, void*) {
    std::vector<melange::shaders::ProgramInfo> list(melange::shaders::ListPrograms(nullptr, 0) + 8);
    list.resize(melange::shaders::ListPrograms(list.data(), list.size()));
    for (const auto& p : list)
        LOG_INFO("[shaders] program %s:%s stage=%d failed=%d pendingReload=%d overridden=%d glsl=%d owner=\"%s\" binds=%u",
                 p.file, p.entry, p.stage, p.failed, p.pendingReload, p.overridden, p.glsl, p.owner, p.binds);
    melange::shaders::Stats s = melange::shaders::GetStats();
    LOG_INFO("[shaders] list: programs=%u overridden=%u glsl=%u reloads=%u compileErrors=%u passthroughLoads=%u profiles=%s/%s",
             s.programs, s.overridden, s.glsl, s.reloads, s.compileErrors, s.passthroughLoads,
             melange::shaders::Profile(0), melange::shaders::Profile(1));
    jlog::Rec("shader", jlog::Level::Info, "list")
        .Uint("programs", s.programs).Uint("overridden", s.overridden).Uint("glsl", s.glsl).Uint("reloads", s.reloads)
        .Uint("compileErrors", s.compileErrors).Uint("passthroughLoads", s.passthroughLoads);
    return true;
}

bool VerbReload(std::string_view args, void*) {
    std::vector<std::string> w = Words(args);
    int n = melange::shaders::Reload(w.empty() ? "" : w[0].c_str());
    LOG_INFO("[shaders] shaders.reload '%s': %d programs marked", w.empty() ? "" : w[0].c_str(), n);
    return true;
}

bool VerbParam(std::string_view args, void*) {
    std::vector<std::string> w = Words(args);
    if (w.size() < 3) return false;
    float v[16] = {};
    int n = 0;
    for (size_t i = 3; i < w.size() && n < 16; ++i) v[n++] = static_cast<float>(atof(w[i].c_str()));
    if (n && !melange::shaders::SetParam(w[0].c_str(), w[1].c_str(), w[2].c_str(), v, n)) return false;
    float r[16] = {};
    int rn = n ? n : 4;
    bool ok = melange::shaders::GetParam(w[0].c_str(), w[1].c_str(), w[2].c_str(), r, rn);
    std::string vals;
    for (int i = 0; i < rn; ++i) vals += (i ? " " : "") + std::to_string(r[i]);
    LOG_INFO("[shaders] param %s:%s %s = %s%s", w[0].c_str(), w[1].c_str(), w[2].c_str(), vals.c_str(), ok ? "" : " (not found)");
    return ok;
}

bool VerbErrors(std::string_view, void*) {
    melange::shaders::CompileError e[64];
    size_t n = melange::shaders::LastErrors(e, 64);
    for (size_t i = 0; i < n; ++i) LOG_INFO("[shaders] error %s(%d): %s [%s]", e[i].file, e[i].line, e[i].text, e[i].owner);
    LOG_INFO("[shaders] %zu errors", n);
    return true;
}

bool VerbFxaa(std::string_view args, void*) {
    std::vector<std::string> w = Words(args);
    bool on = w.empty() ? !engine::FxaaOn() : atoi(w[0].c_str()) != 0;
    std::string why;
    bool ok = FxaaToggle(on, &why);
    LOG_INFO("[shaders] fxaa %s: %s%s", on ? "on" : "off", ok ? "done" : "refused: ", why.c_str());
    return ok;
}

void MenuReloadAll(void*) { melange::shaders::Reload(""); }
void MenuToggleFxaa(void*) {
    std::string why;
    if (!FxaaToggle(!engine::FxaaOn(), &why)) SetToast("FXAA: " + why);
}

class MirageShaders final : public melange::Module {
public:
    const char* Name() const override { return "MirageShaders"; }
    const char* Description() const override { return "shader overrides, hot reload, GLSL replacement, FXAA fix"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 43; }

    bool Install() override {
        g_cfg.fixFxaa = Bool("FixFxaa", true);
        g_cfg.glsl = Bool("GlslReplace", true);
        g_cfg.glslProfile = Bool("GlslProfile", false);
        g_cfg.testCompile = Bool("TestCompile", true);
        if (!engine::Check()) return false;
        if (!g_cg.Load(GetModuleHandleW(L"cg.dll"))) {
            LOG_ERROR("[shaders] cg.dll entry points missing: shader layer disabled");
            return false;
        }
        if (!mem::HookIAT("cg.dll", "cgCreateProgramFromFile", reinterpret_cast<void*>(&HkCreate), reinterpret_cast<void**>(&g_create)))
            return false;
        if (glsl::Configure(g_cfg.glsl, g_cfg.glslProfile) && !HookBind())
            LOG_WARN("[shaders] cgGLBindProgram not hooked: GLSL replacements stay inactive");
        modfs::Watch(L"shaders", &OnShaderFileChanged, nullptr);
        LoadParamsIni();

        melange::testcmd::Register("shaders.list", &VerbList);
        melange::testcmd::Register("shaders.reload", &VerbReload);
        melange::testcmd::Register("shaders.param", &VerbParam);
        melange::testcmd::Register("shaders.errors", &VerbErrors);
        melange::testcmd::Register("shaders.fxaa", &VerbFxaa);
        melange::overlay::AddMenuItem("Mirage/Shaders/Reload all", &MenuReloadAll, nullptr);
        melange::overlay::AddMenuItem("Mirage/Shaders/Toggle FXAA", &MenuToggleFxaa, nullptr);
        RegisterPanel();

        int roots = static_cast<int>(MakeSources(L"CG").roots.size());
        LOG_INFO("[shaders] ready: FixFxaa=%d TestCompile=%d GlslReplace=%d GlslProfile=%d, %d shader override root%s",
                 g_cfg.fixFxaa, g_cfg.testCompile, g_cfg.glsl, g_cfg.glslProfile, roots, roots == 1 ? "" : "s");
        return true;
    }
};
}  // namespace

const src::Cg& CgApi() { return g_cg; }

src::Sources MakeSources(const std::wstring& vanillaDir) {
    src::Sources s;
    s.vanillaDir = vanillaDir;
    s.builtins = g_cfg.fixFxaa;
    modfs::Root roots[128];
    int n = modfs::Roots(roots, 128);
    for (int i = 0; i < n; ++i) {
        std::wstring d = std::wstring(roots[i].dir) + L"\\shaders";
        DWORD a = GetFileAttributesW(d.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) s.roots.push_back({d, roots[i].id});
    }
    std::lock_guard lk(g_mx);
    for (const CodeRoot& c : g_codeRoots) s.roots.push_back({c.dir, c.owner});
    return s;
}

void ReportIssue(const src::Issue& i) {
    if (!Fresh(i.file + std::to_string(i.line) + i.text + i.owner)) return;
    if (i.error) AddError(i.file, i.line, i.text, i.owner);
    std::string where = i.file + (i.line ? "(" + std::to_string(i.line) + ")" : "");
    if (i.error) LOG_WARN("[shaders] %s: %s [%s]", where.c_str(), i.text.c_str(), i.owner.c_str());
    else LOG_INFO("[shaders] %s: %s [%s]", where.c_str(), i.text.c_str(), i.owner.c_str());
    jlog::Rec("shader", i.error ? jlog::Level::Error : jlog::Level::Info, i.error ? "patch error" : "patch note")
        .Str("where", where).Str("file", i.file).Int("line", i.line).Str("text", i.text).Str("owner", i.owner);
    if (i.error) SetToast(where + ": " + i.text);
}

const char* Intern(const std::string& s) {
    static std::mutex mx;
    static std::set<std::string> pool;
    std::lock_guard lk(mx);
    return pool.insert(s).first->c_str();
}

std::vector<ParamRow> Params() {
    std::vector<ParamRow> out;
    std::lock_guard lk(g_mx);
    for (const Param& p : g_params) {
        ParamRow r{p.file, p.entry, p.name, "", "", p.owner, p.n, {}, 0, 1, p.spec != nullptr};
        std::copy(p.v, p.v + p.n, r.v);
        if (p.spec) {
            r.label = p.spec->label;
            r.type = p.spec->type;
            r.min = p.spec->min;
            r.max = p.spec->max;
        }
        out.push_back(r);
    }
    return out;
}

bool FxaaToggle(bool on, std::string* why) {
    if (!engine::Check()) {
        *why = "engine checks failed";
        return false;
    }
    if (on) {
        struct Find {
            bool found = false, failed = false;
        } f;
        engine::ForEachCgProg(
            [](const engine::CgProg& c, void* u) {
                if (SafeStr(c.entry) == "CopyFxaa") {
                    static_cast<Find*>(u)->found = true;
                    static_cast<Find*>(u)->failed = c.failed;
                }
            },
            &f);
        if (!f.found || f.failed) {
            *why = "CopyFxaa did not compile on this GPU (set [MirageShaders] FixFxaa=1)";
            return false;
        }
    }
    if (!engine::SetFxaa(on)) {
        *why = "the game's SSAA factor is not 1x1";
        return false;
    }
    LOG_INFO("[shaders] FXAA %s", on ? "on" : "off");
    jlog::Rec("mirage", jlog::Level::Info, "fxaa").Bool("on", on);
    SetToast(std::string("FXAA ") + (on ? "on" : "off"));
    return true;
}

std::string LastToast(uint64_t* tick) {
    std::lock_guard lk(g_mx);
    if (tick) *tick = g_toastTick;
    return g_toast;
}

MELANGE_MODULE(MirageShaders);
}  // namespace melange::mirage::shaders

namespace melange::shaders {
namespace ms = melange::mirage::shaders;
namespace src = melange::mirage::shadersrc;
namespace engine = melange::mirage::engine;

size_t ListPrograms(ProgramInfo* out, size_t max) {
    struct Ctx {
        ProgramInfo* out;
        size_t max, n;
    } ctx{out, max, 0};
    engine::ForEachCgProg(
        [](const engine::CgProg& c, void* u) {
            auto& x = *static_cast<Ctx*>(u);
            if (x.out && x.n >= x.max) return;
            if (x.out) {
                std::string file = src::BaseName(ms::SafeStr(c.path)), entry = ms::SafeStr(c.entry);
                ProgramInfo& p = x.out[x.n];
                p = {ms::Intern(file), ms::Intern(entry), static_cast<uint8_t>(c.type == 1), c.failed, c.reload, false,
                     ms::glsl::Active(file, entry), "", c.binds};
                std::lock_guard lk(ms::g_mx);
                auto it = ms::g_records.find(ms::Key(file, entry, c.type == 1 ? 1 : 0));
                if (it != ms::g_records.end()) {
                    p.overridden = it->second.overridden;
                    p.owner = it->second.owner;
                }
            }
            ++x.n;
        },
        &ctx);
    return ctx.n;
}

const char* Profile(uint8_t stage) { return ms::ProfileName(engine::CgProfile(stage)); }

int Reload(const char* match) {
    std::string m = match ? match : "";
    if (m == "*") m.clear();
    return ms::ReloadWhere(
        [&](const std::string& file, const std::string& entry, const std::vector<std::string>& deps) {
            if (src::IContains(file, m) || src::IContains(entry, m)) return true;
            for (const std::string& d : deps)
                if (src::IContains(d, m)) return true;
            return false;
        },
        m.empty() ? "all" : m);
}

bool SetParam(const char* file, const char* entry, const char* param, const float* v, int n) {
    return ms::StoreParam(file, entry, param, v, n, true);
}

bool GetParam(const char* file, const char* entry, const char* param, float* v, int n) {
    if (!file || !entry || !param || !v || n < 1) return false;
    std::lock_guard lk(ms::g_mx);
    if (ms::Param* p = ms::FindParam(file, entry, param)) {
        for (int i = 0; i < n; ++i) v[i] = i < p->n ? p->v[i] : 0.f;
        return true;
    }
    if (!ms::g_cg.ok) return false;
    for (const auto& [key, r] : ms::g_records) {
        if (!r.program || !src::IEquals(r.file, file) || !src::Glob(entry, r.entry)) continue;
        src::CGparameter cp = ms::g_cg.GetNamedParameter(r.program, param);
        float tmp[16] = {};
        int got = cp ? ms::g_cg.GetParameterValuefr(cp, 16, tmp) : 0;
        ms::g_cg.Drain();
        if (got <= 0) continue;
        for (int i = 0; i < n; ++i) v[i] = i < got && i < 16 ? tmp[i] : 0.f;
        return true;
    }
    return false;
}

int AddOverrideRoot(const wchar_t* dir, const char* owner) {
    if (!dir || !*dir) return 0;
    std::lock_guard lk(ms::g_mx);
    int h = ms::g_nextRoot++;
    ms::g_codeRoots.push_back({h, dir, owner ? owner : ""});
    return h;
}

void RemoveOverrideRoot(int handle) {
    std::lock_guard lk(ms::g_mx);
    std::erase_if(ms::g_codeRoots, [&](const ms::CodeRoot& c) { return c.handle == handle; });
}

size_t LastErrors(CompileError* out, size_t max) {
    std::lock_guard lk(ms::g_mx);
    size_t n = std::min(max, ms::g_errors.size());
    for (size_t i = 0; out && i < n; ++i) {
        const ms::Err& e = ms::g_errors[ms::g_errors.size() - n + i];
        out[i] = {e.file, e.line, e.text, e.owner};
    }
    return out ? n : ms::g_errors.size();
}

Stats GetStats() {
    Stats s{};
    s.programs = static_cast<uint32_t>(ListPrograms(nullptr, 0));
    std::lock_guard lk(ms::g_mx);
    for (const auto& [key, r] : ms::g_records)
        if (r.overridden) ++s.overridden;
    s.glsl = ms::glsl::ActiveCount();
    s.reloads = ms::g_reloads;
    s.compileErrors = ms::g_compileErrors;
    s.passthroughLoads = ms::g_passthrough;
    return s;
}
}  // namespace melange::shaders
