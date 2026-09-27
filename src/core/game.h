#pragma once
#include <cstdint>
#include <string>

// Identity of the running game executable and well-known paths.
namespace wf::game {
struct ExeInfo {
    uint32_t fileSize = 0;
    uint32_t timestamp = 0;  // PE TimeDateStamp
    std::string sha256;
    const char* build = "unknown";  // human-readable build name if recognised
    bool known = false;             // true when it matches a profile we have verified addresses for
};

void Init(void* pluginModule);
const ExeInfo& Exe();
// True only for the Steam/GOG build #1077 that all hard-coded addresses are verified against.
inline bool IsKnownBuild() { return Exe().known; }
uintptr_t Base();                 // image base of WormsMayhem.exe
const std::wstring& GameDir();
const std::wstring& PluginDir();  // folder containing WUMFix.asi
const std::wstring& DataDir();    // <PluginDir>\WUMFix  (logs, dumps)
std::string DescribeAddress(uintptr_t addr);  // "00401234 WormsMayhem.exe+0x1234"
std::string Narrow(const std::wstring& w);
}  // namespace wf::game
