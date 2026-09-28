// Offline self-test of the post-FX stack: effect.ini parsing, GLSL assembly and the persisted formats, then (in a
// hidden window with the local driver) compiling the sample effects and running them on a synthetic frame.
#include <windows.h>
#include <GL/gl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "render/mirage/postfx_internal.h"

namespace pfx = melange::mirage::postfx;
using melange::render::Stage;

namespace {
int g_checks = 0, g_failed = 0;

void Check(bool ok, const char* what, const std::string& detail = {}) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    printf("FAIL: %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
}

bool Has(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }

// ---------------------------------------------------------------- effect.ini
const char* kSpecExample = R"(; comment
[effect]
title=Bloom
stage=PostWorld          ; PostWorld | Final
order=300                ; lower first; ties by id
enabled=0                ; the default until the user toggles it (then Melange.ini wins)

[param.threshold]        ; uniform float p_threshold
type=float               ; float | vec2 | vec3 | color | int | bool
default=0.8
min=0
max=2
label=Threshold

[texture.lut]            ; uniform sampler2D t_lut, loaded from the effect folder (PNG via stb_image)
file=lut.png
filter=linear            ; linear | nearest
wrap=clamp               ; clamp | repeat

[pass.extract]
shader=extract.frag
scale=0.5                ; relative to scene size
format=rgba16f           ; rgba8 | rgba16f | r8 | rg8
inputs=scene             ; scene, depth, prev, pass.<name>, texture.<name>

[pass.blurh]
shader=blur.frag
defines=HORIZONTAL=1
scale=0.5
inputs=prev

[pass.combine]           ; the last pass writes the effect's output at scene size
shader=combine.frag
inputs=scene,pass.blurh
)";

std::string ParseError(const char* text) {
    pfx::EffectDesc d;
    std::string err;
    return pfx::ParseEffect(text, &d, &err) ? std::string("(parsed)") : err;
}

void TestParse() {
    pfx::EffectDesc d;
    std::string err;
    bool ok = pfx::ParseEffect(std::string("\xEF\xBB\xBF") + kSpecExample, &d, &err);
    Check(ok, "spec example parses", err);
    if (!ok) return;
    Check(d.title == "Bloom" && d.stage == Stage::PostWorld && d.order == 300 && !d.enabled, "[effect] fields");
    Check(d.params.size() == 1 && d.params[0].name == "threshold" && d.params[0].n == 1 && d.params[0].def[0] == 0.8f &&
              d.params[0].hasRange && d.params[0].min == 0.f && d.params[0].max == 2.f && d.params[0].label == "Threshold",
          "[param.threshold]");
    Check(d.textures.size() == 1 && d.textures[0].file == "lut.png" && d.textures[0].linear && !d.textures[0].repeat,
          "[texture.lut]");
    Check(d.passes.size() == 3, "three passes");
    if (d.passes.size() == 3) {
        const pfx::PassDesc &a = d.passes[0], &b = d.passes[1], &c = d.passes[2];
        Check(a.name == "extract" && a.shader == "extract.frag" && a.scale == 0.5f && a.format == pfx::Format::Rgba16f &&
                  a.inputs.size() == 1 && a.inputs[0].kind == pfx::InputKind::Scene,
              "[pass.extract]");
        Check(b.defines.size() == 1 && b.defines[0] == "HORIZONTAL 1" && b.inputs.size() == 1 &&
                  b.inputs[0].kind == pfx::InputKind::Prev && b.format == pfx::Format::Rgba8,
              "[pass.blurh]");
        Check(c.inputs.size() == 2 && c.inputs[1].kind == pfx::InputKind::Pass && c.inputs[1].name == "blurh" &&
                  pfx::SamplerName(c.inputs[1]) == "mg_pass_blurh",
              "[pass.combine] inputs");
    }
    Check(pfx::SamplerName({pfx::InputKind::Texture, "lut"}) == "t_lut" && pfx::SamplerName({pfx::InputKind::Depth, ""}) == "mg_depth",
          "sampler names");

    Check(ParseError("[effect]\nstage=Final\n[pass.a]\nshader=a.frag\n") == "(parsed)", "minimal effect");
    {
        pfx::EffectDesc m;
        pfx::ParseEffect("[effect]\n[param.c]\ntype=color\n[param.v]\ntype=vec3\ndefault=1, 2,3\n[pass.a]\nshader=a\n", &m, &err);
        Check(m.params.size() == 2 && m.params[0].n == 3 && m.params[0].def[0] == 1.f && m.params[1].def[2] == 3.f &&
                  m.passes[0].inputs.size() == 1 && m.passes[0].inputs[0].kind == pfx::InputKind::Prev,
              "defaults: color white, vec3 values, inputs=prev");
    }
    Check(Has(ParseError("[effect]\nfoo=1\n[pass.a]\nshader=a\n"), "effect.ini(2): unknown key 'foo'"), "unknown key with line",
          ParseError("[effect]\nfoo=1\n[pass.a]\nshader=a\n"));
    Check(Has(ParseError("[effect]\nstage=Hud\n[pass.a]\nshader=a\n"), "stage must be"), "bad stage");
    Check(Has(ParseError("[effect]\n[pass.a]\nshader=a\ninputs=pass.b\n[pass.b]\nshader=b\n"), "effect.ini(4): input 'pass.b' is not an earlier pass"),
          "forward pass reference", ParseError("[effect]\n[pass.a]\nshader=a\ninputs=pass.b\n[pass.b]\nshader=b\n"));
    Check(Has(ParseError("[pass.a]\nshader=a\n"), "missing [effect]"), "missing [effect]");
    Check(Has(ParseError("[effect]\ntitle=x\n"), "no [pass.*]"), "no passes");
    Check(Has(ParseError("[effect]\n[param.p]\ntype=vec2\ndefault=1\n[pass.a]\nshader=a\n"), "default needs 2 values"), "default count");
    Check(Has(ParseError("[effect]\n[texture.t]\nfilter=nearest\n[pass.a]\nshader=a\n"), "has no file="), "texture without file");
    Check(Has(ParseError("[effect]\n[pass.a]\nshader=a\n[pass.A]\nshader=b\n"), "duplicate [pass.A]"), "duplicate pass");
    Check(Has(ParseError("[effect]\n[pass.a]\nshader=a\ninputs=texture.x\n"), "names no [texture.*]"), "unknown texture input");
    Check(Has(ParseError("[effect]\n[pass.a]\nshader=a\nscale=0\n"), "scale must be"), "scale range");
    Check(Has(ParseError("[effect]\n[pass.my-pass]\nshader=a\n"), "bad section"), "pass names are identifiers");
    Check(Has(ParseError("[effect]\n[pass.a]\n"), "has no shader="), "pass without shader");
}

// ---------------------------------------------------------------- GLSL assembly
struct Files {
    std::vector<std::pair<std::string, std::string>> f;
};
bool FakeInclude(const std::string& name, std::string* text, void* user) {
    for (auto& [n, t] : static_cast<Files*>(user)->f)
        if (n == name) {
            *text = t;
            return true;
        }
    return false;
}

void TestAssemble() {
    pfx::Source s;
    std::string err;
    bool ok = pfx::BuildFragment("void main() {}\n", "a.frag", {"HORIZONTAL 1", "FAST"}, nullptr, nullptr, &s, &err);
    Check(ok && s.version == 120 && s.text == "#version 120\n#define HORIZONTAL 1\n#define FAST\n#line 0 0\nvoid main() {}\n",
          "no #version: 120 and defines", s.text);
    ok = pfx::BuildFragment("// hi\n/* x */ #version 130\nin vec2 mg_uv;\n", "b.frag", {}, nullptr, nullptr, &s, &err);
    Check(ok && s.version == 130 && s.text.rfind("#version 130\n#line 0 0\n", 0) == 0 && Has(s.text, "in vec2 mg_uv;") &&
              !Has(s.text.substr(10), "#version"),
          "#version after comments is moved to the top", s.text);
    ok = pfx::BuildFragment("#version 330 core\nvoid main(){}\n", "c.frag", {}, nullptr, nullptr, &s, &err);
    Check(ok && s.version == 330 && s.profile == "core" && s.text.rfind("#version 330 core\n#line 1 0\n", 0) == 0,
          "330: #line counts from the next line", s.text);
    Check(!pfx::BuildFragment("#version 300 es\n", "d.frag", {}, nullptr, nullptr, &s, &err) && Has(err, "desktop"), "es rejected");

    Files files{{{"lib.h", "float f() { return 1.0; }\n"}, {"deep.h", "#include \"lib.h\"\n"}}};
    ok = pfx::BuildFragment("#version 130\nuniform float x;\n#include \"lib.h\"\nvoid main() {}\n", "e.frag", {}, &FakeInclude,
                            &files, &s, &err);
    Check(ok && Has(s.text, "uniform float x;\n#line 0 1\nfloat f() { return 1.0; }\n#line 3 0\nvoid main() {}\n") &&
              s.files.size() == 2 && s.files[1] == "lib.h",
          "#include inlined with #line bookkeeping", s.text + err);
    ok = pfx::BuildFragment("#include \"deep.h\"\n", "f.frag", {}, &FakeInclude, &files, &s, &err);
    Check(ok && s.files.size() == 3 && Has(s.text, "float f()"), "nested include", err);
    Check(!pfx::BuildFragment("\n\n#include \"nope.h\"\n", "g.frag", {}, &FakeInclude, &files, &s, &err) &&
              Has(err, "g.frag(3): cannot open \"nope.h\""),
          "missing include names file and line", err);
    Check(!pfx::BuildFragment("#include \"../x.h\"\n", "h.frag", {}, &FakeInclude, &files, &s, &err) && Has(err, "effect folder"),
          "include cannot leave the folder");
    Files loop{{{"self.h", "#include \"self.h\"\n"}}};
    Check(!pfx::BuildFragment("#include \"self.h\"\n", "i.frag", {}, &FakeInclude, &loop, &s, &err) && Has(err, "too deep"),
          "include recursion is bounded");

    Check(Has(pfx::BuildVertex(120, ""), "attribute vec2 mg_pos;") && Has(pfx::BuildVertex(120, ""), "varying vec2 mg_uv;"), "120 VS");
    Check(pfx::BuildVertex(150, "compatibility").rfind("#version 150 compatibility\nin vec2 mg_pos;\nout vec2 mg_uv;", 0) == 0,
          "150 VS");
    Check(pfx::ParseGlslVersion("4.60") == 460 && pfx::ParseGlslVersion("1.20 NVIDIA via Cg") == 120 &&
              pfx::ParseGlslVersion("4.6") == 460 && pfx::ParseGlslVersion("x") == 0 && pfx::ParseGlslVersion(nullptr) == 0,
          "GLSL version parsing");
}

// ---------------------------------------------------------------- persistence
void TestPersist() {
    auto v = pfx::ParseStack("a/b:100:1, c/d:-5:0,bad,x:y:z,own:er/x:3:1,a/b:7:0");
    Check(v.size() == 3 && v[0].id == "a/b" && v[0].order == 7 && !v[0].enabled && v[1].id == "c/d" && v[1].order == -5 &&
              v[2].id == "own:er/x" && v[2].enabled,
          "Stack parsing", pfx::FormatStack(v));
    Check(pfx::FormatStack(v) == "a/b:7:0,c/d:-5:0,own:er/x:3:1", "Stack formatting");
    Check(pfx::ParseStack("").empty(), "empty Stack");
    float f[4] = {};
    Check(pfx::ParseFloats("0.8, 1,-2.5", f, 4) == 3 && f[0] == 0.8f && f[2] == -2.5f, "ParseFloats");
    Check(pfx::FormatFloats(f, 3) == "0.8,1,-2.5", "FormatFloats round-trips", pfx::FormatFloats(f, 3));
    Check(pfx::ParseFloats("1,x,3", f, 4) == 1 && pfx::ParseFloats("1,2,3,4,5", f, 4) == 4, "ParseFloats stops");

    float proj[16] = {}, inv[16];
    const float n = 1.f, fa = 100.f, t = std::tan(0.5f);
    proj[0] = 1.f / (t * 1.6f);
    proj[5] = 1.f / t;
    proj[10] = -(fa + n) / (fa - n);
    proj[11] = -1.f;
    proj[14] = -2.f * fa * n / (fa - n);
    bool ok = pfx::Invert4(proj, inv);
    float worst = 0;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += proj[k * 4 + r] * inv[c * 4 + k];
            worst = std::max(worst, std::fabs(s - (r == c ? 1.f : 0.f)));
        }
    Check(ok && worst < 1e-5f, "Invert4 of a perspective matrix");
    float zero[16] = {};
    Check(!pfx::Invert4(zero, inv), "Invert4 rejects a singular matrix");
}

// ---------------------------------------------------------------- GL
std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
const std::string kSamples = std::string(MELANGE_SOURCE_DIR) + "/dist/Mods/mirage-samples/postfx";

bool ReadText(const std::string& path, std::string* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char b[4096];
    out->clear();
    for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0;) out->append(b, n);
    fclose(f);
    return true;
}

void TestSampleIni() {
    for (const char* name : {"smaa", "tonemap", "bloom", "ssao", "sharpen"}) {
        std::string text, err;
        pfx::EffectDesc d;
        bool ok = ReadText(kSamples + "/" + name + "/effect.ini", &text) && pfx::ParseEffect(text, &d, &err);
        Check(ok && !d.enabled, (std::string("sample ") + name + " parses and ships disabled").c_str(), err);
    }
}

HGLRC MakeContext() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MelangePostfxSelftest";
    wc.style = CS_OWNDC;
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"postfx", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!wnd) return nullptr;
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd{sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) return nullptr;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) return nullptr;
    return rc;
}

constexpr int kW = 320, kH = 200;
constexpr GLenum kDEPTH24_STENCIL8 = 0x88F0, kDEPTH_STENCIL = 0x84F9, kUNSIGNED_INT_24_8 = 0x84FA,
                 kTEXTURE_COMPARE_MODE = 0x884C, kCOMPARE_REF_TO_TEXTURE = 0x884E, kCLAMP_TO_EDGE = 0x812F;

struct Frame {
    unsigned color = 0, depth = 0;
    std::vector<uint8_t> original;
    float proj[16] = {}, invProj[16] = {};
};

// Dark ground, a white disc, a hard diagonal edge between two greys, and a wall meeting the floor at x = 240.
void MakeFrame(Frame& f) {
    f.original.assign(kW * kH * 4, 0);
    std::vector<uint32_t> depth(kW * kH);
    const float n = 1.f, fa = 100.f, t = std::tan(0.5f), aspect = static_cast<float>(kW) / kH;
    std::memset(f.proj, 0, sizeof f.proj);
    f.proj[0] = 1.f / (t * aspect);
    f.proj[5] = 1.f / t;
    f.proj[10] = -(fa + n) / (fa - n);
    f.proj[11] = -1.f;
    f.proj[14] = -2.f * fa * n / (fa - n);
    pfx::Invert4(f.proj, f.invProj);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            uint8_t* p = &f.original[(y * kW + x) * 4];
            int v = 50;
            if ((x - 80) * (x - 80) + (y - 100) * (y - 100) <= 20 * 20) v = 255;
            else if (x > 150 && x < 230 && y > 2 * (x - 150)) v = 180;
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v);
            p[2] = static_cast<uint8_t>(v * 9 / 10);
            p[3] = 255;
            // view z: a floor receding with y, and a wall rising towards the camera from x = 240 (a concave crease)
            float z = y > 150 ? 1e9f : -(10.f + (150 - y) * 0.2f);
            if (x >= 240 && y <= 150) z = std::min(z + (x - 240) * 0.5f, -2.f);
            float d = 1.f;
            if (z < 0) {
                float zc = f.proj[10] * z + f.proj[14], wc = -z;
                d = (zc / wc) * 0.5f + 0.5f;
            }
            depth[y * kW + x] = static_cast<uint32_t>(std::lround(std::clamp(d, 0.f, 1.f) * 16777215.0)) << 8;
        }
    if (!f.color) glGenTextures(1, &f.color);
    glBindTexture(GL_TEXTURE_2D, f.color);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA, GL_UNSIGNED_BYTE, f.original.data());
    if (!f.depth) glGenTextures(1, &f.depth);
    glBindTexture(GL_TEXTURE_2D, f.depth);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kCLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kCLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, kTEXTURE_COMPARE_MODE, kCOMPARE_REF_TO_TEXTURE);
    glTexImage2D(GL_TEXTURE_2D, 0, kDEPTH24_STENCIL8, kW, kH, 0, kDEPTH_STENCIL, kUNSIGNED_INT_24_8, depth.data());
}

void ResetColor(const Frame& f) {
    glBindTexture(GL_TEXTURE_2D, f.color);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, f.original.data());
}

std::vector<uint8_t> ReadColor(const Frame& f) {
    std::vector<uint8_t> px(kW * kH * 4);
    glBindTexture(GL_TEXTURE_2D, f.color);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    return px;
}

void Defaults(pfx::Effect& e) {
    e.values.resize(e.desc.params.size());
    for (size_t i = 0; i < e.desc.params.size(); ++i)
        for (int k = 0; k < 4; ++k) e.values[i][k] = e.desc.params[i].def[k];
}

bool LoadEffect(pfx::Effect& e, const std::string& dir, const char* id, bool expectOk = true) {
    std::string text, err;
    e.id = id;
    e.dir = W(dir);
    e.enabled = true;
    if (!ReadText(dir + "/effect.ini", &text) || !pfx::ParseEffect(text, &e.desc, &err)) {
        Check(false, (std::string(id) + " effect.ini").c_str(), err);
        return false;
    }
    e.stage = e.desc.stage;
    Defaults(e);
    bool ok = pfx::Compile(e);
    if (expectOk) Check(ok && !e.failed, (std::string(id) + " compiles and links").c_str(), e.error);
    return ok;
}

void SetValue(pfx::Effect& e, const char* name, float v) {
    for (size_t i = 0; i < e.desc.params.size(); ++i)
        if (e.desc.params[i].name == name) e.values[i][0] = v;
}

pfx::RunResult RunChain(const Frame& f, std::vector<pfx::Effect*> chain, bool split = false) {
    ResetColor(f);
    pfx::FrameInput in;
    in.sceneColor = f.color;
    in.sceneDepth = f.depth;
    in.w = kW;
    in.h = kH;
    std::copy(f.proj, f.proj + 16, in.proj);
    std::copy(f.invProj, f.invProj + 16, in.invProj);
    for (int k = 0; k < 16; k += 5) in.view[k] = 1.f;
    in.nearFar[0] = 1.f;
    in.nearFar[1] = 100.f;
    in.frame = 7;
    in.splitCompare = split;
    return pfx::Run(chain, in);
}

double Luma(const std::vector<uint8_t>& px, int x, int y) {
    const uint8_t* p = &px[(y * kW + x) * 4];
    return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2];
}

void WriteFile(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

std::string TempEffect(const char* name, const char* ini, std::initializer_list<std::pair<const char*, const char*>> files) {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "melange_postfx_selftest";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += std::string("\\") + name;
    CreateDirectoryA(dir.c_str(), nullptr);
    WriteFile(dir + "\\effect.ini", ini);
    for (auto& [n, t] : files) WriteFile(dir + "\\" + n, t);
    return dir;
}

void RedPass(const melange::postfx::PassContext& c, void* user) {
    auto* seen = static_cast<int*>(user);
    *seen = (c.srcColor != 0) + (c.w == kW) + (c.h == kH) + (c.dstFbo != 0) + (c.proj != nullptr) + (c.frame == 7);
    glClearColor(1.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void CrashPass(const melange::postfx::PassContext&, void*) { *static_cast<volatile int*>(nullptr) = 1; }

void TestGl() {
    HGLRC rc = MakeContext();
    if (!rc) {
        printf("GL tests skipped: no OpenGL context\n");
        return;
    }
    printf("GL: %s | %s | GLSL %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
           reinterpret_cast<const char*>(glGetString(GL_VERSION)),
           reinterpret_cast<const char*>(glGetString(0x8B8C)));
    Check(pfx::OwnContext(), "adopts the first context");
    std::string why;
    if (!pfx::GlReady(&why)) {
        Check(false, "GlReady", why);
        return;
    }
    Frame f;
    MakeFrame(f);

    const char* kCopyIni = "[effect]\ntitle=Copy\n[pass.a]\nshader=copy.frag\ninputs=scene\n[pass.b]\nshader=prev.frag\n";
    std::string copyDir = TempEffect("copy", kCopyIni,
                                     {{"copy.frag", "uniform sampler2D mg_scene; varying vec2 mg_uv;\n"
                                                    "void main() { gl_FragColor = texture2D(mg_scene, mg_uv); }\n"},
                                      {"prev.frag", "#version 130\nuniform sampler2D mg_prev; in vec2 mg_uv; out vec4 o;\n"
                                                    "void main() { o = texture(mg_prev, mg_uv); }\n"}});
    pfx::Effect copy, copy2;
    LoadEffect(copy, copyDir, "test/copy");
    LoadEffect(copy2, copyDir, "test/copy2");
    pfx::RunResult r = RunChain(f, {&copy});
    Check(r.effects == 1 && r.passes == 2 && r.glErrors == 0, "identity effect runs");
    Check(ReadColor(f) == f.original, "identity effect is byte-exact");
    r = RunChain(f, {&copy, &copy2});
    Check(r.effects == 2 && r.glErrors == 0 && ReadColor(f) == f.original, "two chained identity effects are byte-exact");

    GLint cmp = 0;
    glBindTexture(GL_TEXTURE_2D, f.depth);
    glGetTexParameteriv(GL_TEXTURE_2D, kTEXTURE_COMPARE_MODE, &cmp);
    Check(cmp == static_cast<GLint>(kCOMPARE_REF_TO_TEXTURE), "depth compare mode restored after the run");

    const char* kInvIni = "[effect]\n[pass.a]\nshader=inv.frag\ninputs=scene\n";
    pfx::Effect inv;
    LoadEffect(inv, TempEffect("inv", kInvIni, {{"inv.frag", "uniform sampler2D mg_scene; varying vec2 mg_uv;\n"
                                                              "void main() { vec4 c = texture2D(mg_scene, mg_uv); gl_FragColor = vec4(1.0 - c.rgb, c.a); }\n"}}),
               "test/inv");
    RunChain(f, {&inv}, true);
    auto px = ReadColor(f);
    bool leftSame = true, rightInv = true;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            size_t i = (y * kW + x) * 4;
            if (x < kW / 2) leftSame &= px[i] == f.original[i];
            else rightInv &= px[i] == 255 - f.original[i];
        }
    Check(leftSame && rightInv, "split compare keeps the left half");

    int seen = 0;
    pfx::Effect red;
    red.id = "test/red";
    red.code = true;
    red.fn = &RedPass;
    red.user = &seen;
    red.enabled = true;
    pfx::Compile(red);
    r = RunChain(f, {&copy, &red, &inv});
    px = ReadColor(f);
    Check(seen == 6, "code pass gets a filled PassContext", std::to_string(seen));
    Check(px[0] == 0 && px[1] == 255 && px[2] == 255, "code pass output feeds the next effect");
    pfx::Effect crash;
    crash.id = "test/crash";
    crash.code = true;
    crash.fn = &CrashPass;
    crash.enabled = true;
    pfx::Compile(crash);
    r = RunChain(f, {&crash, &copy});
    Check(crash.failed && Has(crash.error, "0xc0000005") && r.effects == 2, "a faulting code pass is caught", crash.error);

    pfx::Effect bad;
    LoadEffect(bad, TempEffect("bad", kInvIni, {{"inv.frag", "void main() {\n  gl_FragColor = vec4(1.0)\n}\n"}}), "test/bad", false);
    Check(bad.failed && Has(bad.error, "inv.frag") && (Has(bad.error, "(3)") || Has(bad.error, ":3") || Has(bad.error, "(2)") ||
                                                       Has(bad.error, ":2")),
          "a syntax error fails the effect with file and line", bad.error);
    pfx::Effect unlisted;
    LoadEffect(unlisted,
               TempEffect("unlisted", kInvIni, {{"inv.frag", "uniform sampler2D mg_depth; varying vec2 mg_uv;\n"
                                                             "void main() { gl_FragColor = texture2D(mg_depth, mg_uv); }\n"}}),
               "test/unlisted", false);
    Check(unlisted.failed && Has(unlisted.error, "'mg_depth' is not listed in inputs="), "undeclared sampler is an error",
          unlisted.error);
    r = RunChain(f, {&bad, &copy});
    Check(r.effects == 1 && ReadColor(f) == f.original, "a failed effect is skipped and the rest runs");

    // the sample effects
    pfx::Effect smaa, tonemap, bloom, ssao, sharpen;
    bool okSmaa = LoadEffect(smaa, kSamples + "/smaa", "mirage-samples/smaa");
    bool okTone = LoadEffect(tonemap, kSamples + "/tonemap", "mirage-samples/tonemap");
    bool okBloom = LoadEffect(bloom, kSamples + "/bloom", "mirage-samples/bloom");
    bool okSsao = LoadEffect(ssao, kSamples + "/ssao", "mirage-samples/ssao");
    bool okSharp = LoadEffect(sharpen, kSamples + "/sharpen", "mirage-samples/sharpen");

    if (okBloom) {
        r = RunChain(f, {&bloom});
        px = ReadColor(f);
        double before = 0, after = 0;
        int n = 0;
        for (int y = 60; y < 140; ++y)
            for (int x = 40; x < 120; ++x) {
                int d2 = (x - 80) * (x - 80) + (y - 100) * (y - 100);
                if (d2 < 23 * 23 || d2 > 34 * 34) continue;
                before += Luma(f.original, x, y);
                after += Luma(px, x, y);
                ++n;
            }
        Check(r.glErrors == 0 && after > before * 1.05, "bloom brightens around the disc",
              std::to_string(before / n) + " -> " + std::to_string(after / n));
    }
    if (okTone) {
        RunChain(f, {&tonemap});
        px = ReadColor(f);
        Check(px != f.original, "tonemap changes the image");
        SetValue(tonemap, "strength", 0);
        SetValue(tonemap, "contrast", 1);
        SetValue(tonemap, "saturation", 1);
        RunChain(f, {&tonemap});
        px = ReadColor(f);
        int worst = 0;
        for (size_t i = 0; i < px.size(); ++i) worst = std::max(worst, std::abs(px[i] - f.original[i]));
        Check(worst <= 2, "tonemap at neutral settings with the neutral LUT is within 2/255", std::to_string(worst));
        Defaults(tonemap);
    }
    if (okBloom && okTone) {
        RunChain(f, {&bloom, &tonemap});
        auto ab = ReadColor(f);
        RunChain(f, {&tonemap, &bloom});
        Check(ab != ReadColor(f), "bloom->tonemap differs from tonemap->bloom");
    }
    if (okSharp) {
        RunChain(f, {&sharpen});
        px = ReadColor(f);
        double lapBefore = 0, lapAfter = 0;
        for (int y = 1; y < kH - 1; ++y)
            for (int x = 1; x < kW - 1; ++x) {
                auto lap = [&](const std::vector<uint8_t>& p) {
                    return std::fabs(4 * Luma(p, x, y) - Luma(p, x - 1, y) - Luma(p, x + 1, y) - Luma(p, x, y - 1) - Luma(p, x, y + 1));
                };
                lapBefore += lap(f.original);
                lapAfter += lap(px);
            }
        Check(lapAfter > lapBefore * 1.02, "sharpen raises local contrast",
              std::to_string(lapBefore) + " -> " + std::to_string(lapAfter));
    }
    if (okSmaa) {
        r = RunChain(f, {&smaa});
        px = ReadColor(f);
        auto mid = [&](const std::vector<uint8_t>& p) {
            int n = 0;
            for (int y = 0; y < kH; ++y)
                for (int x = 145; x < 235; ++x) {
                    int v = p[(y * kW + x) * 4];
                    n += v > 55 && v < 175;
                }
            return n;
        };
        Check(r.glErrors == 0 && mid(px) > mid(f.original) + 40, "smaa adds intermediate pixels on the diagonal edge",
              std::to_string(mid(f.original)) + " -> " + std::to_string(mid(px)));
    }
    if (okSsao) {
        SetValue(ssao, "debug", 1);
        SetValue(ssao, "radius", 2);
        r = RunChain(f, {&ssao});
        px = ReadColor(f);
        double crease = 0, open = 0, sky = 0;
        for (int y = 40; y < 140; ++y) {
            crease += px[(y * kW + 238) * 4];
            open += px[(y * kW + 40) * 4];
        }
        for (int x = 0; x < kW; ++x) sky += px[(180 * kW + x) * 4];
        crease /= 100;
        open /= 100;
        sky /= kW;
        Check(r.glErrors == 0 && crease < open - 10 && sky == 255, "ssao darkens the crease, keeps the sky at 1",
              "crease " + std::to_string(crease) + " open " + std::to_string(open) + " sky " + std::to_string(sky));
        SetValue(ssao, "debug", 0);
    }
    if (okSmaa && okTone && okBloom && okSsao && okSharp) {
        std::vector<pfx::Effect*> all = {&ssao, &bloom, &tonemap, &smaa, &sharpen};
        for (int i = 0; i < 8; ++i) r = RunChain(f, all);
        glFinish();
        r = RunChain(f, all);
        Check(r.effects == 5 && r.glErrors == 0, "all five run together without GL errors");

        // timings at the reference window size (informational; the budgets are checked in game)
        constexpr int bw = 1024, bh = 768;
        std::vector<uint32_t> noise(bw * bh);
        uint32_t seed = 1;
        for (uint32_t& v : noise) v = (seed = seed * 1664525u + 1013904223u) | 0xFF000000u;
        unsigned color = 0, depth = 0;
        glGenTextures(1, &color);
        glBindTexture(GL_TEXTURE_2D, color);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, bw, bh, 0, GL_RGBA, GL_UNSIGNED_BYTE, noise.data());
        for (uint32_t& v : noise) v = (v & 0xFFFF00u) << 8;
        glGenTextures(1, &depth);
        glBindTexture(GL_TEXTURE_2D, depth);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, kDEPTH24_STENCIL8, bw, bh, 0, kDEPTH_STENCIL, kUNSIGNED_INT_24_8, noise.data());
        pfx::FrameInput in;
        in.sceneColor = color;
        in.sceneDepth = depth;
        in.w = bw;
        in.h = bh;
        std::copy(f.proj, f.proj + 16, in.proj);
        std::copy(f.invProj, f.invProj + 16, in.invProj);
        for (int i = 0; i < 30; ++i) r = pfx::Run(all, in);
        glFinish();
        r = pfx::Run(all, in);
        double sum = 0;
        for (pfx::Effect* e : all) {
            printf("  %-24s gpu %.3f ms  cpu %.3f ms (%dx%d)\n", e->id.c_str(), e->gpuMs, e->cpuMs, bw, bh);
            sum += e->gpuMs;
        }
        printf("  all five: gpu %.3f ms\n", sum);
        Check(r.glErrors == 0, "all five at 1024x768 without GL errors");
        glDeleteTextures(1, &color);
        glDeleteTextures(1, &depth);
    }
    for (pfx::Effect* e : {&copy, &copy2, &inv, &red, &crash, &bad, &unlisted, &smaa, &tonemap, &bloom, &ssao, &sharpen})
        pfx::Release(*e);
    pfx::ReleaseShared();
    Check(glGetError() == GL_NO_ERROR, "no GL error after releasing everything");
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(rc);
}
}  // namespace

int main() {
    TestParse();
    TestAssemble();
    TestPersist();
    TestSampleIni();
    TestGl();
    printf("postfx_selftest: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
