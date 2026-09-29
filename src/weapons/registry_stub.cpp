#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "weapons/manifest.h"
#include "weapons/registry.h"

namespace melange::weapons {
namespace {
void Fill(const manifest::CloneDecl& d, CloneInfo* o) {
    *o = CloneInfo{};
    o->k = d.k;
    o->vid = kVidBase + d.k;
    o->base = d.baseId;
    strncpy_s(o->name, d.name.c_str(), _TRUNCATE);
    strncpy_s(o->mod, d.mod.c_str(), _TRUNCATE);
    o->cell = static_cast<int8_t>(d.cell);
}
}  // namespace

int Declared(CloneInfo* out, int max) {
    const auto& all = manifest::Frozen();
    const int n = static_cast<int>(all.size());
    for (int i = 0; out && i < std::min(n, max); ++i) Fill(all[i], &out[i]);
    return n;
}

bool Live() { return false; }

int ActiveClone() { return -1; }

bool IsVid(int32_t v) { return v >= kVidBase && v < kVidBase + kMaxClones; }

int32_t BaseOf(int32_t v) {
    const auto& all = manifest::Frozen();
    if (IsVid(v) && static_cast<size_t>(v - kVidBase) < all.size()) return all[v - kVidBase].baseId;
    LOG_ERROR("[weapons] unknown virtual id %x mapped to the Bazooka", static_cast<unsigned>(v));
    return 1;
}

namespace registry {
void OnInit() {}
void OnMatchEnd() {}
const CloneInfo* ByDesc(uintptr_t) { return nullptr; }
}  // namespace registry
}  // namespace melange::weapons
