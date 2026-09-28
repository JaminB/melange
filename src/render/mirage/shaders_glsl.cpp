// Per-program GLSL replacement (experimental, [MirageShaders] GlslReplace=1): a mod file
// shaders\<File>.<Entry>.glsl replaces one Cg program. The Cg program still exists, so every engine handle, bind and
// parameter keeps working; Melange links its own GL program from the mod stage and the partner stage (the partner's
// Cg entry compiled to GLSL by Cg) and makes it current while the engine has the replaced program bound. Uniforms
// keep the Cg names: the values cgGL flushes to the ARB programs are mirrored and uploaded before each draw.
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/mem.h"
#include "melange/jlog.h"
#include "render/mirage/engine.h"
#include "render/mirage/hub.h"
#include "render/mirage/modfs.h"
#include "render/mirage/shaders_internal.h"

namespace melange::mirage::shaders::glsl {
namespace src = shadersrc;
namespace {
using GLchar = char;
constexpr GLenum kFragmentShader = 0x8B30, kVertexShader = 0x8B31, kCompileStatus = 0x8B81, kLinkStatus = 0x8B82,
                 kInfoLogLength = 0x8B84, kActiveUniforms = 0x8B86, kFloatVec2 = 0x8B50, kFloatVec3 = 0x8B51,
                 kFloatVec4 = 0x8B52, kFloatMat4 = 0x8B5C, kSampler1D = 0x8B5D, kSampler2DRectShadow = 0x8B64,
                 kTexture0 = 0x84C0, kVertexProgramArb = 0x8620, kFragmentProgramArb = 0x8804;
constexpr int kCgGlVertex = 8, kCgGlFragment = 9;

struct Gl {
    GLuint(WINAPI* CreateShader)(GLenum);
    void(WINAPI* ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void(WINAPI* CompileShader)(GLuint);
    void(WINAPI* GetShaderiv)(GLuint, GLenum, GLint*);
    void(WINAPI* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void(WINAPI* DeleteShader)(GLuint);
    GLuint(WINAPI* CreateProgram)();
    void(WINAPI* AttachShader)(GLuint, GLuint);
    void(WINAPI* LinkProgram)(GLuint);
    void(WINAPI* GetProgramiv)(GLuint, GLenum, GLint*);
    void(WINAPI* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void(WINAPI* DeleteProgram)(GLuint);
    void(WINAPI* UseProgram)(GLuint);
    void(WINAPI* GetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, GLchar*);
    GLint(WINAPI* GetUniformLocation)(GLuint, const GLchar*);
    void(WINAPI* Uniform1i)(GLint, GLint);
    void(WINAPI* Uniform1fv)(GLint, GLsizei, const GLfloat*);
    void(WINAPI* Uniform2fv)(GLint, GLsizei, const GLfloat*);
    void(WINAPI* Uniform3fv)(GLint, GLsizei, const GLfloat*);
    void(WINAPI* Uniform4fv)(GLint, GLsizei, const GLfloat*);
    void(WINAPI* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    bool tried = false, ok = false;
} g_gl;

GLuint(__cdecl* g_cgGLGetProgramID)(src::CGprogram) = nullptr;
GLenum(__cdecl* g_cgGLGetTextureEnum)(src::CGparameter) = nullptr;
void(__cdecl* g_enableProfile)(int) = nullptr;
void(__cdecl* g_disableProfile)(int) = nullptr;
int(__cdecl* g_latestProfile)(int) = nullptr;

struct File {
    std::wstring path;
    std::string owner, name;
};
std::map<std::string, File> g_files;  // lower(file)|entry
bool g_installed = false;
src::CGcontext g_ctx = nullptr;

struct Prog {
    std::string file, entry;
    int stage;
    std::wstring vdir;
    bool replaced;
    bool linkedOk;
};
std::unordered_map<src::CGprogram, Prog> g_progs;

struct Binding {
    GLint loc;
    GLenum type;
    GLint count;
    GLuint arb;
    unsigned index;
};
struct Linked {
    GLuint program = 0;
    bool failed = false;
    std::vector<Binding> bindings;
    uint32_t seen[2] = {~0u, ~0u};
    GLuint arb[2] = {};
};
std::map<std::pair<src::CGprogram, src::CGprogram>, Linked> g_linked;
src::CGprogram g_cur[2] = {};
bool g_profileOn[2] = {};
GLuint g_used = 0;
Linked* g_active = nullptr;

struct Mirror {
    std::vector<float> v;  // 4 floats per program.local index
    uint32_t version = 0;
};
std::unordered_map<GLuint, Mirror> g_mirror;
GLuint g_boundArb[2] = {};

std::string Key(std::string_view file, std::string_view entry) { return src::Lower(file) + "|" + std::string(entry); }

void Store(GLuint prog, GLuint index, GLsizei count, const GLfloat* v) {
    if (!prog || !v || count <= 0 || index + count > 4096) return;
    Mirror& m = g_mirror[prog];
    if (m.v.size() < (index + count) * 4u) m.v.resize((index + count) * 4u);
    std::copy(v, v + count * 4, m.v.begin() + index * 4);
    ++m.version;
}

int ArbSlot(GLenum target) { return target == kFragmentProgramArb ? 1 : target == kVertexProgramArb ? 0 : -1; }

using PL4fv = void(WINAPI*)(GLuint, GLenum, GLuint, const GLfloat*);
using PLs4fv = void(WINAPI*)(GLuint, GLenum, GLuint, GLsizei, const GLfloat*);
using PL4f = void(WINAPI*)(GLuint, GLenum, GLuint, GLfloat, GLfloat, GLfloat, GLfloat);
using L4fv = void(WINAPI*)(GLenum, GLuint, const GLfloat*);
using Ls4fv = void(WINAPI*)(GLenum, GLuint, GLsizei, const GLfloat*);
using BindArb = void(WINAPI*)(GLenum, GLuint);
using DrawElements = void(WINAPI*)(GLenum, GLsizei, GLenum, const void*);
using DrawArrays = void(WINAPI*)(GLenum, GLint, GLsizei);
using DrawRange = void(WINAPI*)(GLenum, GLuint, GLuint, GLsizei, GLenum, const void*);
PL4fv n_pl4fv;
PLs4fv n_pls4fv;
PL4f n_pl4f;
L4fv n_l4fv;
Ls4fv n_ls4fv;
BindArb n_bindArb;
DrawElements n_drawElements;
DrawArrays n_drawArrays;
DrawRange n_drawRange;

void Upload();

void WINAPI HkPL4fv(GLuint p, GLenum t, GLuint i, const GLfloat* v) {
    Store(p, i, 1, v);
    n_pl4fv(p, t, i, v);
}
void WINAPI HkPLs4fv(GLuint p, GLenum t, GLuint i, GLsizei n, const GLfloat* v) {
    Store(p, i, n, v);
    n_pls4fv(p, t, i, n, v);
}
void WINAPI HkPL4f(GLuint p, GLenum t, GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    GLfloat v[4] = {x, y, z, w};
    Store(p, i, 1, v);
    n_pl4f(p, t, i, x, y, z, w);
}
void WINAPI HkL4fv(GLenum t, GLuint i, const GLfloat* v) {
    if (int s = ArbSlot(t); s >= 0) Store(g_boundArb[s], i, 1, v);
    n_l4fv(t, i, v);
}
void WINAPI HkLs4fv(GLenum t, GLuint i, GLsizei n, const GLfloat* v) {
    if (int s = ArbSlot(t); s >= 0) Store(g_boundArb[s], i, n, v);
    n_ls4fv(t, i, n, v);
}
void WINAPI HkBindArb(GLenum t, GLuint p) {
    if (int s = ArbSlot(t); s >= 0) g_boundArb[s] = p;
    n_bindArb(t, p);
}
void WINAPI HkDrawElements(GLenum m, GLsizei c, GLenum t, const void* i) {
    if (g_active) Upload();
    n_drawElements(m, c, t, i);
}
void WINAPI HkDrawArrays(GLenum m, GLint f, GLsizei c) {
    if (g_active) Upload();
    n_drawArrays(m, f, c);
}
void WINAPI HkDrawRange(GLenum m, GLuint s, GLuint e, GLsizei c, GLenum t, const void* i) {
    if (g_active) Upload();
    n_drawRange(m, s, e, c, t, i);
}

template <class T>
void Proc(T& fn, const char* name) {
    PROC p = wglGetProcAddress(name);
    auto v = reinterpret_cast<intptr_t>(p);
    fn = (p && v != 1 && v != 2 && v != 3 && v != -1) ? reinterpret_cast<T>(p) : nullptr;
}

bool LoadGl() {
    if (g_gl.tried) return g_gl.ok;
    g_gl.tried = true;
    Proc(g_gl.CreateShader, "glCreateShader");
    Proc(g_gl.ShaderSource, "glShaderSource");
    Proc(g_gl.CompileShader, "glCompileShader");
    Proc(g_gl.GetShaderiv, "glGetShaderiv");
    Proc(g_gl.GetShaderInfoLog, "glGetShaderInfoLog");
    Proc(g_gl.DeleteShader, "glDeleteShader");
    Proc(g_gl.CreateProgram, "glCreateProgram");
    Proc(g_gl.AttachShader, "glAttachShader");
    Proc(g_gl.LinkProgram, "glLinkProgram");
    Proc(g_gl.GetProgramiv, "glGetProgramiv");
    Proc(g_gl.GetProgramInfoLog, "glGetProgramInfoLog");
    Proc(g_gl.DeleteProgram, "glDeleteProgram");
    Proc(g_gl.UseProgram, "glUseProgram");
    Proc(g_gl.GetActiveUniform, "glGetActiveUniform");
    Proc(g_gl.GetUniformLocation, "glGetUniformLocation");
    Proc(g_gl.Uniform1i, "glUniform1i");
    Proc(g_gl.Uniform1fv, "glUniform1fv");
    Proc(g_gl.Uniform2fv, "glUniform2fv");
    Proc(g_gl.Uniform3fv, "glUniform3fv");
    Proc(g_gl.Uniform4fv, "glUniform4fv");
    Proc(g_gl.UniformMatrix4fv, "glUniformMatrix4fv");
    g_gl.ok = g_gl.CreateShader && g_gl.ShaderSource && g_gl.CompileShader && g_gl.GetShaderiv && g_gl.GetShaderInfoLog &&
              g_gl.DeleteShader && g_gl.CreateProgram && g_gl.AttachShader && g_gl.LinkProgram && g_gl.GetProgramiv &&
              g_gl.GetProgramInfoLog && g_gl.DeleteProgram && g_gl.UseProgram && g_gl.GetActiveUniform &&
              g_gl.GetUniformLocation && g_gl.Uniform1i && g_gl.Uniform1fv && g_gl.Uniform2fv && g_gl.Uniform3fv &&
              g_gl.Uniform4fv && g_gl.UniformMatrix4fv;
    if (!g_gl.ok) LOG_WARN("[shaders] GLSL entry points missing: GLSL replacements stay inactive");
    return g_gl.ok;
}

void ReportLog(const std::string& file, const std::string& owner, const std::string& log) {
    size_t p = 0;
    while (p < log.size()) {
        size_t q = log.find('\n', p);
        if (q == std::string::npos) q = log.size();
        std::string l = log.substr(p, q - p);
        p = q + 1;
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.empty()) continue;
        int line = 0;
        if (size_t c = l.find("0:"); c != std::string::npos) line = atoi(l.c_str() + c + 2);
        else if (size_t b = l.find("0("); b != std::string::npos) line = atoi(l.c_str() + b + 2);
        ReportIssue({file, line, l, owner, true});
    }
}

GLuint CompileStage(GLenum type, const std::string& text, const std::string& file, const std::string& owner) {
    GLuint s = g_gl.CreateShader(type);
    const GLchar* p = text.c_str();
    g_gl.ShaderSource(s, 1, &p, nullptr);
    g_gl.CompileShader(s);
    GLint ok = 0, len = 0;
    g_gl.GetShaderiv(s, kCompileStatus, &ok);
    if (!ok) {
        g_gl.GetShaderiv(s, kInfoLogLength, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        g_gl.GetShaderInfoLog(s, len, nullptr, log.data());
        ReportLog(file, owner, log.c_str());
        g_gl.DeleteShader(s);
        return 0;
    }
    return s;
}

// The partner stage as GLSL, compiled by Cg from the same sources the engine uses.
bool Generate(const Prog& p, std::string* text, std::vector<src::CgVar>* vars) {
    const src::Cg& cg = CgApi();
    if (!g_ctx) g_ctx = cg.CreateContext();
    if (!g_ctx) return false;
    const char* prof = p.stage == 0 ? "glslv" : "glslf";
    src::Sources s = MakeSources(p.vdir);
    std::vector<src::Issue> issues;
    src::Loaded main = s.Load(p.file, p.entry, prof, &issues);
    if (!main.found) return false;
    src::Job job;
    job.src = &s;
    job.cg = &cg;
    job.mainName = p.file;
    job.entry = p.entry;
    job.profile = prof;
    cg.Drain();
    src::CGprogram cp = src::Compile(job, g_ctx, main.text, cg.ProfileByName(prof), nullptr);
    if (!cp) {
        const char* l = cg.GetLastListing(g_ctx);
        ReportIssue({p.file, 0, std::string(prof) + " translation failed: " + (l ? l : ""), "", true});
        cg.Drain();
        return false;
    }
    const char* out = cg.GetProgramString(cp, src::kCgCompiledProgram);
    *text = out ? out : "";
    *vars = src::ParseVars(*text);
    cg.DestroyProgram(cp);
    cg.Drain();
    return !text->empty();
}

std::string StripIndex(std::string s) {
    if (size_t b = s.find('['); b != std::string::npos) s.resize(b);
    return s;
}

Linked Link(src::CGprogram vp, src::CGprogram fp) {
    Linked l;
    l.failed = true;
    const src::Cg& cg = CgApi();
    src::CGprogram progs[2] = {vp, fp};
    std::string text[2], label[2], owner[2];
    std::vector<src::CgVar> vars[2];
    bool mod[2] = {};
    for (int s = 0; s < 2; ++s) {
        const Prog& p = g_progs[progs[s]];
        label[s] = p.file + ":" + p.entry;
        auto f = g_files.find(Key(p.file, p.entry));
        if (p.replaced && f != g_files.end()) {
            mod[s] = true;
            owner[s] = f->second.owner;
            label[s] = f->second.name;
            if (!src::ReadFile(f->second.path, &text[s])) return l;
        } else if (!Generate(p, &text[s], &vars[s])) {
            return l;
        }
        l.arb[s] = g_cgGLGetProgramID ? g_cgGLGetProgramID(progs[s]) : 0;
    }
    cg.Drain();
    GLuint sh[2] = {CompileStage(kVertexShader, text[0], label[0], owner[0]), CompileStage(kFragmentShader, text[1], label[1], owner[1])};
    if (!sh[0] || !sh[1]) {
        for (GLuint s : sh)
            if (s) g_gl.DeleteShader(s);
        return l;
    }
    GLuint prog = g_gl.CreateProgram();
    g_gl.AttachShader(prog, sh[0]);
    g_gl.AttachShader(prog, sh[1]);
    g_gl.LinkProgram(prog);
    g_gl.DeleteShader(sh[0]);
    g_gl.DeleteShader(sh[1]);
    GLint ok = 0, len = 0;
    g_gl.GetProgramiv(prog, kLinkStatus, &ok);
    if (!ok) {
        g_gl.GetProgramiv(prog, kInfoLogLength, &len);
        std::string log(static_cast<size_t>(std::max(len, 1)), '\0');
        g_gl.GetProgramInfoLog(prog, len, nullptr, log.data());
        ReportLog(mod[0] ? label[0] : label[1], mod[0] ? owner[0] : owner[1], log.c_str());
        g_gl.DeleteProgram(prog);
        return l;
    }
    g_gl.UseProgram(prog);
    GLint n = 0;
    g_gl.GetProgramiv(prog, kActiveUniforms, &n);
    for (GLint i = 0; i < n; ++i) {
        char name[128] = {};
        GLint size = 0;
        GLenum type = 0;
        g_gl.GetActiveUniform(prog, static_cast<GLuint>(i), sizeof(name), nullptr, &size, &type, name);
        std::string glName = StripIndex(name);
        if (glName.rfind("gl_", 0) == 0) continue;
        GLint loc = g_gl.GetUniformLocation(prog, name);
        bool found = false;
        for (int s = 0; s < 2 && !found; ++s) {
            std::string cgName = glName;
            if (!mod[s]) {
                auto v = std::find_if(vars[s].begin(), vars[s].end(), [&](const src::CgVar& x) { return StripIndex(x.resource) == glName; });
                if (v == vars[s].end()) continue;
                cgName = v->name;
            }
            src::CGparameter cp = cg.GetNamedParameter(progs[s], cgName.c_str());
            if (!cp) continue;
            found = true;
            if (type >= kSampler1D && type <= kSampler2DRectShadow) {
                GLenum unit = g_cgGLGetTextureEnum ? g_cgGLGetTextureEnum(cp) : kTexture0;
                g_gl.Uniform1i(loc, static_cast<GLint>(unit >= kTexture0 ? unit - kTexture0 : 0));
            } else {
                l.bindings.push_back({loc, type, size, l.arb[s], static_cast<unsigned>(cg.GetParameterResourceIndex(cp))});
            }
        }
        cg.Drain();
        if (!found) LOG_WARN("[shaders] GLSL %s: uniform '%s' has no Cg parameter; it stays 0", label[mod[0] ? 0 : 1].c_str(), name);
    }
    g_gl.UseProgram(g_used);
    l.program = prog;
    l.failed = false;
    LOG_INFO("[shaders] GLSL program %u linked: %s + %s, %zu mirrored uniforms", prog, label[0].c_str(), label[1].c_str(), l.bindings.size());
    jlog::Rec("shader", jlog::Level::Info, "glsl linked").Str("vertex", label[0]).Str("fragment", label[1]).Uint("uniforms", l.bindings.size());
    return l;
}

void Upload() {
    Linked& l = *g_active;
    const Mirror* m[2] = {};
    for (int s = 0; s < 2; ++s) {
        auto it = g_mirror.find(l.arb[s]);
        if (it != g_mirror.end()) m[s] = &it->second;
    }
    uint32_t v0 = m[0] ? m[0]->version : 0, v1 = m[1] ? m[1]->version : 0;
    if (v0 == l.seen[0] && v1 == l.seen[1]) return;
    l.seen[0] = v0;
    l.seen[1] = v1;
    for (const Binding& b : l.bindings) {
        const Mirror* mm = b.arb == l.arb[0] ? m[0] : m[1];
        int regs = b.type == kFloatMat4 ? 4 * b.count : b.count;
        if (!mm || mm->v.size() < (b.index + regs) * 4u) continue;
        const float* r = mm->v.data() + b.index * 4;
        float tmp[4 * 64];
        int n = std::min(b.count, 64);
        switch (b.type) {
        case kFloatMat4:
            // Cg's rows become the GL matrix's columns, so `m * v` in GLSL equals mul(v, m) in Cg.
            g_gl.UniformMatrix4fv(b.loc, std::min(b.count, 16), 0, r);
            break;
        case kFloatVec4:
            g_gl.Uniform4fv(b.loc, n, r);
            break;
        case kFloatVec3:
            for (int i = 0; i < n; ++i) std::copy(r + i * 4, r + i * 4 + 3, tmp + i * 3);
            g_gl.Uniform3fv(b.loc, n, tmp);
            break;
        case kFloatVec2:
            for (int i = 0; i < n; ++i) std::copy(r + i * 4, r + i * 4 + 2, tmp + i * 2);
            g_gl.Uniform2fv(b.loc, n, tmp);
            break;
        case GL_FLOAT:
            for (int i = 0; i < n; ++i) tmp[i] = r[i * 4];
            g_gl.Uniform1fv(b.loc, n, tmp);
            break;
        default:
            break;
        }
    }
}

void Update() {
    src::CGprogram vp = g_cur[0], fp = g_cur[1];
    auto replaced = [](src::CGprogram p) {
        auto it = g_progs.find(p);
        return it != g_progs.end() && it->second.replaced;
    };
    GLuint want = 0;
    Linked* active = nullptr;
    if (vp && fp && g_profileOn[0] && g_profileOn[1] && (replaced(vp) || replaced(fp)) && LoadGl()) {
        auto key = std::make_pair(vp, fp);
        auto it = g_linked.find(key);
        if (it == g_linked.end()) {
            it = g_linked.emplace(key, Link(vp, fp)).first;
            for (src::CGprogram p : {vp, fp})
                if (replaced(p)) g_progs[p].linkedOk = !it->second.failed;
        }
        if (!it->second.failed) {
            want = it->second.program;
            active = &it->second;
        }
    }
    if (want != g_used && g_gl.UseProgram) {
        g_gl.UseProgram(want);
        g_used = want;
    }
    if (active && active != g_active) active->seen[0] = active->seen[1] = ~0u;
    g_active = active;
}

void Forget(src::CGprogram p) {
    for (auto it = g_linked.begin(); it != g_linked.end();) {
        if (it->first.first == p || it->first.second == p) {
            if (&it->second == g_active) g_active = nullptr;
            if (it->second.program && g_gl.DeleteProgram && wglGetCurrentContext()) g_gl.DeleteProgram(it->second.program);
            it = g_linked.erase(it);
        } else {
            ++it;
        }
    }
}

void ScanFiles() {
    g_files.clear();
    modfs::Root roots[128];
    int n = modfs::Roots(roots, 128);
    for (int i = 0; i < n; ++i) {
        std::wstring dir = std::wstring(roots[i].dir) + L"\\shaders";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*.glsl").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string name = src::Narrow(fd.cFileName), stem = name.substr(0, name.size() - 5);
            size_t dot = stem.rfind('.');
            if (dot == std::string::npos || dot == 0) continue;
            std::string file = stem.substr(0, dot), entry = stem.substr(dot + 1);
            if (file.find('.') == std::string::npos) file += ".cg";
            g_files[Key(file, entry)] = {dir + L"\\" + fd.cFileName, roots[i].id, name};
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void __cdecl HkEnableProfile(int profile) {
    g_enableProfile(profile);
    if (profile == engine::CgProfile(0)) g_profileOn[0] = true;
    if (profile == engine::CgProfile(1)) g_profileOn[1] = true;
    Update();
}

void __cdecl HkDisableProfile(int profile) {
    g_disableProfile(profile);
    if (profile == engine::CgProfile(0)) g_profileOn[0] = false;
    if (profile == engine::CgProfile(1)) g_profileOn[1] = false;
    Update();
}

int __cdecl HkLatestProfile(int cls) {
    const src::Cg& cg = CgApi();
    if (cls == kCgGlVertex) return cg.ProfileByName("glslv");
    if (cls == kCgGlFragment) return cg.ProfileByName("glslf");
    return g_latestProfile(cls);
}

template <class T>
bool Chain(const char* name, T hook, T* next) {
    return hub::Interpose(name, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(next));
}
}  // namespace

bool Configure(bool enabled, bool profileExperiment) {
    if (profileExperiment) {
        if (mem::HookIAT("cgGL.dll", "cgGLGetLatestProfile", reinterpret_cast<void*>(&HkLatestProfile), reinterpret_cast<void**>(&g_latestProfile)))
            LOG_WARN("[shaders] GlslProfile=1: every Cg program compiles to GLSL (diagnostic only)");
    }
    if (!enabled) return false;
    ScanFiles();
    if (g_files.empty()) return false;
    HMODULE cggl = GetModuleHandleW(L"cgGL.dll");
    g_cgGLGetProgramID = reinterpret_cast<decltype(g_cgGLGetProgramID)>(GetProcAddress(cggl, "cgGLGetProgramID"));
    g_cgGLGetTextureEnum = reinterpret_cast<decltype(g_cgGLGetTextureEnum)>(GetProcAddress(cggl, "cgGLGetTextureEnum"));
    if (!g_cgGLGetProgramID || !hub::Require("MirageShaders")) {
        LOG_WARN("[shaders] GLSL replacements found but the GL hub is unavailable; they stay inactive");
        return false;
    }
    bool ok = Chain("glNamedProgramLocalParameter4fvEXT", &HkPL4fv, &n_pl4fv) &&
              Chain("glNamedProgramLocalParameters4fvEXT", &HkPLs4fv, &n_pls4fv) &&
              Chain("glNamedProgramLocalParameter4fEXT", &HkPL4f, &n_pl4f) &&
              Chain("glProgramLocalParameter4fvARB", &HkL4fv, &n_l4fv) &&
              Chain("glProgramLocalParameters4fvEXT", &HkLs4fv, &n_ls4fv) && Chain("glBindProgramARB", &HkBindArb, &n_bindArb) &&
              Chain("glDrawElements", &HkDrawElements, &n_drawElements) && Chain("glDrawArrays", &HkDrawArrays, &n_drawArrays) &&
              Chain("glDrawRangeElements", &HkDrawRange, &n_drawRange);
    ok = ok && mem::HookIAT("cgGL.dll", "cgGLEnableProfile", reinterpret_cast<void*>(&HkEnableProfile), reinterpret_cast<void**>(&g_enableProfile));
    ok = ok && mem::HookIAT("cgGL.dll", "cgGLDisableProfile", reinterpret_cast<void*>(&HkDisableProfile), reinterpret_cast<void**>(&g_disableProfile));
    if (!ok) {
        LOG_WARN("[shaders] GLSL replacement hooks incomplete; replacements stay inactive");
        return false;
    }
    g_installed = true;
    for (const auto& [key, f] : g_files) LOG_INFO("[shaders] GLSL replacement %s from %s (experimental)", f.name.c_str(), f.owner.c_str());
    return true;
}

bool Installed() { return g_installed; }

bool Has(const std::string& file, const std::string& entry) { return g_installed && g_files.count(Key(file, entry)) != 0; }

void OnCreate(src::CGprogram p, const std::string& file, const std::string& entry, int stage, const std::wstring& vdir) {
    if (!g_installed) return;
    Forget(p);
    g_progs[p] = {file, entry, stage, vdir, Has(file, entry), false};
}

void OnBind(src::CGprogram p) {
    auto it = g_progs.find(p);
    if (it == g_progs.end()) return;
    g_cur[it->second.stage] = p;
    Update();
}

void OnFileChanged(const std::wstring& path) {
    if (!g_installed) {
        LOG_INFO("[shaders] %s: GLSL replacements added after start need a restart", src::Narrow(path).c_str());
        return;
    }
    ScanFiles();
    std::vector<src::CGprogram> all;
    for (auto& [p, prog] : g_progs) {
        prog.replaced = Has(prog.file, prog.entry);
        prog.linkedOk = false;
        all.push_back(p);
    }
    for (src::CGprogram p : all) Forget(p);
    Update();
    LOG_INFO("[shaders] GLSL replacements rescanned: %zu files", g_files.size());
}

bool Active(const std::string& file, const std::string& entry) {
    for (const auto& [p, prog] : g_progs)
        if (prog.replaced && prog.linkedOk && src::IEquals(prog.file, file) && prog.entry == entry) return true;
    return false;
}

std::string Owner(const std::string& file, const std::string& entry) {
    auto f = g_files.find(Key(file, entry));
    return f != g_files.end() ? f->second.owner : std::string();
}

uint32_t ActiveCount() {
    uint32_t n = 0;
    for (const auto& [p, prog] : g_progs)
        if (prog.replaced && prog.linkedOk) ++n;
    return n;
}
}  // namespace melange::mirage::shaders::glsl
