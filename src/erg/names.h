#pragma once
#include <string>
#include <string_view>

// Pack prefixes and level stems: pure rules, no game or OS calls.
namespace melange::erg::names {
constexpr size_t kMaxStem = 48;
constexpr std::string_view kTestPrefix = "ergtest";

std::string Prefix(std::string_view modId);          // '-' -> '_'
bool ValidSlug(std::string_view slug);                // ^[a-z0-9]{1,24}$
bool ValidPrefix(std::string_view prefix, std::string* err);   // a pack prefix; "ergtest" is reserved
// <prefix>_<slug>: the slug rule, no dot, at most kMaxStem characters, not a vanilla stem.
bool ValidStem(std::string_view stem, std::string_view prefix, std::string* err);
bool CollidesWithVanilla(std::string_view stem);      // against the stems of every vanilla WXFE_LevelDetails (S table)
std::string Key(std::string_view stem);              // "Multi.<stem>"
// Multi.<prefix>_<slug> (a pack or ergtest_ stem, never a vanilla one) or its ".S" Survivor copy.
bool ModKey(std::string_view key);
}  // namespace melange::erg::names
