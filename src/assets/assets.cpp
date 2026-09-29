// melange::assets: the public facade of melange/assets.h. Resolves a mod id to its folder through Thumper, then
// delegates to roots/banks/icons, which do the actual validation and engine calls.
#include <cstdio>
#include <string>

#include "assets/banks.h"
#include "assets/icons.h"
#include "assets/roots.h"
#include "assets/upload.h"
#include "core/game.h"
#include "melange/assets.h"
#include "mods/thumper_internal.h"

namespace melange::assets {
namespace {
bool Fail(char* err, size_t errLen, const std::string& why) {
    if (err && errLen) snprintf(err, errLen, "%s", why.c_str());
    return false;
}

bool FindMod(const char* modId, thumper::Entry* out, char* err, size_t errLen) {
    if (!modId || !*modId) {
        Fail(err, errLen, "no mod id");
        return false;
    }
    if (!thumper::FindEntry(modId, out)) {
        Fail(err, errLen, "unknown mod id");
        return false;
    }
    return true;
}
}  // namespace

bool AddModRoot(const char* modId, char* err, size_t errLen) {
    thumper::Entry e;
    if (!FindMod(modId, &e, err, errLen)) return false;
    const std::wstring looseDir = e.dir + L"\\" + game::Widen(e.manifest.assetsRoot) + L"\\loose";
    const std::string gameRel = "Mods/" + std::string(modId) + "/" + e.manifest.assetsRoot + "/loose";
    std::string werr;
    if (!roots::Add(modId, looseDir, gameRel, &werr)) return Fail(err, errLen, werr);
    return true;
}

int LoadModBank(const char* modId, const char* rel, char* err, size_t errLen) {
    if (!rel || !*rel) {
        Fail(err, errLen, "no bank path");
        return -1;
    }
    thumper::Entry e;
    if (!FindMod(modId, &e, err, errLen)) return -1;
    const std::wstring dataDir = e.dir + L"\\" + game::Widen(e.manifest.assetsRoot) + L"\\data";
    const std::string gameRelDir = "Mods/" + std::string(modId) + "/" + e.manifest.assetsRoot + "/data/";
    std::string werr;
    const int rc = banks::Load(modId, dataDir, gameRelDir, rel, &werr);
    if (rc != 0 && !werr.empty()) Fail(err, errLen, werr);
    return rc;
}

bool ReservePanelIcon(const char* modId, const char* relPng, uint32_t* iconCode, char* err, size_t errLen) {
    if (iconCode) *iconCode = 0;
    if (!relPng || !*relPng) return Fail(err, errLen, "no icon path");
    thumper::Entry e;
    if (!FindMod(modId, &e, err, errLen)) return false;
    const std::wstring assetsDir = e.dir + L"\\" + game::Widen(e.manifest.assetsRoot);
    std::string werr;
    if (!icons::Reserve(modId, assetsDir, relPng, iconCode, &werr)) return Fail(err, errLen, werr);
    return true;
}

Stats GetStats() {
    Stats s{};
    s.roots = roots::Count();
    s.banks = banks::Count();
    s.icons = icons::Count();
    const auto u = upload::GetStats();
    s.uploadsPatched = u.uploadsPatched;
    s.msLastPatch = u.msLastPatch;
    return s;
}
}  // namespace melange::assets
