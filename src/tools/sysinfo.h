#pragma once
// System, GPU and plugin info for the log export.
#include <cstdint>
#include <string>
#include <vector>

namespace melange::sysinfo {
// Every *.asi / dinput8.dll next to the game exe.
struct PluginFile {
    std::string name;
    uint32_t size = 0;
    std::string version;   // FileVersion resource, or ""
    std::string sha256;
};
std::vector<PluginFile> DetectPlugins();

// system.json as a complete JSON object. GL info is only valid once the overlay has seen a GL context.
std::string CollectJson();

std::string PluginsJson();
}  // namespace melange::sysinfo
