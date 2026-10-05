#pragma once
#include <string>
#include <vector>

#include "launcher/plugin_settings.h"

// %LOCALAPPDATA%\Melange\launcher.json: the chosen game folder, theme, window placement and the user's defaults.
namespace melange::launcher {
struct DefaultPlugin {
    std::string id;
    bool enabled = true;
    plugins::Values settings;
};
struct WindowState {
    bool saved = false, maximized = false;
    int left = 0, top = 0, right = 0, bottom = 0;   // restored rect, in screen pixels
};
struct Settings {
    std::wstring gameDir;
    bool firstRunDone = false;
    std::string theme = "system";   // system | light | dark
    WindowState window;
    std::vector<DefaultPlugin> defaults;
    bool defaultsSeeded = false;
    std::string lastUpdateCheck;    // ISO time of the last look at GitHub for a newer Melange
};
std::wstring SettingsPath();
bool LoadSettings(const std::wstring& path, Settings* out);   // false (and defaults) when missing or unreadable
bool SaveSettings(const std::wstring& path, const Settings& s);
std::string DefaultsJson(const std::vector<DefaultPlugin>& d, bool seeded);
bool DefaultsFromJson(const json::Value& v, std::vector<DefaultPlugin>* out, bool* seeded, std::string* err);
}  // namespace melange::launcher
