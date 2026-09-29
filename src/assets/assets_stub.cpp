#include <cstdio>

#include "assets/searchpath.h"
#include "assets/upload.h"
#include "melange/assets.h"

namespace melange::assets {
namespace {
bool NotYet(char* err, size_t errLen) {
    if (err && errLen) snprintf(err, errLen, "mod assets are not available in this build");
    return false;
}
}  // namespace

bool AddModRoot(const char*, char* err, size_t errLen) { return NotYet(err, errLen); }

int LoadModBank(const char*, const char*, char* err, size_t errLen) {
    NotYet(err, errLen);
    return -1;
}

bool ReservePanelIcon(const char*, const char*, uint32_t* iconCode, char* err, size_t errLen) {
    if (iconCode) *iconCode = 0;
    return NotYet(err, errLen);
}

Stats GetStats() {
    const auto u = upload::GetStats();
    Stats s{};
    s.roots = static_cast<uint32_t>(searchpath::Added().size());
    s.uploadsPatched = u.uploadsPatched;
    s.msLastPatch = u.msLastPatch;
    return s;
}
}  // namespace melange::assets
