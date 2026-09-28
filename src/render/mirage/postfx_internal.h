#pragma once
// Post-FX internals shared by the module, the GL runner, the panel and the offline self-test.
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "melange/postfx.h"

namespace melange::mirage::postfx {
using render::Stage;

// ---------------------------------------------------------------- effect.ini (no GL)
enum class Format : uint8_t { Rgba8, Rgba16f, R8, Rg8 };
enum class ParamType : uint8_t { Float, Vec2, Vec3, Color, Int, Bool };
struct ParamDesc {
    std::string name, label;
    ParamType type = ParamType::Float;
    int n = 1;
    float def[4] = {};
    float min = 0, max = 1;
    bool hasRange = false;
};
struct TextureDesc {
    std::string name, file;
    bool linear = true, repeat = false;
};
enum class InputKind : uint8_t { Scene, Depth, Prev, Pass, Texture };
struct Input {
    InputKind kind = InputKind::Prev;
    std::string name;  // pass or texture name
};
struct PassDesc {
    std::string name, shader;
    std::vector<std::string> defines;  // "NAME" or "NAME VALUE"
    float scale = 1;
    Format format = Format::Rgba8;
    std::vector<Input> inputs;
};
struct EffectDesc {
    std::string title;
    Stage stage = Stage::PostWorld;
    int order = 0;
    bool enabled = false;
    std::vector<ParamDesc> params;
    std::vector<TextureDesc> textures;
    std::vector<PassDesc> passes;
};
// `error` gets "effect.ini(<line>): <what>".
bool ParseEffect(std::string_view text, EffectDesc* out, std::string* error);
std::string SamplerName(const Input& in);  // mg_scene, mg_depth, mg_prev, mg_pass_<n>, t_<n>
const char* FormatName(Format f);
const char* StageName(Stage s);

// ---------------------------------------------------------------- GLSL assembly (no GL)
struct Source {
    std::string text;
    int version = 120;
    std::string profile;             // "", "core" or "compatibility"
    std::vector<std::string> files;  // GLSL source-string number -> file name
};
using IncludeFn = bool (*)(const std::string& name, std::string* text, void* user);
// Prepends the version line and the defines, inlines #include "file" (through `include`), keeps line numbers.
bool BuildFragment(std::string_view text, const std::string& fileName, const std::vector<std::string>& defines,
                   IncludeFn include, void* user, Source* out, std::string* error);
std::string BuildVertex(int version, const std::string& profile);
int ParseGlslVersion(const char* s);  // "4.60 ..." -> 460, 0 if unparsable

// ---------------------------------------------------------------- persistence (no GL)
struct StackEntry {
    std::string id;
    int order = 0;
    bool enabled = false;
};
std::vector<StackEntry> ParseStack(std::string_view s);  // "<id>:<order>:<0|1>,..."
std::string FormatStack(const std::vector<StackEntry>& v);
int ParseFloats(std::string_view s, float* v, int max);  // "0.8, 1,2"; returns the count parsed
std::string FormatFloats(const float* v, int n);
bool Invert4(const float* m, float* out);  // column-major 4x4

// ---------------------------------------------------------------- effects and the GL runner
struct Target {
    unsigned tex = 0, fbo = 0;
    int w = 0, h = 0;
    Format format = Format::Rgba8;
};
struct Uniform {
    int loc = -1;
    unsigned type = 0;
};
struct PassGl {
    unsigned program = 0;
    Target target;  // unused by the last pass
    struct Sampler {
        int unit;
        Input input;
    };
    std::vector<Sampler> samplers;
    std::vector<Uniform> params;  // parallel to EffectDesc::params
    Uniform resolution, sceneResolution, time, frame, proj, invProj, view, nearFar;
};
struct Effect {
    std::string id, owner, folder;
    std::wstring dir;
    EffectDesc desc;
    bool code = false;
    melange::postfx::PassFn fn = nullptr;
    void* user = nullptr;
    int handle = 0;

    bool enabled = false, missing = false, failed = false;
    bool dirty = true;     // (re)compile before the next run
    bool reparse = false;  // re-read effect.ini at the next scan
    int order = 0;
    Stage stage = Stage::PostWorld;
    std::string error, skipReason;
    std::vector<std::array<float, 4>> values;  // parallel to desc.params

    int glGen = 0;  // ContextGeneration() the GL names below belong to
    std::vector<PassGl> passes;
    std::vector<unsigned> textures;  // parallel to desc.textures
    unsigned queries[3] = {};
    bool queryIssued[3] = {};
    int queryHead = 0;
    double gpuMs = -1, cpuMs = 0;
    uint64_t runs = 0;
};

struct FrameInput {
    unsigned sceneColor = 0, sceneDepth = 0;
    int w = 0, h = 0;
    float proj[16] = {}, invProj[16] = {}, view[16] = {};
    float nearFar[2] = {};
    float timeSec = 0;
    uint64_t frame = 0;
    bool splitCompare = false;
    bool gpuTimers = true;
};
struct RunResult {
    uint32_t passes = 0, effects = 0;
    int glErrors = 0;
};

// The first context we run in owns our GL objects. Another context current (the engine's screenshot path) is
// skipped; one that stays current for 30 calls is adopted: every GL name is forgotten and the generation bumps.
bool OwnContext();
int ContextGeneration();
// Call with a GL context current. False (and *why) without GLSL 1.20 and framebuffer objects.
bool GlReady(std::string* why);
int GlslVersion();
// Compiles every pass and loads the textures; sets failed/error. Needs GlReady().
bool Compile(Effect& e);
void Release(Effect& e);
// Runs the compiled effects of `chain` in order: reads in.sceneColor and writes the result back into it through an
// own framebuffer. Changes the bound framebuffer, viewport, program, texture bindings, blend, depth and scissor
// state: callers wrap it in render::PushState/PopState (which does not reset the program if it was 0).
RunResult Run(const std::vector<Effect*>& chain, const FrameInput& in);
void ReleaseShared();

// ---------------------------------------------------------------- module state for the panel (main thread)
std::recursive_mutex& Mutex();
const std::vector<std::shared_ptr<Effect>>& Effects();  // hold Mutex()
bool Bypassed();
void SetBypassed(bool on);
bool SplitCompare();
void SetSplitCompare(bool on);
void DrawPanel(void*);  // postfx_panel.cpp
}  // namespace melange::mirage::postfx
