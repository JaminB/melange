#pragma once
#include <cstdint>
namespace melange::shaders {
struct ProgramInfo {
    const char* file;      // "Landscape.cg"
    const char* entry;     // "LandscapeFragmentMain"
    uint8_t stage;         // 0 = vertex, 1 = fragment
    bool failed;           // the engine gave up compiling it
    bool pendingReload;    // recompiles at its next bind
    bool overridden;       // source or include came from an override root or a built-in patch
    bool glsl;             // a GLSL replacement is active for this program
    const char* owner;     // "builtin", a mod id, or "" (vanilla)
    uint32_t binds;
};
// Strings stay valid until the next reload of that program.
size_t ListPrograms(ProgramInfo* out, size_t max);
const char* Profile(uint8_t stage);  // e.g. "arbvp1" / "arbfp1"

// Marks every program whose file or entry contains `match` (or that includes a file that matches) for a lazy
// reload at its next bind. The new source is test-compiled first: on error the old program stays in use.
int Reload(const char* match);  // returns the number of programs marked

// A Cg parameter that Melange sets on every bind of (file, entry), before the engine's own values are flushed.
// n = 1..16 floats. Used by mod shaders for tunables; shown as sliders in the Shaders panel.
bool SetParam(const char* file, const char* entry, const char* param, const float* v, int n);
bool GetParam(const char* file, const char* entry, const char* param, float* v, int n);

// Override roots added from code (mods are added automatically). Later roots win over earlier ones.
int AddOverrideRoot(const wchar_t* dir, const char* owner);  // any thread; applies at the next load/reload
void RemoveOverrideRoot(int handle);

struct CompileError { const char* file; int line; const char* text; const char* owner; };
size_t LastErrors(CompileError* out, size_t max);
struct Stats { uint32_t programs, overridden, glsl, reloads, compileErrors, passthroughLoads; };
Stats GetStats();
}
