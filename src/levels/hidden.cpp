#include "levels/hidden.h"

#include <windows.h>

#include <cstdio>

namespace melange::levels::hidden {
namespace {
bool StemChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

bool ValidStem(std::string_view s) {
    if (s.empty() || s.size() > 48) return false;
    for (char c : s)
        if (!StemChar(c)) return false;
    return true;
}
}  // namespace

std::set<std::string> Parse(std::string_view text) {
    std::set<std::string> out;
    if (text.size() > kMaxBytes) text = text.substr(0, kMaxBytes);
    size_t p = 0, lines = 0;
    while (p < text.size() && lines < kMaxLines) {
        size_t q = text.find('\n', p);
        if (q == std::string_view::npos) q = text.size();
        std::string_view line = text.substr(p, q - p);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (ValidStem(line)) out.emplace(line);
        ++lines;
        p = q + 1;
    }
    return out;
}

std::string Serialize(const std::set<std::string>& stems) {
    std::string s;
    size_t n = 0;
    for (const auto& st : stems) {
        if (!ValidStem(st)) continue;
        if (++n > kMaxLines || s.size() + st.size() + 1 > kMaxBytes) break;
        s += st;
        s += '\n';
    }
    return s;
}

std::set<std::string> Load(const std::wstring& gameDir) {
    std::string text;
    FILE* f = gameDir.empty() ? nullptr : _wfopen((gameDir + L"\\" + kRel).c_str(), L"rb");
    if (!f) return {};
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && text.size() <= kMaxBytes) text.append(buf, n);
    fclose(f);
    return Parse(text);
}

bool Save(const std::wstring& gameDir, const std::set<std::string>& stems) {
    if (gameDir.empty()) return false;
    const std::wstring path = gameDir + L"\\" + kRel;
    const std::string data = Serialize(stems);
    if (data.empty()) return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    CreateDirectoryW((gameDir + L"\\Melange").c_str(), nullptr);
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr) && w == data.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

std::string StemOfKey(std::string_view key) {
    if (key.size() <= 6 || key.substr(0, 6) != "Multi.") return {};
    std::string_view s = key.substr(6);
    if (s.size() > 2 && s.substr(s.size() - 2) == ".S") s.remove_suffix(2);
    return ValidStem(s) ? std::string(s) : std::string();
}
}  // namespace melange::levels::hidden
