#pragma once
#include <string>

// The Oasis methods and routes served with the game closed (Melange.exe): logs, captures, replays, mods and
// Melange.ini, all read from the game folder at call time so the folder can be chosen or changed while serving.
namespace melange::oasis::standalone {
struct StandaloneHost {
    std::wstring (*gameDir)() = nullptr;      // "" until a folder is chosen
    std::string (*writeGate)() = nullptr;     // "" when writes to the folder are allowed now, else user copy
    const char* version = "";
    std::wstring (*defaultsIni)() = nullptr;  // the shipped Melange.ini (keys and defaults for ini.get), "" if none
};
void RegisterStandalone(const StandaloneHost& host);
// The Erg level service and asset route, bound to one game folder (installed once, when a folder is first known).
void RegisterLevels(const std::wstring& gameDir);
// Where the game writes session logs: [Logging] Dir, else Documents\Melange\logs, else <game>\Melange\logs.
std::wstring LogsDir(const std::wstring& gameDir);
std::string IniGet(const std::wstring& gameDir, const char* section, const char* key, const char* def);
}  // namespace melange::oasis::standalone
