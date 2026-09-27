#pragma once
// System/GPU info for the "Save logs as..." export (component D). Not a frozen sdk header: private to
// src/tools/, built for wf::exporter's system.json (docs/m0-design.md §3 "D", Layout).
#include <cstdint>
#include <string>
#include <vector>

namespace wf::sysinfo {
// One `mods/plugins.json` entry: every *.asi / dinput8.dll found next to the game exe.
struct PluginFile {
    std::string name;      // file name only
    uint32_t size = 0;
    std::string version;   // FileVersion from the version resource, or "" if none
    std::string sha256;
};
std::vector<PluginFile> DetectPlugins();

// Returns a complete JSON object (with braces) for system.json: OS, CPU, RAM, display, GL (from
// wf::overlay::Gl(), which is only meaningful once the real overlay component has created a GL
// context; until then gl.valid is false), exe identity, locale and detected sibling plugins.
std::string CollectJson();

// Same shape as system.json's "mods.plugins" array, exposed separately for mods/plugins.json.
std::string PluginsJson();
}  // namespace wf::sysinfo
