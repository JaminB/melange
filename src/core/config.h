#pragma once
#include <string>

// Melange.ini access. Sections are module names; every module has at least "Enabled".
namespace melange::config {
void Init(const std::wstring& iniPath);
const std::wstring& Path();
int GetInt(const char* section, const char* key, int def);
bool GetBool(const char* section, const char* key, bool def);
float GetFloat(const char* section, const char* key, float def);
std::string GetString(const char* section, const char* key, const char* def);
// Writes the key only if it is missing, so a fresh ini documents every option with its default.
void EnsureKey(const char* section, const char* key, const char* def);
void SetString(const char* section, const char* key, const char* value);
// Reads are cached (a watcher thread drops the cache when the file changes); a writer that bypasses SetString calls this
// so its change is seen on the very next read.
void Invalidate();
}  // namespace melange::config
