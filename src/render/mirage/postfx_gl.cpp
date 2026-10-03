// Post-FX GL runner: compiles effect passes, owns the render targets and draws the chain.
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#pragma warning(push, 0)
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"
#pragma warning(pop)

#include "render/mirage/postfx_internal.h"

namespace melange::mirage::postfx {
std::atomic<int> g_activeCodeHandle{0};
std::atomic<unsigned long> g_activeCodeThread{0};
namespace {
constexpr GLenum kFRAGMENT_SHADER = 0x8B30, kVERTEX_SHADER = 0x8B31, kCOMPILE_STATUS = 0x8B81, kLINK_STATUS = 0x8B82,
                 kINFO_LOG_LENGTH = 0x8B84, kACTIVE_UNIFORMS = 0x8B86, kSHADING_LANGUAGE_VERSION = 0x8B8C,
                 kFRAMEBUFFER = 0x8D40, kCOLOR_ATTACHMENT0 = 0x8CE0, kFRAMEBUFFER_COMPLETE = 0x8CD5, kTEXTURE0 = 0x84C0,
                 kCLAMP_TO_EDGE = 0x812F, kTEXTURE_MAX_LEVEL = 0x813D, kRGBA16F = 0x881A, kR8 = 0x8229, kRG8 = 0x822B,
                 kRED = 0x1903, kRG = 0x8227, kTEXTURE_COMPARE_MODE = 0x884C, kTIME_ELAPSED = 0x88BF,
                 kQUERY_RESULT = 0x8866, kQUERY_RESULT_AVAILABLE = 0x8867, kUNPACK_ROW_LENGTH = 0x0CF2;
constexpr GLenum kFLOAT = 0x1406, kFLOAT_VEC2 = 0x8B50, kFLOAT_VEC3 = 0x8B51, kFLOAT_VEC4 = 0x8B52, kINT = 0x1404,
                 kINT_VEC2 = 0x8B53, kINT_VEC3 = 0x8B54, kINT_VEC4 = 0x8B55, kBOOL = 0x8B56, kBOOL_VEC2 = 0x8B57,
                 kBOOL_VEC3 = 0x8B58, kBOOL_VEC4 = 0x8B59, kFLOAT_MAT4 = 0x8B5C, kSAMPLER_2D = 0x8B5E,
                 kSAMPLER_2D_SHADOW = 0x8B62;

struct Procs {
    HGLRC ctx = nullptr;
    bool ok = false, float16 = false, rg = false, timer = false;
    int glsl = 0;
    std::string why;
    GLuint(APIENTRY* CreateShader)(GLenum);
    void(APIENTRY* ShaderSource)(GLuint, GLsizei, const char* const*, const GLint*);
    void(APIENTRY* CompileShader)(GLuint);
    void(APIENTRY* GetShaderiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    void(APIENTRY* DeleteShader)(GLuint);
    GLuint(APIENTRY* CreateProgram)();
    void(APIENTRY* AttachShader)(GLuint, GLuint);
    void(APIENTRY* BindAttribLocation)(GLuint, GLuint, const char*);
    void(APIENTRY* LinkProgram)(GLuint);
    void(APIENTRY* GetProgramiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, char*);
    void(APIENTRY* DeleteProgram)(GLuint);
    void(APIENTRY* UseProgram)(GLuint);
    GLint(APIENTRY* GetUniformLocation)(GLuint, const char*);
    void(APIENTRY* GetActiveUniform)(GLuint, GLuint, GLsizei, GLsizei*, GLint*, GLenum*, char*);
    void(APIENTRY* Uniform1i)(GLint, GLint);
    void(APIENTRY* Uniform2i)(GLint, GLint, GLint);
    void(APIENTRY* Uniform3i)(GLint, GLint, GLint, GLint);
    void(APIENTRY* Uniform4i)(GLint, GLint, GLint, GLint, GLint);
    void(APIENTRY* Uniform1f)(GLint, GLfloat);
    void(APIENTRY* Uniform2f)(GLint, GLfloat, GLfloat);
    void(APIENTRY* Uniform3f)(GLint, GLfloat, GLfloat, GLfloat);
    void(APIENTRY* Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
    void(APIENTRY* UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*);
    void(APIENTRY* VertexAttrib2f)(GLuint, GLfloat, GLfloat);
    void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
    void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
    void(APIENTRY* BindFramebuffer)(GLenum, GLuint);
    void(APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum(APIENTRY* CheckFramebufferStatus)(GLenum);
    void(APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void(APIENTRY* ActiveTexture)(GLenum);
    void(APIENTRY* GenQueries)(GLsizei, GLuint*);
    void(APIENTRY* DeleteQueries)(GLsizei, const GLuint*);
    void(APIENTRY* BeginQuery)(GLenum, GLuint);
    void(APIENTRY* EndQuery)(GLenum);
    void(APIENTRY* GetQueryObjectiv)(GLuint, GLenum, GLint*);
    void(APIENTRY* GetQueryObjectui64v)(GLuint, GLenum, unsigned long long*);
};
Procs g_p;

bool HasExt(const char* exts, const char* name) {
    if (!exts) return false;
    size_t len = strlen(name);
    for (const char* p = exts; (p = strstr(p, name)) != nullptr; p += len)
        if ((p == exts || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0)) return true;
    return false;
}

template <class T>
bool Proc(T& fn, const char* a, const char* b = nullptr) {
    fn = nullptr;
    for (const char* n : {a, b}) {
        if (!n) continue;
        PROC p = wglGetProcAddress(n);
        auto v = reinterpret_cast<intptr_t>(p);
        if (p && v != 1 && v != 2 && v != 3 && v != -1) {
            fn = reinterpret_cast<T>(p);
            return true;
        }
    }
    return false;
}

const Procs& P() {
    HGLRC ctx = wglGetCurrentContext();
    if (ctx == g_p.ctx) return g_p;
    Procs p{};
    p.ctx = ctx;
    if (!ctx) {
        p.why = "no GL context";
        g_p = p;
        return g_p;
    }
    int major = 1, minor = 0;
    if (const char* v = reinterpret_cast<const char*>(glGetString(GL_VERSION))) sscanf(v, "%d.%d", &major, &minor);
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    bool gl3 = major >= 3;
    p.glsl = major >= 2 ? ParseGlslVersion(reinterpret_cast<const char*>(glGetString(kSHADING_LANGUAGE_VERSION))) : 0;
    bool ok = p.glsl >= 110;
    ok &= Proc(p.CreateShader, "glCreateShader") && Proc(p.ShaderSource, "glShaderSource") &&
          Proc(p.CompileShader, "glCompileShader") && Proc(p.GetShaderiv, "glGetShaderiv") &&
          Proc(p.GetShaderInfoLog, "glGetShaderInfoLog") && Proc(p.DeleteShader, "glDeleteShader") &&
          Proc(p.CreateProgram, "glCreateProgram") && Proc(p.AttachShader, "glAttachShader") &&
          Proc(p.BindAttribLocation, "glBindAttribLocation") && Proc(p.LinkProgram, "glLinkProgram") &&
          Proc(p.GetProgramiv, "glGetProgramiv") && Proc(p.GetProgramInfoLog, "glGetProgramInfoLog") &&
          Proc(p.DeleteProgram, "glDeleteProgram") && Proc(p.UseProgram, "glUseProgram") &&
          Proc(p.GetUniformLocation, "glGetUniformLocation") && Proc(p.GetActiveUniform, "glGetActiveUniform") &&
          Proc(p.Uniform1i, "glUniform1i") && Proc(p.Uniform2i, "glUniform2i") && Proc(p.Uniform3i, "glUniform3i") &&
          Proc(p.Uniform4i, "glUniform4i") && Proc(p.Uniform1f, "glUniform1f") && Proc(p.Uniform2f, "glUniform2f") &&
          Proc(p.Uniform3f, "glUniform3f") && Proc(p.Uniform4f, "glUniform4f") &&
          Proc(p.UniformMatrix4fv, "glUniformMatrix4fv") && Proc(p.VertexAttrib2f, "glVertexAttrib2f") &&
          Proc(p.ActiveTexture, "glActiveTexture", "glActiveTextureARB");
    bool fbo = Proc(p.GenFramebuffers, "glGenFramebuffers", "glGenFramebuffersEXT") &&
               Proc(p.DeleteFramebuffers, "glDeleteFramebuffers", "glDeleteFramebuffersEXT") &&
               Proc(p.BindFramebuffer, "glBindFramebuffer", "glBindFramebufferEXT") &&
               Proc(p.FramebufferTexture2D, "glFramebufferTexture2D", "glFramebufferTexture2DEXT") &&
               Proc(p.CheckFramebufferStatus, "glCheckFramebufferStatus", "glCheckFramebufferStatusEXT");
    Proc(p.BlitFramebuffer, "glBlitFramebuffer", "glBlitFramebufferEXT");
    p.float16 = gl3 || HasExt(ext, "GL_ARB_texture_float");
    p.rg = gl3 || HasExt(ext, "GL_ARB_texture_rg");
    p.timer = (major > 3 || (major == 3 && minor >= 3) || HasExt(ext, "GL_ARB_timer_query")) &&
              Proc(p.GenQueries, "glGenQueries") && Proc(p.DeleteQueries, "glDeleteQueries") &&
              Proc(p.BeginQuery, "glBeginQuery") && Proc(p.EndQuery, "glEndQuery") &&
              Proc(p.GetQueryObjectiv, "glGetQueryObjectiv") && Proc(p.GetQueryObjectui64v, "glGetQueryObjectui64v");
    p.ok = ok && fbo;
    if (!ok) p.why = "GLSL 1.10+ (OpenGL 2.0) is not available";
    else if (!fbo) p.why = "framebuffer objects are not available";
    while (glGetError() != GL_NO_ERROR) {}
    g_p = p;
    return g_p;
}

// ---------------------------------------------------------------- context ownership
HGLRC g_owner = nullptr;
int g_gen = 1, g_foreign = 0;

// ---------------------------------------------------------------- targets
struct Shared {
    int gen = 0;
    Target ping, pong;
    unsigned outFbo = 0, outTex = 0;
    int outW = 0, outH = 0;  // a resize may recreate the engine texture under the same name
    unsigned resolveFbo = 0, resolveColor = 0, resolveDepth = 0;
    int resolveW = 0, resolveH = 0;
    unsigned copyProg = 0;
    std::string copyError;
};
Shared g_s;

void GetFormat(Format f, GLint* internal, GLenum* fmt, GLenum* type) {
    const Procs& p = P();
    *type = GL_UNSIGNED_BYTE;
    if (f == Format::Rgba16f && p.float16) {
        *internal = kRGBA16F;
        *fmt = GL_RGBA;
        *type = GL_FLOAT;
    } else if (f == Format::R8 && p.rg) {
        *internal = kR8;
        *fmt = kRED;
    } else if (f == Format::Rg8 && p.rg) {
        *internal = kRG8;
        *fmt = kRG;
    } else {
        *internal = GL_RGBA8;
        *fmt = GL_RGBA;
    }
}

void DeleteTarget(Target& t) {
    const Procs& p = P();
    if (t.fbo && p.DeleteFramebuffers) p.DeleteFramebuffers(1, &t.fbo);
    if (t.tex) glDeleteTextures(1, &t.tex);
    t = Target{};
}

bool EnsureTarget(Target& t, int w, int h, Format f) {
    if (t.tex && t.w == w && t.h == h && t.format == f) return true;
    DeleteTarget(t);
    const Procs& p = P();
    GLint internal;
    GLenum fmt, type;
    GetFormat(f, &internal, &fmt, &type);
    glGenTextures(1, &t.tex);
    glBindTexture(GL_TEXTURE_2D, t.tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kCLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kCLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, kTEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, nullptr);
    p.GenFramebuffers(1, &t.fbo);
    p.BindFramebuffer(kFRAMEBUFFER, t.fbo);
    p.FramebufferTexture2D(kFRAMEBUFFER, kCOLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
    bool ok = p.CheckFramebufferStatus(kFRAMEBUFFER) == kFRAMEBUFFER_COMPLETE;
    t.w = w;
    t.h = h;
    t.format = f;
    if (!ok) DeleteTarget(t);
    return ok;
}

// ---------------------------------------------------------------- shaders
std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool ReadAll(const std::wstring& path, std::string* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart < (64 << 20);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = ReadFile(h, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) && got == out->size();
    }
    CloseHandle(h);
    return ok;
}

bool ReadInclude(const std::string& name, std::string* text, void* user) {
    return ReadAll(*static_cast<const std::wstring*>(user) + L"\\" + Widen(name), text);
}

std::string InfoLog(GLuint obj, bool program) {
    const Procs& p = P();
    GLint len = 0;
    (program ? p.GetProgramiv : p.GetShaderiv)(obj, kINFO_LOG_LENGTH, &len);
    std::string s(len > 0 ? static_cast<size_t>(len) : 0, '\0');
    if (len > 0) (program ? p.GetProgramInfoLog : p.GetShaderInfoLog)(obj, len, nullptr, s.data());
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

GLuint CompileStage(GLenum type, const std::string& text, std::string* log) {
    const Procs& p = P();
    GLuint sh = p.CreateShader(type);
    const char* src = text.c_str();
    p.ShaderSource(sh, 1, &src, nullptr);
    p.CompileShader(sh);
    GLint ok = 0;
    p.GetShaderiv(sh, kCOMPILE_STATUS, &ok);
    if (!ok) {
        *log = InfoLog(sh, false);
        p.DeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint Link(const std::string& vsText, const Source& fs, std::string* error) {
    const Procs& p = P();
    std::string log;
    GLuint vs = CompileStage(kVERTEX_SHADER, vsText, &log);
    if (!vs) {
        *error = "vertex shader: " + log;
        return 0;
    }
    GLuint f = CompileStage(kFRAGMENT_SHADER, fs.text, &log);
    if (!f) {
        p.DeleteShader(vs);
        *error = fs.files[0] + ": " + log;
        if (fs.files.size() > 1) {
            *error += "\n(source numbers:";
            for (size_t i = 0; i < fs.files.size(); ++i) *error += " " + std::to_string(i) + "=" + fs.files[i];
            *error += ")";
        }
        return 0;
    }
    GLuint prog = p.CreateProgram();
    p.AttachShader(prog, vs);
    p.AttachShader(prog, f);
    p.BindAttribLocation(prog, 0, "mg_pos");
    p.LinkProgram(prog);
    p.DeleteShader(vs);
    p.DeleteShader(f);
    GLint ok = 0;
    p.GetProgramiv(prog, kLINK_STATUS, &ok);
    if (!ok) {
        *error = fs.files[0] + ": link: " + InfoLog(prog, true);
        p.DeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool IsSampler(GLenum t) { return t == kSAMPLER_2D || t == kSAMPLER_2D_SHADOW; }

void Set(const Uniform& u, const float* v, int n) {
    if (u.loc < 0) return;
    const Procs& p = P();
    auto f = [&](int i) { return i < n ? v[i] : 0.f; };
    auto k = [&](int i) { return static_cast<GLint>(std::lround(f(i))); };
    switch (u.type) {
    case kFLOAT: p.Uniform1f(u.loc, f(0)); break;
    case kFLOAT_VEC2: p.Uniform2f(u.loc, f(0), f(1)); break;
    case kFLOAT_VEC3: p.Uniform3f(u.loc, f(0), f(1), f(2)); break;
    case kFLOAT_VEC4: p.Uniform4f(u.loc, f(0), f(1), f(2), n > 3 ? v[3] : 1.f); break;
    case kINT:
    case kBOOL: p.Uniform1i(u.loc, k(0)); break;
    case kINT_VEC2:
    case kBOOL_VEC2: p.Uniform2i(u.loc, k(0), k(1)); break;
    case kINT_VEC3:
    case kBOOL_VEC3: p.Uniform3i(u.loc, k(0), k(1), k(2)); break;
    case kINT_VEC4:
    case kBOOL_VEC4: p.Uniform4i(u.loc, k(0), k(1), k(2), k(3)); break;
    case kFLOAT_MAT4:
        if (n >= 16) p.UniformMatrix4fv(u.loc, 1, GL_FALSE, v);
        break;
    default: break;
    }
}

void DrawTriangle() {
    const Procs& p = P();
    glBegin(GL_TRIANGLES);
    p.VertexAttrib2f(0, -1.f, -1.f);
    p.VertexAttrib2f(0, 3.f, -1.f);
    p.VertexAttrib2f(0, -1.f, 3.f);
    glEnd();
}

bool EnsureShared() {
    if (g_s.gen != g_gen) g_s = Shared{g_gen};
    if (g_s.copyProg || !g_s.copyError.empty()) return g_s.copyProg != 0;
    const char* kCopy = "uniform sampler2D mg_scene;\nvarying vec2 mg_uv;\nvoid main() { gl_FragColor = texture2D(mg_scene, mg_uv); }\n";
    Source src;
    std::string err;
    if (BuildFragment(kCopy, "copy", {}, nullptr, nullptr, &src, &err)) g_s.copyProg = Link(BuildVertex(120, ""), src, &err);
    if (!g_s.copyProg) g_s.copyError = err.empty() ? "copy shader failed" : err;
    return g_s.copyProg != 0;
}

bool LoadPng(const std::wstring& path, const TextureDesc& t, unsigned* out, std::string* error) {
    std::string bytes;
    if (!ReadAll(path, &bytes)) {
        *error = "cannot read " + t.file;
        return false;
    }
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &w,
                                        &h, &comp, 4);
    if (!px) {
        *error = t.file + ": " + (stbi_failure_reason() ? stbi_failure_reason() : "not a PNG");
        return false;
    }
    glGenTextures(1, out);
    glBindTexture(GL_TEXTURE_2D, *out);
    GLint filter = t.linear ? GL_LINEAR : GL_NEAREST;
    GLint wrap = t.repeat ? GL_REPEAT : kCLAMP_TO_EDGE;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    glTexParameteri(GL_TEXTURE_2D, kTEXTURE_MAX_LEVEL, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(kUNPACK_ROW_LENGTH, 0);
    // rows as stored (top row first), so v = 0 is the image's top row
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return true;
}

void Forget(Effect& e) {
    e.passes.clear();
    e.textures.clear();
    for (int i = 0; i < 3; ++i) {
        e.queries[i] = 0;
        e.queryIssued[i] = false;
    }
    e.glGen = g_gen;
}

bool Fail(Effect& e, const std::string& why) {
    Release(e);
    e.failed = true;
    e.error = why;
    return false;
}

bool NeedsDepth(const Effect& e) {
    for (const PassDesc& p : e.desc.passes)
        for (const Input& in : p.inputs)
            if (in.kind == InputKind::Depth) return true;
    return false;
}

bool CallCode(melange::postfx::PassFn fn, const melange::postfx::PassContext& c, void* user, unsigned long* code) {
    __try {
        fn(c, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *code = GetExceptionCode();
        return false;
    }
}

void BeginTimer(Effect& e, bool on) {
    const Procs& p = P();
    if (!on || !p.timer) return;
    int slot = e.queryHead;
    if (!e.queries[0]) p.GenQueries(3, e.queries);
    if (e.queryIssued[slot]) {
        GLint avail = 0;
        p.GetQueryObjectiv(e.queries[slot], kQUERY_RESULT_AVAILABLE, &avail);
        if (avail) {
            unsigned long long ns = 0;
            p.GetQueryObjectui64v(e.queries[slot], kQUERY_RESULT, &ns);
            e.gpuMs = static_cast<double>(ns) / 1e6;
        }
    }
    p.BeginQuery(kTIME_ELAPSED, e.queries[slot]);
}

void EndTimer(Effect& e, bool on) {
    const Procs& p = P();
    if (!on || !p.timer) return;
    p.EndQuery(kTIME_ELAPSED);
    e.queryIssued[e.queryHead] = true;
    e.queryHead = (e.queryHead + 1) % 3;
}

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
}
}  // namespace

bool OwnContext() {
    HGLRC c = wglGetCurrentContext();
    if (!c) return false;
    if (c == g_owner) {
        g_foreign = 0;
        return true;
    }
    if (g_owner && ++g_foreign < 30) return false;
    g_owner = c;
    g_foreign = 0;
    ++g_gen;
    return true;
}

int ContextGeneration() { return g_gen; }

bool GlReady(std::string* why) {
    const Procs& p = P();
    if (!p.ok && why) *why = p.why;
    return p.ok;
}

int GlslVersion() { return P().glsl; }

void Release(Effect& e) {
    const Procs& p = P();
    if (e.glGen != g_gen) {
        Forget(e);
        return;
    }
    for (PassGl& pass : e.passes) {
        if (pass.program && p.DeleteProgram) p.DeleteProgram(pass.program);
        DeleteTarget(pass.target);
    }
    e.passes.clear();
    if (!e.textures.empty()) glDeleteTextures(static_cast<GLsizei>(e.textures.size()), e.textures.data());
    e.textures.clear();
    if (e.queries[0] && p.DeleteQueries) p.DeleteQueries(3, e.queries);
    for (int i = 0; i < 3; ++i) {
        e.queries[i] = 0;
        e.queryIssued[i] = false;
    }
}

bool Compile(Effect& e) {
    Release(e);
    e.glGen = g_gen;
    e.dirty = false;
    e.failed = false;
    e.error.clear();
    e.gpuMs = -1;
    if (e.code) return true;
    std::string why;
    if (!GlReady(&why)) return Fail(e, why);
    const Procs& p = P();
    for (size_t i = 0; i < e.desc.passes.size(); ++i) {
        const PassDesc& pd = e.desc.passes[i];
        std::string text, err;
        if (!ReadAll(e.dir + L"\\" + Widen(pd.shader), &text)) return Fail(e, "cannot read " + pd.shader);
        Source src;
        if (!BuildFragment(text, pd.shader, pd.defines, &ReadInclude, &e.dir, &src, &err)) return Fail(e, err);
        if (src.version > p.glsl) {
            char b[96];
            snprintf(b, sizeof b, ": needs GLSL %d, the driver has %d", src.version, p.glsl);
            return Fail(e, pd.shader + b);
        }
        PassGl pass;
        pass.program = Link(BuildVertex(src.version, src.profile), src, &err);
        if (!pass.program) return Fail(e, err);
        e.passes.push_back(pass);
        PassGl& pg = e.passes.back();
        pg.params.resize(e.desc.params.size());
        p.UseProgram(pg.program);
        GLint count = 0;
        p.GetProgramiv(pg.program, kACTIVE_UNIFORMS, &count);
        int unit = 0;
        for (GLint u = 0; u < count; ++u) {
            char name[256];
            GLint size = 0;
            GLenum type = 0;
            p.GetActiveUniform(pg.program, static_cast<GLuint>(u), sizeof name, nullptr, &size, &type, name);
            if (char* br = strchr(name, '[')) *br = 0;
            Uniform un{p.GetUniformLocation(pg.program, name), type};
            if (un.loc < 0) continue;
            std::string n = name;
            if (IsSampler(type)) {
                bool found = false;
                for (const Input& in : pd.inputs)
                    if (SamplerName(in) == n) {
                        pg.samplers.push_back({unit, in});
                        p.Uniform1i(un.loc, unit++);
                        found = true;
                    }
                if (!found) return Fail(e, pd.shader + ": sampler '" + n + "' is not listed in inputs= of [pass." + pd.name + "]");
                continue;
            }
            if (n == "mg_resolution") pg.resolution = un;
            else if (n == "mg_sceneResolution") pg.sceneResolution = un;
            else if (n == "mg_renderScale") pg.renderScale = un;
            else if (n == "mg_time") pg.time = un;
            else if (n == "mg_frame") pg.frame = un;
            else if (n == "mg_proj") pg.proj = un;
            else if (n == "mg_invProj") pg.invProj = un;
            else if (n == "mg_view") pg.view = un;
            else if (n == "mg_nearFar") pg.nearFar = un;
            else if (n.rfind("p_", 0) == 0)
                for (size_t k = 0; k < e.desc.params.size(); ++k)
                    if (e.desc.params[k].name == n.substr(2)) pg.params[k] = un;
        }
        p.UseProgram(0);
    }
    for (const TextureDesc& t : e.desc.textures) {
        unsigned tex = 0;
        std::string err;
        if (!LoadPng(e.dir + L"\\" + Widen(t.file), t, &tex, &err)) return Fail(e, err);
        e.textures.push_back(tex);
    }
    while (glGetError() != GL_NO_ERROR) {}
    return true;
}

RunResult Run(const std::vector<Effect*>& chain, const FrameInput& in) {
    RunResult r;
    const Procs& p = P();
    if (!p.ok || in.w <= 0 || in.h <= 0 || !in.sceneColor) return r;
    std::vector<Effect*> run;
    for (Effect* e : chain) {
        e->skipReason.clear();
        if (e->failed || e->dirty || e->missing || e->glGen != g_gen) continue;
        if (!e->code && e->passes.empty()) continue;
        if (!e->code && !in.sceneDepth && NeedsDepth(*e)) {
            e->skipReason = "no depth texture at this stage";
            continue;
        }
        run.push_back(e);
    }
    if (run.empty() || !EnsureShared()) return r;
    if (!EnsureTarget(g_s.ping, in.w, in.h, Format::Rgba8) || !EnsureTarget(g_s.pong, in.w, in.h, Format::Rgba8)) return r;
    if (!g_s.outFbo) p.GenFramebuffers(1, &g_s.outFbo);
    if (g_s.outTex != in.sceneColor || g_s.outW != in.w || g_s.outH != in.h) {
        p.BindFramebuffer(kFRAMEBUFFER, g_s.outFbo);
        p.FramebufferTexture2D(kFRAMEBUFFER, kCOLOR_ATTACHMENT0, GL_TEXTURE_2D, in.sceneColor, 0);
        if (p.CheckFramebufferStatus(kFRAMEBUFFER) != kFRAMEBUFFER_COMPLETE) {
            p.FramebufferTexture2D(kFRAMEBUFFER, kCOLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
            g_s.outTex = 0;
            return r;
        }
        g_s.outTex = in.sceneColor;
        g_s.outW = in.w;
        g_s.outH = in.h;
    }
    Target out{in.sceneColor, g_s.outFbo, in.w, in.h, Format::Rgba8};

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_ALPHA_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glClearColor(0.f, 0.f, 0.f, 0.f);

    // the engine's depth texture may compare (shadow sampling) or use a mipmap filter it lacks the levels for
    GLint depthCompare = 0, depthMin = 0;
    if (in.sceneDepth) {
        p.ActiveTexture(kTEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in.sceneDepth);
        glGetTexParameteriv(GL_TEXTURE_2D, kTEXTURE_COMPARE_MODE, &depthCompare);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &depthMin);
        glTexParameteri(GL_TEXTURE_2D, kTEXTURE_COMPARE_MODE, GL_NONE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    }

    p.BindFramebuffer(kFRAMEBUFFER, g_s.ping.fbo);
    glViewport(0, 0, in.w, in.h);
    p.UseProgram(g_s.copyProg);
    p.ActiveTexture(kTEXTURE0);
    glBindTexture(GL_TEXTURE_2D, in.sceneColor);
    DrawTriangle();

    const float sceneRes[4] = {static_cast<float>(in.w), static_cast<float>(in.h), 1.f / in.w, 1.f / in.h};
    const float frameF = static_cast<float>(in.frame % 16777216u);
    const Target* cur = &g_s.ping;
    for (size_t i = 0; i < run.size(); ++i) {
        Effect& e = *run[i];
        const bool lastEffect = i + 1 == run.size();
        const Target* dst = lastEffect ? &out : (cur == &g_s.ping ? &g_s.pong : &g_s.ping);
        double t0 = Now();
        BeginTimer(e, in.gpuTimers);
        if (e.code) {
            // Removed by an earlier pass's own callback in this same chain (AddCodePass/RemoveCodePass are code,
            // not effect.ini, so `chain` was snapshotted once per stage and may already be stale mid-loop).
            if (e.removed.load(std::memory_order_acquire)) {
                EndTimer(e, in.gpuTimers);
                continue;
            }
            p.BindFramebuffer(kFRAMEBUFFER, dst->fbo);
            glViewport(0, 0, in.w, in.h);
            if (lastEffect && in.splitCompare) {
                glEnable(GL_SCISSOR_TEST);
                glScissor(in.w / 2, 0, in.w - in.w / 2, in.h);
            }
            p.UseProgram(0);
            p.ActiveTexture(kTEXTURE0);
            melange::postfx::PassContext ctx{cur->tex, in.sceneDepth, dst->fbo, in.w, in.h, in.proj, in.invProj,
                                             in.timeSec, in.frame};
            g_activeCodeHandle.store(e.handle, std::memory_order_release);
            g_activeCodeThread.store(GetCurrentThreadId(), std::memory_order_release);
            unsigned long code = 0;
            bool ok = CallCode(e.fn, ctx, e.user, &code);
            g_activeCodeHandle.store(0, std::memory_order_release);
            if (!ok) {
                char b[64];
                snprintf(b, sizeof b, "raised exception 0x%08lx", code);
                e.failed = true;
                e.error = b;
            }
            ++r.passes;
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glClearColor(0.f, 0.f, 0.f, 0.f);
        } else {
            for (size_t j = 0; j < e.passes.size(); ++j) {
                PassGl& pg = e.passes[j];
                const PassDesc& pd = e.desc.passes[j];
                const bool lastPass = j + 1 == e.passes.size();
                const Target* t = dst;
                if (!lastPass) {
                    int w = std::max(1, static_cast<int>(std::lround(in.w * pd.scale)));
                    int h = std::max(1, static_cast<int>(std::lround(in.h * pd.scale)));
                    if (!EnsureTarget(pg.target, w, h, pd.format)) {
                        e.failed = true;
                        e.error = "cannot create the " + std::string(FormatName(pd.format)) + " target of [pass." + pd.name + "]";
                        break;
                    }
                    t = &pg.target;
                }
                p.BindFramebuffer(kFRAMEBUFFER, t->fbo);
                glViewport(0, 0, t->w, t->h);
                if (t != &out) glClear(GL_COLOR_BUFFER_BIT);
                else if (in.splitCompare) {
                    glEnable(GL_SCISSOR_TEST);
                    glScissor(in.w / 2, 0, in.w - in.w / 2, in.h);
                }
                p.UseProgram(pg.program);
                const float res[4] = {static_cast<float>(t->w), static_cast<float>(t->h), 1.f / t->w, 1.f / t->h};
                Set(pg.resolution, res, 4);
                Set(pg.sceneResolution, sceneRes, 4);
                Set(pg.renderScale, in.renderScale, 2);
                Set(pg.time, &in.timeSec, 1);
                Set(pg.frame, &frameF, 1);
                Set(pg.proj, in.proj, 16);
                Set(pg.invProj, in.invProj, 16);
                Set(pg.view, in.view, 16);
                Set(pg.nearFar, in.nearFar, 2);
                for (size_t k = 0; k < pg.params.size() && k < e.values.size(); ++k)
                    Set(pg.params[k], e.values[k].data(), e.desc.params[k].n);
                for (const PassGl::Sampler& s : pg.samplers) {
                    unsigned tex = 0;
                    switch (s.input.kind) {
                    case InputKind::Scene: tex = cur->tex; break;
                    case InputKind::Depth: tex = in.sceneDepth; break;
                    case InputKind::Prev: tex = j == 0 ? cur->tex : e.passes[j - 1].target.tex; break;
                    case InputKind::Pass:
                        for (size_t k = 0; k < j; ++k)
                            if (e.desc.passes[k].name == s.input.name) tex = e.passes[k].target.tex;
                        break;
                    case InputKind::Texture:
                        for (size_t k = 0; k < e.desc.textures.size() && k < e.textures.size(); ++k)
                            if (e.desc.textures[k].name == s.input.name) tex = e.textures[k];
                        break;
                    }
                    p.ActiveTexture(kTEXTURE0 + static_cast<GLenum>(s.unit));
                    glBindTexture(GL_TEXTURE_2D, tex);
                }
                p.ActiveTexture(kTEXTURE0);
                DrawTriangle();
                glDisable(GL_SCISSOR_TEST);
                ++r.passes;
            }
        }
        EndTimer(e, in.gpuTimers);
        e.cpuMs = Now() - t0;
        ++e.runs;
        ++r.effects;
        cur = dst;
    }
    p.UseProgram(0);
    if (in.sceneDepth) {
        p.ActiveTexture(kTEXTURE0);
        glBindTexture(GL_TEXTURE_2D, in.sceneDepth);
        glTexParameteri(GL_TEXTURE_2D, kTEXTURE_COMPARE_MODE, depthCompare);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, depthMin);
    }
    for (GLenum err; (err = glGetError()) != GL_NO_ERROR && r.glErrors < 64;) ++r.glErrors;
    return r;
}

namespace {
constexpr GLenum kREAD_FRAMEBUFFER = 0x8CA8, kDRAW_FRAMEBUFFER = 0x8CA9, kDEPTH_STENCIL_ATTACHMENT = 0x821A,
                 kDEPTH24_STENCIL8 = 0x88F0, kDEPTH_STENCIL = 0x84F9, kUNSIGNED_INT_24_8 = 0x84FA;

void DeleteResolve() {
    const Procs& p = P();
    if (g_s.resolveFbo && p.DeleteFramebuffers) p.DeleteFramebuffers(1, &g_s.resolveFbo);
    unsigned tex[2] = {g_s.resolveColor, g_s.resolveDepth};
    if (tex[0] || tex[1]) glDeleteTextures(2, tex);
    g_s.resolveFbo = g_s.resolveColor = g_s.resolveDepth = 0;
    g_s.resolveW = g_s.resolveH = 0;
}

unsigned NewTexture(GLint internal, GLenum fmt, GLenum type, int w, int h) {
    unsigned t = 0;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kCLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kCLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, kTEXTURE_MAX_LEVEL, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, fmt, type, nullptr);
    return t;
}

bool EnsureResolve(int w, int h) {
    if (g_s.gen != g_gen) g_s = Shared{g_gen};
    if (g_s.resolveFbo && g_s.resolveW == w && g_s.resolveH == h) return true;
    DeleteResolve();
    const Procs& p = P();
    g_s.resolveColor = NewTexture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, w, h);
    g_s.resolveDepth = NewTexture(kDEPTH24_STENCIL8, kDEPTH_STENCIL, kUNSIGNED_INT_24_8, w, h);
    p.GenFramebuffers(1, &g_s.resolveFbo);
    p.BindFramebuffer(kFRAMEBUFFER, g_s.resolveFbo);
    p.FramebufferTexture2D(kFRAMEBUFFER, kCOLOR_ATTACHMENT0, GL_TEXTURE_2D, g_s.resolveColor, 0);
    p.FramebufferTexture2D(kFRAMEBUFFER, kDEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, g_s.resolveDepth, 0);
    g_s.resolveW = w;
    g_s.resolveH = h;
    if (p.CheckFramebufferStatus(kFRAMEBUFFER) == kFRAMEBUFFER_COMPLETE) return true;
    DeleteResolve();
    return false;
}
}  // namespace

bool ResolveMultisample(unsigned fbo, int w, int h, FrameInput* in) {
    const Procs& p = P();
    if (!p.ok || !p.BlitFramebuffer || !fbo || w <= 0 || h <= 0 || !EnsureResolve(w, h)) return false;
    while (glGetError() != GL_NO_ERROR) {}
    p.BindFramebuffer(kREAD_FRAMEBUFFER, fbo);
    p.BindFramebuffer(kDRAW_FRAMEBUFFER, g_s.resolveFbo);
    p.BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    p.BindFramebuffer(kFRAMEBUFFER, fbo);
    in->sceneColor = g_s.resolveColor;
    in->sceneDepth = g_s.resolveDepth;
    in->w = w;
    in->h = h;
    return glGetError() == GL_NO_ERROR;
}

bool WriteBackMultisample(unsigned fbo, const FrameInput& in) {
    const Procs& p = P();
    if (!p.BlitFramebuffer || !g_s.resolveFbo || in.sceneColor != g_s.resolveColor) return false;
    p.BindFramebuffer(kREAD_FRAMEBUFFER, g_s.resolveFbo);
    p.BindFramebuffer(kDRAW_FRAMEBUFFER, fbo);
    p.BlitFramebuffer(0, 0, in.w, in.h, 0, 0, in.w, in.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    p.BindFramebuffer(kFRAMEBUFFER, fbo);
    return glGetError() == GL_NO_ERROR;
}

void ReleaseShared() {
    if (g_s.gen != g_gen) {
        g_s = Shared{};
        return;
    }
    const Procs& p = P();
    DeleteTarget(g_s.ping);
    DeleteTarget(g_s.pong);
    DeleteResolve();
    if (g_s.outFbo && p.DeleteFramebuffers) p.DeleteFramebuffers(1, &g_s.outFbo);
    if (g_s.copyProg && p.DeleteProgram) p.DeleteProgram(g_s.copyProg);
    g_s = Shared{};
}
}  // namespace melange::mirage::postfx
