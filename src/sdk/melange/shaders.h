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
    bool glsl;             // a GLSL replacement is currently bound for this program (available and not disabled)
    bool glslAvailable;    // a GLSL replacement file exists for this program, whether or not it's enabled right now
    const char* owner;     // "builtin", a mod id, or "" (vanilla)
    uint32_t binds;
};
// Strings stay valid until the next reload of that program.
size_t ListPrograms(ProgramInfo* out, size_t max);
const char* Profile(uint8_t stage);  // e.g. "arbvp1" / "arbfp1"

// Runtime enable/disable of a program's GLSL replacement (L0): persisted to [MirageShaders] GlslDisabled, takes
// effect at once (no restart). False if that (file, entry) has no GLSL replacement file at all.
bool SetGlslEnabled(const char* file, const char* entry, bool on);
bool GetGlslEnabled(const char* file, const char* entry);
// Pause or resume one of `owner`'s (a mod id) own GLSL replacements; not persisted, takes effect at the next bind.
bool SetOwnGlslEnabled(const char* owner, const char* file, const char* entry, bool on);

// Marks every program whose file or entry contains `match` (or that includes a file that matches) for a lazy
// reload at its next bind. The new source is test-compiled first: on error the old program stays in use.
int Reload(const char* match);  // returns the number of programs marked

// A Cg parameter that Melange sets on every bind of (file, entry), before the engine's own values are flushed.
// n = 1..16 floats. Used by mod shaders for tunables; shown as sliders in the Shaders panel. A GLSL replacement's
// uniform that has no Cg parameter of that name gets the value too (float, vec2, vec3 or vec4).
bool SetParam(const char* file, const char* entry, const char* param, const float* v, int n);
bool GetParam(const char* file, const char* entry, const char* param, float* v, int n);
// SetParam limited to a row that `owner`'s (a mod id) shaders\params.ini declares; `entry` is that row's glob.
bool SetOwnParam(const char* owner, const char* file, const char* entry, const char* param, const float* v, int n);

// Override roots added from code (mods are added automatically). Later roots win over earlier ones.
int AddOverrideRoot(const wchar_t* dir, const char* owner);  // any thread; applies at the next load/reload
void RemoveOverrideRoot(int handle);

struct CompileError { const char* file; int line; const char* text; const char* owner; };
size_t LastErrors(CompileError* out, size_t max);
struct Stats { uint32_t programs, overridden, glsl, reloads, compileErrors, passthroughLoads; };
Stats GetStats();
}
