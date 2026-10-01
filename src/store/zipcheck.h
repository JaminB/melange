#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The plugin store's zip rules, applied to a central-directory listing before a byte is written. Pure.
namespace melange::store::zipcheck {
constexpr size_t kMaxEntries = 2000;
constexpr uint64_t kMaxEntryBytes = 32ull << 20;
constexpr size_t kMaxPathBytes = 180, kMaxSegments = 8;

struct Entry {
    std::string name;
    uint64_t compSize = 0, size = 0;
    uint16_t method = 0, flags = 0, madeBy = 0;
    uint32_t externalAttr = 0;
};

bool CheckName(std::string_view name, std::string_view id, std::string* why);
bool Check(const std::vector<Entry>& entries, std::string_view id, uint64_t maxTotal, std::string* why);
bool ExecutableName(std::string_view name);
bool ExecutableMagic(const void* head, size_t n);   // PE "MZ" or "\x7fELF"
}  // namespace melange::store::zipcheck
