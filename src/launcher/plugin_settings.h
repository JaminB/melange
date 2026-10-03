#pragma once
#include <map>
#include <string>
#include <vector>

#include "tools/json_read.h"

// A plugin's settings: declared by spice.json's "settings" array, stored in Melange.ini [Mod.<id>] exactly as
// wum.config reads and writes them in the game.
namespace melange::launcher::plugins {
struct Val {
    enum class T { Bool, Num, Str } t = T::Str;
    bool b = false;
    double num = 0;
    std::string str;
    static Val B(bool v) { Val x; x.t = T::Bool; x.b = v; return x; }
    static Val N(double v) { Val x; x.t = T::Num; x.num = v; return x; }
    static Val S(std::string v) { Val x; x.t = T::Str; x.str = std::move(v); return x; }
};
struct OptionLabel { std::string option, label, help; };
struct Setting {
    std::string key, type, label, help;   // type: bool | int | float | string | enum
    Val def;
    bool hasMin = false, hasMax = false;
    double min = 0, max = 0;
    std::vector<std::string> options;
    std::vector<OptionLabel> optionLabels;
};
using Values = std::map<std::string, Val>;

bool ParseDecl(const json::Value& settingsArray, std::vector<Setting>* out, std::string* err);
// The plugin's folder under Mods\ (by id: Mods\<id>, else the folder whose spice.json has that id).
std::wstring ModFolder(const std::wstring& gameDir, const std::string& id);
bool LoadDecl(const std::wstring& gameDir, const std::string& id, std::vector<Setting>* out, std::string* err);
Values ReadValues(const std::wstring& gameDir, const std::string& id, const std::vector<Setting>& decl);
bool Validate(const std::vector<Setting>& decl, const Values& values, std::string* badKey, std::string* why);
// Writes only the keys in `values` (validated first). Every other byte of Melange.ini is kept.
bool WriteValues(const std::wstring& gameDir, const std::string& id, const std::vector<Setting>& decl, const Values& values,
                 std::string* err);
Values Defaults(const std::vector<Setting>& decl);
std::string IniText(const Setting& s, const Val& v);

bool ValFromJson(const json::Value& v, Val* out);
std::string ValJson(const Val& v);
std::string ValuesJson(const Values& v);
std::string DeclJson(const std::vector<Setting>& decl);
}  // namespace melange::launcher::plugins
