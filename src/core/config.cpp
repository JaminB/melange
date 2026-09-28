#include "core/config.h"

#include <windows.h>

#include <cstdlib>

namespace melange::config {
namespace {
std::wstring g_path;

std::wstring W(const char* s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
}  // namespace

void Init(const std::wstring& iniPath) { g_path = iniPath; }
const std::wstring& Path() { return g_path; }

std::string GetString(const char* section, const char* key, const char* def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(W(section).c_str(), W(key).c_str(), W(def).c_str(), buf, 1024, g_path.c_str());
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, buf, -1, s.data(), n, nullptr, nullptr);
    // strip inline comments ("value ; comment")
    if (auto p = s.find(';'); p != std::string::npos) s.resize(p);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

int GetInt(const char* section, const char* key, int def) {
    auto s = GetString(section, key, "");
    if (s.empty()) return def;
    return static_cast<int>(strtol(s.c_str(), nullptr, 0));
}

bool GetBool(const char* section, const char* key, bool def) { return GetInt(section, key, def ? 1 : 0) != 0; }

float GetFloat(const char* section, const char* key, float def) {
    auto s = GetString(section, key, "");
    return s.empty() ? def : static_cast<float>(atof(s.c_str()));
}

void EnsureKey(const char* section, const char* key, const char* def) {
    wchar_t buf[8];
    GetPrivateProfileStringW(W(section).c_str(), W(key).c_str(), L"\x1", buf, 8, g_path.c_str());
    if (buf[0] == L'\x1') WritePrivateProfileStringW(W(section).c_str(), W(key).c_str(), W(def).c_str(), g_path.c_str());
}
}  // namespace melange::config
