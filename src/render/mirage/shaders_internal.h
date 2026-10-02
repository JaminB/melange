#pragma once
// Internal interfaces between the MirageShaders translation units.
#include <cstdint>
#include <string>
#include <vector>

#include "render/mirage/shaders_source.h"

namespace melange::mirage::shaders {
using shadersrc::CGprogram;

// shaders.cpp
const shadersrc::Cg& CgApi();
// Override roots (enabled mods' shaders\ folders, then code roots) over the engine's CG folder.
shadersrc::Sources MakeSources(const std::wstring& vanillaDir);
void ReportIssue(const shadersrc::Issue& i);
const char* Intern(const std::string& s);
struct ParamRow {
    std::string file, entryGlob, name, label, type, owner;
    int n;
    float v[16], min, max;
    bool hasSpec;
};
std::vector<ParamRow> Params();
// Parameter values for GLSL-only uniforms; `entry` is a concrete program entry matched against the rows' globs.
bool HasParam(const std::string& file, const std::string& entry, const std::string& name);
bool ParamValue(const std::string& file, const std::string& entry, const std::string& name, float* v, int n);
uint32_t ParamsVersion();  // bumped on every parameter change
bool FxaaToggle(bool on, std::string* why);
std::string LastToast(uint64_t* tick);

// shaders_glsl.cpp (per-program GLSL replacement, experimental)
namespace glsl {
bool Configure(bool enabled, bool profileExperiment);  // Install time
bool HasFile(const std::string& file, const std::string& entry);  // a replacement file exists, regardless of the toggle
bool Has(const std::string& file, const std::string& entry);      // HasFile() and not administratively disabled
void OnCreate(CGprogram p, const std::string& file, const std::string& entry, int stage, const std::wstring& vanillaDir);
void OnBind(CGprogram p);  // after the real cgGLBindProgram
void OnFileChanged(const std::wstring& path);
bool Active(const std::string& file, const std::string& entry);
std::string Owner(const std::string& file, const std::string& entry);
uint32_t ActiveCount();
bool Installed();
// Runtime toggle (L0): persisted to [MirageShaders] GlslDisabled, takes effect immediately, no restart needed.
bool IsEnabled(const std::string& file, const std::string& entry);
bool SetEnabled(const std::string& file, const std::string& entry, bool on);  // false if no replacement file exists
// Pause/resume by the mod that ships the file (not persisted); applied at the next program bind.
bool SetModEnabled(const std::string& owner, const std::string& file, const std::string& entry, bool on);
void Profile(bool on);  // on: reset and start counting; off: log the totals
}  // namespace glsl

// shaders_panel.cpp
void RegisterPanel();
}  // namespace melange::mirage::shaders
