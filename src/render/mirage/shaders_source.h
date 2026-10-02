#pragma once
// Shader sources for the Cg override layer: override roots, find/replace patches, parameter metadata and the
// include-resolving compile. No engine state: shared by the MirageShaders module and the offline self-tests.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace melange::mirage::shadersrc {
std::string Lower(std::string_view s);
bool IEquals(std::string_view a, std::string_view b);
bool IContains(std::string_view s, std::string_view needle);
bool Glob(std::string_view pattern, std::string_view s);  // case-insensitive, `*` and `?`
std::string BaseName(std::string_view path);              // after the last '/' or '\'
std::string CrlfToLf(std::string_view s);
// `#include "x"` / `<x>` targets in order, conditional or not.
std::vector<std::string> ScanIncludes(std::string_view text);

// Patch file (`<file>.patch`): an optional `@@ entry <glob>` line, then blocks of `@@ find` / `@@ replace` / `@@ end`.
struct PatchBlock {
    std::string find, replace;
    int line;  // of its `@@ find`
};
struct Patch {
    std::string entryGlob;  // empty = every entry
    std::vector<PatchBlock> blocks;
};
bool ParsePatch(std::string_view text, Patch* out, std::string* error);
// Every block must match exactly once. On failure *text is unchanged, *bad is the block index and *matches its count.
bool ApplyPatch(const Patch& p, std::string* text, int* bad, int* matches);

// One line of a Cg compiler listing. Virtual include names ("/Fxaa3_9.h") lose their slash; lines without a file
// name belong to `mainFile`.
struct Diag {
    std::string file;
    int line;
    bool error;
    std::string text;
};
std::vector<Diag> ParseListing(std::string_view listing, std::string_view mainFile);

// `params.ini`: `[Landscape.cg:LandscapeFragmentMain]` (the entry may be a glob), then `name=type,default,min,max`.
// Types: float, vec2, vec3, vec4, color (3 floats); vector defaults are space-separated.
struct ParamSpec {
    std::string file, entryGlob, name, type, label;
    int n;
    float def[4], min, max;
};
bool ParseParams(std::string_view ini, std::vector<ParamSpec>* out, std::string* error);

// `//var` / `#var` lines of a compiled Cg program: Cg name -> resource (GLSL uniform name or ARB register).
struct CgVar {
    std::string type, name, semantic, resource;
    int count;  // registers: 4 for a float4x4
    bool used;
};
std::vector<CgVar> ParseVars(std::string_view compiled);

// Fragment profiles without an explicit-LOD texture fetch.
bool LacksExplicitLod(std::string_view profile);

// Built-in patches compiled into the plugin (shaders_fixes.cpp).
struct Builtin {
    const char* id;
    const char* file;       // the file the patch applies to (may be an include)
    const char* entryGlob;  // the programs it applies to
    bool (*profileOk)(std::string_view profile);
    const char* text;       // patch file syntax
};
const std::vector<Builtin>& Builtins();

struct Root {
    std::wstring dir;  // holds the shader files directly (a mod's shaders\ folder)
    std::string owner;
};
struct Issue {
    std::string file;
    int line;
    std::string text, owner;
    bool error;
};
struct Loaded {
    bool found = false, fromRoot = false, patched = false, builtin = false;
    std::string text;   // LF line ends once patched
    std::string owner;  // the mod that supplied or patched it last; "builtin"; "" for vanilla
};

class Sources {
public:
    std::vector<Root> roots;  // lowest priority first
    std::wstring vanillaDir;  // the engine's CG folder
    bool builtins = true;

    // The effective text of `rel` (a file name, or a path relative to the CG folder) for one program.
    Loaded Load(std::string_view rel, std::string_view entry, std::string_view profile, std::vector<Issue>* issues) const;
    // Highest-priority root file for `rel`; empty if none.
    std::wstring FindInRoots(std::string_view rel, std::string* owner) const;
    bool AnyBuiltin(std::string_view entry, std::string_view profile) const;
    // Base names (lower case) of every file `rel` includes, recursively, as the sources resolve them.
    std::vector<std::string> Dependencies(std::string_view rel) const;
};

bool ReadFile(const std::wstring& path, std::string* out);
std::wstring Widen(std::string_view s);
std::string Narrow(std::wstring_view s);

// Cg runtime (cg.dll), resolved by name.
using CGcontext = void*;
using CGprogram = void*;
using CGparameter = void*;
using CGinclude = void(__cdecl*)(CGcontext, const char*);
struct Cg {
    CGcontext(__cdecl* CreateContext)();
    void(__cdecl* DestroyContext)(CGcontext);
    CGprogram(__cdecl* CreateProgram)(CGcontext, int, const char*, int, const char*, const char**);
    void(__cdecl* DestroyProgram)(CGprogram);
    int(__cdecl* GetError)();
    const char*(__cdecl* GetErrorString)(int);
    const char*(__cdecl* GetLastListing)(CGcontext);
    void(__cdecl* SetCompilerIncludeCallback)(CGcontext, CGinclude);
    CGinclude(__cdecl* GetCompilerIncludeCallback)(CGcontext);
    void(__cdecl* SetCompilerIncludeString)(CGcontext, const char*, const char*);
    const char*(__cdecl* ProfileString)(int);
    int(__cdecl* ProfileByName)(const char*);
    const char*(__cdecl* GetProgramString)(CGprogram, int);
    CGparameter(__cdecl* GetNamedParameter)(CGprogram, const char*);
    void(__cdecl* SetParameterValuefr)(CGparameter, int, const float*);
    int(__cdecl* GetParameterValuefr)(CGparameter, int, float*);
    unsigned long(__cdecl* GetParameterResourceIndex)(CGparameter);
    int(__cdecl* GetParameterRows)(CGparameter);
    CGparameter(__cdecl* GetFirstLeafParameter)(CGprogram, int);
    CGparameter(__cdecl* GetNextLeafParameter)(CGparameter);
    const char*(__cdecl* GetParameterName)(CGparameter);
    int(__cdecl* GetParameterVariability)(CGparameter);
    int(__cdecl* GetProgramProfile)(CGprogram);
    int(__cdecl* IsParameterReferenced)(CGparameter);
    bool ok = false;

    bool Load(void* cgDll);  // HMODULE
    void Drain() const;
};
constexpr int kCgSource = 0x1010, kCgCompiledProgram = 0x100A, kCgProgramNs = 0x100D, kCgUniform = 0x1006;

struct Job {
    const Sources* src = nullptr;
    const Cg* cg = nullptr;
    std::string mainName, entry, profile;
    std::vector<std::string> deps;  // lower-case base names
    std::vector<Issue> issues;
    std::string owner;
    bool overridden = false;
    std::vector<std::string> vfs;  // include names set in the context during the compile
};
// Compiles `text` in `ctx` with every #include resolved through the job's sources. The context's include
// callback and virtual files are restored afterwards; the Cg error state is left as cgCreateProgram left it.
CGprogram Compile(Job& job, CGcontext ctx, const std::string& text, int profile, const char** args);
}  // namespace melange::mirage::shadersrc
