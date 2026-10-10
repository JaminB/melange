#pragma once
#include <cstdint>
#include <string>

// Identity of the running game executable and well-known paths.
namespace melange::game {
struct ExeInfo {
    uint32_t fileSize = 0;
    uint32_t timestamp = 0;  // PE TimeDateStamp
    std::string sha256;             // canonical: the large-address-aware bit cleared (core/pe_laa.h)
    const char* build = "unknown";  // human-readable build name if recognised
    bool laa = false;               // the running image is large-address-aware
    bool known = false;             // true when it matches a profile we have verified addresses for
};

void Init(void* pluginModule);
const ExeInfo& Exe();
// True only for the Steam/GOG build #1077 that all hard-coded addresses are verified against.
inline bool IsKnownBuild() { return Exe().known; }
uintptr_t Base();                 // image base of WormsMayhem.exe
const std::wstring& GameDir();
const std::wstring& PluginDir();  // folder containing melange.asi
const std::wstring& DataDir();    // <PluginDir>\Melange  (logs, dumps)
std::string DescribeAddress(uintptr_t addr);  // "00401234 WormsMayhem.exe+0x1234"
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);  // UTF-8 -> UTF-16
}  // namespace melange::game
