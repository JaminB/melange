#pragma once
#include <set>
#include <string>
#include <string_view>

// Melange\hidden-levels.txt: pack level stems the player took out of their own map list and random pools. The bank
// entries stay registered, so a host who picks a hidden level still loads it everywhere.
namespace melange::levels::hidden {
constexpr const wchar_t* kRel = L"Melange\\hidden-levels.txt";
constexpr size_t kMaxLines = 1024, kMaxBytes = 64 * 1024;

std::set<std::string> Parse(std::string_view text);   // one stem per line; bad lines are skipped
std::string Serialize(const std::set<std::string>& stems);
std::set<std::string> Load(const std::wstring& gameDir);
bool Save(const std::wstring& gameDir, const std::set<std::string>& stems);   // atomic; an empty set deletes the file
// The stem of "Multi.<stem>" or "Multi.<stem>.S", "" for any other key.
std::string StemOfKey(std::string_view key);
}  // namespace melange::levels::hidden
