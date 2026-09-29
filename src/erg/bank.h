#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "xom/xom.h"

// The registry bank: one WXFE_LevelDetails per level under "Multi.<stem>", TYPE records taken from the user's own
// SCRIPTS.XOM.
namespace melange::erg::bank {
struct Entry {
    std::string key, stem, frontendName, scripts;   // scripts: "stdvs,wormpot[,<stem>]"
    int levelType = 0, themeType = 5;
};
std::vector<uint8_t> RegistryBank(const xom::Document& scriptsXom, const std::vector<Entry>& entries, std::string* err);
}  // namespace melange::erg::bank
