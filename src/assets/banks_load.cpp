#include "assets/banks.h"

#include <fstream>
#include <iterator>

#include "core/game.h"
#include "core/log.h"
#include "weapons/engine.h"

namespace melange::assets::banks {
namespace {
namespace engine = weapons::engine;
uint32_t g_count = 0;

std::wstring ToBackslash(std::wstring s) {
    for (auto& c : s)
        if (c == L'/') c = L'\\';
    return s;
}

bool NamesOk(const std::wstring& absDataDir, const std::string& rel, std::string* err) {
    const std::wstring absPath = absDataDir + L"\\" + ToBackslash(game::Widen(rel));
    std::ifstream f(absPath, std::ios::binary);
    if (!f) {
        if (err) *err = "could not read the bank to check its contents";
        return false;
    }
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::string> names;
    if (!BankResourceNames(bytes, &names, err)) return false;
    for (auto& n : names) {
        // engine::Lookup is the container DRM slot, so this catches the concrete risk (a bank also naming a
        // vanilla or another mod's *container*, e.g. a weapon). A resource this bank is about to create is never
        // already live, so this never refuses that one; anything that IS already live would otherwise be silently
        // overwritten by LoadBank's overwrite flag.
        if (engine::Lookup(n.c_str())) {
            if (err) *err = "the bank also declares \"" + n + "\", which already exists";
            return false;
        }
    }
    return true;
}
}  // namespace

int Load(const std::string& modId, const std::wstring& absDataDir, const std::string& gameRelDataDir,
         const std::string& rel, std::string* err) {
    if (!crcsafe::Available()) {
        if (err) *err = "the CRC table could not be verified; refusing to load a bank";
        LOG_ERROR("[assets] %s: %s", modId.c_str(), err ? err->c_str() : "");
        return -1;
    }
    if (!CheckPath(absDataDir, rel, crcsafe::Entries(), err)) {
        LOG_ERROR("[assets] %s: %s", modId.c_str(), err ? err->c_str() : "");
        return -1;
    }
    if (!NamesOk(absDataDir, rel, err)) {
        LOG_ERROR("[assets] %s: %s", modId.c_str(), err ? err->c_str() : "");
        return -1;
    }
    const std::string path = gameRelDataDir + rel;
    const int rc = engine::LoadBank(path.c_str(), /*section=*/0);
    if (rc == 0) ++g_count;
    else if (err) *err = "LoadBank returned " + std::to_string(rc);
    return rc;
}

uint32_t Count() { return g_count; }
}  // namespace melange::assets::banks
