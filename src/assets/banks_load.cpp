#include "assets/banks.h"

#include "core/log.h"
#include "weapons/engine.h"

namespace melange::assets::banks {
namespace {
namespace engine = weapons::engine;
uint32_t g_count = 0;
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
    const std::string path = gameRelDataDir + rel;
    const int rc = engine::LoadBank(path.c_str(), /*section=*/0);
    if (rc == 0) ++g_count;
    else if (err) *err = "LoadBank returned " + std::to_string(rc);
    return rc;
}

uint32_t Count() { return g_count; }
}  // namespace melange::assets::banks
