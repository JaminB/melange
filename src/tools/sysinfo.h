#pragma once
// System, GPU and plugin info for the log export. sysinfo_core.cpp needs no game (Melange.exe links it too);
// sysinfo.cpp adds what only the running game knows (GL, the identified exe).
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
std::vector<PluginFile> DetectPlugins();  // in the running game's folder

// system.json as a complete JSON object. GL info is only valid once the overlay has seen a GL context.
std::string CollectJson();

std::string PluginsJson();

// No game needed (sysinfo_core.cpp).
std::vector<PluginFile> DetectPluginsIn(const std::wstring& gameDir);
std::string PluginsJsonIn(const std::wstring& gameDir);
// system.json from its parts: `glJson` and `exeJson` are complete JSON objects the caller supplies.
std::string CollectJsonWith(const std::wstring& gameDir, const std::string& glJson, const std::string& exeJson);
// system.json without the game running (Melange.exe): gl.valid false, the exe read from the file on disk.
std::string OfflineJson(const std::wstring& gameDir);
std::string FileVersionOf(const std::wstring& path);  // "a.b.c.d" from the version resource, or ""
}  // namespace melange::sysinfo
