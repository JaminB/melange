#pragma once
#include <string>
#include <string_view>

// Melange.ini upgrade: the user's text plus every section and key of the template it lacks (with the template's
// comments). Every existing byte is kept; merging twice changes nothing.
namespace melange::launcher::setup {
std::string IniMerge(std::string_view templ, std::string_view user, int* added = nullptr);
int IniMissingKeys(std::string_view templ, std::string_view user);
}  // namespace melange::launcher::setup
