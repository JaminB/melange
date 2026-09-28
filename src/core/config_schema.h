#pragma once
#include <string>
#include <vector>

// Every key a module declares through config::EnsureKey, with its default. "live" is set by the owning module
// when it re-reads the key at runtime (no restart needed after a change).
namespace melange::config::schema {
struct Key { std::string section, key, def; bool live; };
std::vector<Key> All();
void MarkLive(const char* section, const char* key);
void Record(const char* section, const char* key, const char* def);  // called by config::EnsureKey
}  // namespace melange::config::schema
