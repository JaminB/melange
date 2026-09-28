#pragma once
#include <string>
#include <string_view>
#include <vector>

// Melange.ini as text, read and edited the way GetPrivateProfileString sees it: sections and keys match
// case-insensitively, the first section and the first key win, and a value ends at an inline ';' comment.
// Editing changes one line in place and keeps every other byte. No game dependency.
namespace melange::oasis::ini {
struct Entry {
    std::string section, key, value;  // value: trimmed, inline comment removed (config::GetString's view)
    int line;                         // 1-based
};
std::vector<Entry> Parse(std::string_view text);
const Entry* Find(const std::vector<Entry>& all, std::string_view section, std::string_view key);

bool ValidName(std::string_view s, std::string* why);
bool ValidValue(std::string_view s, std::string* why);
// Keys the browser may not change: the Deep Desert grant can be revoked from the page but never granted.
bool Protected(std::string_view section, std::string_view key, std::string_view value, std::string* why);

// `text` with section/key set to value: the existing line keeps its indentation, key spelling, spacing and inline
// comment; a missing key goes after the section's last key; a missing section is appended. Line endings follow
// the file.
std::string Set(std::string_view text, std::string_view section, std::string_view key, std::string_view value);

enum class Encoding { Ansi, Utf8Bom, Utf16Le };
// File bytes <-> UTF-8 text. Ansi is the system code page (what WritePrivateProfileString writes); Encode fails when
// a character has no representation in it.
std::string Decode(std::string_view bytes, Encoding* enc);
bool Encode(std::string_view utf8, Encoding enc, std::string* bytes);
}  // namespace melange::oasis::ini
