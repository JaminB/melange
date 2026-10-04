#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Worms 4 / Ultimate Mayhem data read through src/xom: the level registry (SCRIPTS.XOM), level descriptors and
// language banks. Pure: bytes in, values out.
namespace melange::import {
struct RegistryEntry {
    std::string key, fileName, frontendName, frontendImage;
    std::vector<std::string> scripts;
    int levelType = -1;
};
struct Descriptor {
    std::string theme, timeOfDay, materialFile, author;
    std::optional<std::string> heightmapBase, heightmapSecond;
    uint32_t customTextureBank = 0, customDetailBank = 0;
};

// Every XContainerResourceDetails whose value is a WXFE_LevelDetails, sorted by key (ASCII).
bool ReadRegistry(const std::vector<uint8_t>& bytes, std::vector<RegistryEntry>* out, std::string* err);
// The Databank keys of a level descriptor (XDataBank of string and uint resources). `authorKey` may be empty.
bool ReadDescriptor(const std::vector<uint8_t>& bytes, const std::string& authorKey, Descriptor* out, std::string* err);
// Name -> value of every string resource in a language bank.
bool ReadStrings(const std::vector<uint8_t>& bytes, std::map<std::string, std::string>* out, std::string* err);
std::vector<std::string> SplitScripts(const std::string& list);
}  // namespace melange::import
