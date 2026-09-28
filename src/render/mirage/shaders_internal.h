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
bool FxaaToggle(bool on, std::string* why);
std::string LastToast(uint64_t* tick);

// shaders_glsl.cpp (per-program GLSL replacement, experimental)
namespace glsl {
bool Configure(bool enabled, bool profileExperiment);  // Install time
bool Has(const std::string& file, const std::string& entry);
void OnCreate(CGprogram p, const std::string& file, const std::string& entry, int stage, const std::wstring& vanillaDir);
void OnBind(CGprogram p);  // after the real cgGLBindProgram
void OnFileChanged(const std::wstring& path);
bool Active(const std::string& file, const std::string& entry);
uint32_t ActiveCount();
bool Installed();
}  // namespace glsl

// shaders_panel.cpp
void RegisterPanel();
}  // namespace melange::mirage::shaders
