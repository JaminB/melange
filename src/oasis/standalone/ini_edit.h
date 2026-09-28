#pragma once
#include <string>

// A small, pure text editor for Melange.ini: reads one key, or edits one key in place without disturbing any
// other byte (comments, blank lines, key order). Offline-testable (tests/oasis_standalone_selftest.cpp).
namespace melange::oasis::standalone::ini {
// "" (not found) is indistinguishable from an empty value; callers use Set to know whether the key exists.
std::string Get(const std::string& text, const std::string& section, const std::string& key);
// Replaces the key's value (adding the section and/or key if missing) and returns the new file text.
// False (text unchanged) if `value` cannot be represented on one ini line (a line break or a ';').
bool Set(const std::string& text, const std::string& section, const std::string& key, const std::string& value,
         std::string* out);
}  // namespace melange::oasis::standalone::ini
