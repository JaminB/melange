// Offline self-test for the shader source layer (src/render/mirage/shaders_source.cpp): globs, patch files, the
// built-in FXAA patch, compiler listings, params.ini, Cg variable tables and override-root resolution, against a
// scratch directory under %TEMP%.
#include <windows.h>

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "render/mirage/glsl_toggle_logic.h"
#include "render/mirage/shaders_source.h"

using namespace melange::mirage::shadersrc;

namespace {
int g_checks = 0, g_failures = 0;

void Check(bool cond, const char* what) {
    ++g_checks;
    if (cond) return;
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
}

void Write(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD n = 0;
    WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(h);
}

void TestText() {
    Check(Glob("CopyFxaa*", "CopyFxaa") && Glob("CopyFxaa*", "CopyFxaaSepia") && !Glob("CopyFxaa*", "Copy1x1"), "glob prefix");
    Check(Glob("*FragmentMain", "HeightMapFragmentMain") && !Glob("*FragmentMain", "LandscapeVertexMain"), "glob suffix");
    Check(Glob("copyfxaa", "CopyFxaa") && Glob("Copy?x?", "Copy2x4") && Glob("*", "") && !Glob("a", ""), "glob case and ?");
    Check(Glob("*a*b*", "xxaxxbxx") && !Glob("*a*b", "xxaxxbxxc"), "glob backtracking");
    Check(BaseName("CG/Landscape.cg") == "Landscape.cg" && BaseName("/FXaa3_9.h") == "FXaa3_9.h" &&
              BaseName("a\\b\\c.h") == "c.h" && BaseName("x") == "x",
          "base name");
    Check(CrlfToLf("a\r\nb\r\n\rc") == "a\nb\n\rc", "crlf");
    std::vector<std::string> inc = ScanIncludes("#define A 1\r\n#include \"FXaa3_9.h\"\r\n  #  include <sub/x.h>\n// #include \"no.h\"\n#include\n");
    Check(inc.size() == 2 && inc[0] == "FXaa3_9.h" && inc[1] == "sub/x.h", "scan includes");
    Check(LacksExplicitLod("arbfp1") && LacksExplicitLod("FP30") && !LacksExplicitLod("fp40") && !LacksExplicitLod("gp4fp") &&
              !LacksExplicitLod("glslf"),
          "explicit-LOD profiles");
    Check(IContains("Landscape.cg", "SCAPE") && !IContains("Water.cg", "land") && IEquals("fxaa3_9.h", "FXaa3_9.H"), "case helpers");
}

void TestPatch() {
    Patch p;
    std::string err;
    Check(ParsePatch("comment\r\n@@ entry CopyFxaa*\r\n@@ find\r\nA\r\n  B\r\n@@ replace\r\nX\r\n@@ end\r\n\r\n@@ find\r\nC\r\n@@ replace\r\n@@ end\r\n", &p, &err),
          "parse valid patch");
    Check(p.entryGlob == "CopyFxaa*" && p.blocks.size() == 2 && p.blocks[0].find == "A\n  B" && p.blocks[0].replace == "X" &&
              p.blocks[0].line == 3 && p.blocks[1].replace.empty(),
          "patch blocks");
    Check(!ParsePatch("@@ find\nA\n@@ end\n", &p, &err) && err.find("line 3") == 0, "end without replace");
    Check(!ParsePatch("@@ replace\nA\n@@ end\n", &p, &err), "replace without find");
    Check(!ParsePatch("@@ find\nA\n@@ replace\nB\n", &p, &err) && err.find("not closed") != std::string::npos, "unclosed block");
    Check(!ParsePatch("@@ find\nA\n@@ replace\nB\n@@ end\n@@ entry X\n", &p, &err), "entry after a block");
    Check(!ParsePatch("@@ bogus\n", &p, &err) && !ParsePatch("nothing here\n", &p, &err), "unknown directive, no blocks");
    Check(!ParsePatch("@@ find\n@@ replace\nB\n@@ end\n", &p, &err), "empty find");

    ParsePatch("@@ find\nfoo\n@@ replace\nbar\n@@ end\n@@ find\nbar baz\n@@ replace\nqux\n@@ end\n", &p, &err);
    std::string t = "x foo baz y", orig = t;
    int bad = -1, n = -1;
    Check(ApplyPatch(p, &t, &bad, &n) && t == "x qux y", "blocks apply in order");
    t = "foo foo baz";
    Check(!ApplyPatch(p, &t, &bad, &n) && t == "foo foo baz" && bad == 0 && n == 2, "two matches: skipped, unchanged");
    t = "foo";
    Check(!ApplyPatch(p, &t, &bad, &n) && t == "foo" && bad == 1 && n == 0, "second block missing: whole patch skipped");
}

void TestBuiltin() {
    const std::vector<Builtin>& b = Builtins();
    Check(b.size() == 1 && std::string(b[0].file) == "Fxaa3_9.h", "one built-in patch, on Fxaa3_9.h");
    Patch p;
    std::string err;
    Check(ParsePatch(b[0].text, &p, &err) && p.entryGlob == "CopyFxaa*" && p.blocks.size() == 2, "fxaa patch parses");
    std::string src = CrlfToLf(
        "#if FXAA_GLSL_120\r\n    #define FxaaTex sampler2D\r\n    #define FxaaTexTop(t, p) texture2DLod(t, p, 0.0)\r\n"
        "    #if (FXAA_FAST_PIXEL_OFFSET == 1)\r\n        #define FxaaTexOff(t, p, o, r) texture2DLodOffset(t, p, 0.0, o)\r\n"
        "    #else\r\n        #define FxaaTexOff(t, p, o, r) texture2DLod(t, p + (o * r), 0.0)\r\n    #endif\r\n#endif\r\n");
    Check(ApplyPatch(p, &src, nullptr, nullptr), "fxaa patch applies");
    Check(src.find("#define FxaaTexTop(t, p) tex2D(t, p)\n") != std::string::npos &&
              src.find("#define FxaaTexOff(t, p, o, r) tex2D(t, p + (o * r))\n") != std::string::npos &&
              src.find("texture2DLod(") == std::string::npos && src.find("texture2DLodOffset(t, p, 0.0, o)") != std::string::npos,
          "fxaa patch result");
    Check(b[0].profileOk("arbfp1") && !b[0].profileOk("gp4fp"), "fxaa profile gate");
}

void TestListing() {
    std::vector<Diag> d = ParseListing(
        "(3) : error C0000: syntax error, unexpected ';' at token \";\"\n/FXaa3_9.h(1346) : error C3004: function \"tex2D\" not "
        "supported in this profile\r\n(12) : warning C7011: implicit cast\n(0) : fatal error C9999: out of memory\n\n",
        "Landscape.cg");
    Check(d.size() == 4, "listing lines");
    if (d.size() == 4) {
        Check(d[0].file == "Landscape.cg" && d[0].line == 3 && d[0].error && d[0].text.rfind("C0000:", 0) == 0, "main-file error");
        Check(d[1].file == "FXaa3_9.h" && d[1].line == 1346 && d[1].error, "include error");
        Check(!d[2].error && d[2].line == 12, "warning");
        Check(d[3].error && d[3].text.rfind("C9999", 0) == 0, "fatal error");
    }
}

void TestParams() {
    std::vector<ParamSpec> ps;
    std::string err;
    Check(ParseParams("; c\n[Landscape.cg:*FragmentMain]\nspecularPower=float,20,1,64\ntint = color, 0.2 0.3 0.4 ,0,1,Tint colour\n"
                      "[Water.cg:WaterFragmentMain]\nw=vec2,1 2\n",
                      &ps, &err),
          "params parse");
    Check(ps.size() == 3, "params count");
    if (ps.size() == 3) {
        Check(ps[0].file == "Landscape.cg" && ps[0].entryGlob == "*FragmentMain" && ps[0].n == 1 && ps[0].def[0] == 20 &&
                  ps[0].min == 1 && ps[0].max == 64,
              "float param");
        Check(ps[1].type == "color" && ps[1].n == 3 && ps[1].def[2] == 0.4f && ps[1].label == "Tint colour", "color param");
        Check(ps[2].n == 2 && ps[2].def[1] == 2 && ps[2].min == 0 && ps[2].max == 1, "vec2 defaults to 0..1");
    }
    ps.clear();
    Check(!ParseParams("x=float,1\n", &ps, &err), "param outside a section");
    Check(!ParseParams("[A.cg:B]\nx=matrix,1\n", &ps, &err), "unknown type");
    Check(!ParseParams("[A.cg:B]\nx=vec3,1 2\n", &ps, &err), "wrong default count");
    Check(!ParseParams("[A.cg]\nx=float,1\n", &ps, &err), "section without entry");
}

void TestVars() {
    std::vector<CgVar> v = ParseVars(
        "//var float4x4 model :  : _model1[0], 4 : 11 : 1\n//var sampler2D texture0 :  : _texture01 : 6 : 1\n"
        "#var float3 globalLightDir :  : c[21] : 14 : 1\n#var float4 inColour : $vin.COLOR : COLOR0 : 3 : 0\n");
    Check(v.size() == 4, "var count");
    if (v.size() == 4) {
        Check(v[0].type == "float4x4" && v[0].name == "model" && v[0].resource == "_model1[0]" && v[0].count == 4 && v[0].used, "matrix var");
        Check(v[1].resource == "_texture01" && v[1].count == 1, "sampler var");
        Check(v[2].resource == "c[21]", "arb var");
        Check(v[3].semantic == "$vin.COLOR" && !v[3].used, "varying var");
    }
}

void TestSources() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring base = std::wstring(tmp) + L"melange_shaders_selftest_" + std::to_wstring(GetCurrentProcessId());
    std::wstring cg = base + L"\\CG", modA = base + L"\\a", modB = base + L"\\b";
    CreateDirectoryW(base.c_str(), nullptr);
    for (const std::wstring& d : {cg, modA, modB}) CreateDirectoryW(d.c_str(), nullptr);
    Write(cg + L"\\Main.cg", "#include \"Inc.h\"\r\nfloat4 A() { return 1; }\r\nfloat4 B() { return 2; }\r\n");
    Write(cg + L"\\Inc.h", "#include \"Deep.h\"\r\n#define V 1\r\n");
    Write(cg + L"\\Deep.h", "#define D 1\r\n");
    Write(cg + L"\\Fxaa3_9.h", "    #define FxaaTexTop(t, p) texture2DLod(t, p, 0.0)\r\n        #define FxaaTexOff(t, p, o, r) texture2DLod(t, p + (o * r), 0.0)\r\n");

    Sources s;
    s.vanillaDir = cg;
    std::vector<Issue> issues;
    Loaded l = s.Load("Main.cg", "A", "arbfp1", &issues);
    Check(l.found && !l.fromRoot && !l.patched && l.owner.empty() && l.text.find("\r\n") != std::string::npos, "vanilla untouched");
    Check(s.FindInRoots("Main.cg", nullptr).empty() && !s.AnyBuiltin("A", "arbfp1"), "no roots, no built-in");
    Check(s.AnyBuiltin("CopyFxaaSepia", "arbfp1") && !s.AnyBuiltin("CopyFxaa", "fp40") && !s.AnyBuiltin("Copy1x1", "arbfp1"),
          "built-in targets");
    l = s.Load("/FXAA3_9.H", "CopyFxaa", "arbfp1", &issues);
    Check(l.found && l.builtin && l.owner == "builtin" && l.text.find("tex2D(t, p)") != std::string::npos, "built-in applied");
    l = s.Load("Fxaa3_9.h", "Copy1x1", "arbfp1", &issues);
    Check(l.found && !l.builtin && l.text.find("texture2DLod") != std::string::npos, "built-in entry-scoped");
    s.builtins = false;
    l = s.Load("Fxaa3_9.h", "CopyFxaa", "arbfp1", &issues);
    Check(!l.builtin && !s.AnyBuiltin("CopyFxaa", "arbfp1"), "FixFxaa=0");
    s.builtins = true;

    s.roots = {{modA, "a-mod"}, {modB, "b-mod"}};
    Write(modA + L"\\Inc.h", "#define V 2\r\n");
    Write(modB + L"\\Inc.h", "#include \"Other.h\"\r\n#define V 3\r\n");
    std::string owner;
    Check(s.FindInRoots("Inc.h", &owner) == modB + L"\\Inc.h" && owner == "b-mod", "later root wins");
    l = s.Load("Inc.h", "A", "arbfp1", &issues);
    Check(l.fromRoot && l.owner == "b-mod" && l.text.find("V 3") != std::string::npos, "root file");

    Write(modA + L"\\Main.cg.patch", "@@ entry B\n@@ find\nreturn 2;\n@@ replace\nreturn 20;\n@@ end\n");
    issues.clear();
    l = s.Load("Main.cg", "A", "arbfp1", &issues);
    Check(!l.patched && l.owner.empty() && issues.empty(), "entry-scoped patch skips other entries");
    l = s.Load("Main.cg", "B", "arbfp1", &issues);
    Check(l.patched && l.owner == "a-mod" && l.text.find("return 20;") != std::string::npos && l.text.find('\r') == std::string::npos,
          "patch applied, CRLF normalised");
    Write(modB + L"\\Main.cg.patch", "@@ find\nreturn 99;\n@@ replace\nx\n@@ end\n");
    issues.clear();
    l = s.Load("Main.cg", "B", "arbfp1", &issues);
    Check(l.patched && l.owner == "a-mod" && issues.size() == 1 && issues[0].error && issues[0].owner == "b-mod" &&
              issues[0].file == "Main.cg.patch" && issues[0].line == 1,
          "non-matching patch reported, others kept");
    Write(modB + L"\\Main.cg.patch", "@@ find\nbroken\n");
    issues.clear();
    s.Load("Main.cg", "B", "arbfp1", &issues);
    Check(issues.size() == 1 && issues[0].text.find("not closed") != std::string::npos, "malformed patch reported");

    Write(modA + L"\\Fxaa3_9.h", "#define FxaaTexTop(t, p) tex2Dlod(t, float4(p, 0, 0))\n");
    issues.clear();
    l = s.Load("Fxaa3_9.h", "CopyFxaa", "arbfp1", &issues);
    Check(l.fromRoot && !l.builtin && l.owner == "a-mod" && issues.size() == 1 && !issues[0].error, "mod-replaced file: built-in quiet");

    std::vector<std::string> deps = s.Dependencies("Main.cg");
    Check(deps.size() == 2 && deps[0] == "inc.h" && deps[1] == "other.h", "dependencies through the winning root");
    s.roots.clear();
    deps = s.Dependencies("Main.cg");
    Check(deps.size() == 2 && deps[1] == "deep.h", "vanilla dependencies");

    std::wstring cmd = L"cmd /c rmdir /s /q \"" + base + L"\"";
    _wsystem(cmd.c_str());
}

void TestSamples() {
#ifdef MELANGE_SOURCE_DIR
    std::wstring dir = Widen(MELANGE_SOURCE_DIR) + L"\\dist\\Mods\\mirage-landscape\\shaders\\";
    std::string text, err;
    Patch p;
    Check(ReadFile(dir + L"Landscape.cg.patch", &text) && ParsePatch(text, &p, &err) && p.blocks.size() == 3 && p.entryGlob.empty(),
          "sample Landscape.cg.patch parses");
    std::vector<ParamSpec> ps;
    Check(ReadFile(dir + L"params.ini", &text) && ParseParams(text, &ps, &err) && ps.size() == 5, "sample params.ini parses");
    for (const ParamSpec& s : ps)
        Check(text.find(s.name) != std::string::npos && p.blocks[0].replace.find(s.name) != std::string::npos, "sample param declared by the patch");
#endif
}
void TestGlslToggle() {
    using namespace melange::mirage::shaders::glsl::logic;
    Check(Decode("").empty(), "glsl toggle: an empty string decodes to no entries");
    auto list = Decode("Landscape.cg:LandscapeFragmentMain,Water.cg:WaterFragmentMain");
    Check(list.size() == 2 && list[0].first == "Landscape.cg" && list[0].second == "LandscapeFragmentMain",
          "glsl toggle: decode splits on ':' and ','");
    Check(Encode(list) == "Landscape.cg:LandscapeFragmentMain,Water.cg:WaterFragmentMain", "glsl toggle: encode round-trips decode");
    Check(Contains(list, "landscape.cg", "LandscapeFragmentMain"), "glsl toggle: file matching is case-insensitive");
    Check(!Contains(list, "Landscape.cg", "landscapefragmentmain"), "glsl toggle: entry matching is case-sensitive (Cg entry names are)");
    Check(Decode("bad,noColon,:emptyfile,emptyentry:").empty(), "glsl toggle: malformed entries are skipped, not crashed on");

    std::vector<Pair> disabled;
    disabled = SetEnabled(disabled, "Landscape.cg", "LandscapeFragmentMain", false);
    Check(Contains(disabled, "Landscape.cg", "LandscapeFragmentMain"), "glsl toggle: disabling adds the pair");
    disabled = SetEnabled(disabled, "Landscape.cg", "LandscapeFragmentMain", false);
    Check(disabled.size() == 1, "glsl toggle: disabling twice is idempotent");
    disabled = SetEnabled(disabled, "Landscape.cg", "LandscapeFragmentMain", true);
    Check(disabled.empty(), "glsl toggle: re-enabling removes the pair");
    disabled = SetEnabled(disabled, "Landscape.cg", "LandscapeFragmentMain", true);
    Check(disabled.empty(), "glsl toggle: re-enabling an already-enabled pair is a no-op");
}
}  // namespace

int main() {
    TestGlslToggle();
    TestText();
    TestPatch();
    TestBuiltin();
    TestListing();
    TestParams();
    TestVars();
    TestSources();
    TestSamples();
    std::cout << "shaders_selftest: " << (g_checks - g_failures) << "/" << g_checks << " checks passed\n";
    return g_failures ? 1 : 0;
}
