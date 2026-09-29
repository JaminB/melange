#include "weapons/vid.h"

#include <safetyhook.hpp>

#include "weapons/engine.h"
#include "weapons/registry.h"
#include "weapons/registry_core.h"

namespace melange::weapons::vid {
namespace {
namespace eng = engine;
SafetyHookMid g_sel, g_selLog, g_inv, g_allowed, g_delay, g_canUse;
SafetyHookMid* const kAll[] = {&g_sel, &g_selLog, &g_inv, &g_allowed, &g_delay, &g_canUse};

int32_t I(uintptr_t r) { return static_cast<int32_t>(r); }

// ebp = the panel value about to be stored in wormData+0xf4 (eax = wormData).
void OnSelect(safetyhook::Context& c) { c.ebp = static_cast<uintptr_t>(registry::Core().Select(I(c.ebp))); }

// eax = the id about to index the name table or an inventory, delay or allowed table.
void OnId(safetyhook::Context& c) { c.eax = static_cast<uintptr_t>(registry::Core().GuardId(I(c.eax))); }

// edi = the id, edx = &name slot passed to the container lookup.
void OnCanUse(safetyhook::Context& c) { c.edx = registry::Core().CanUseSlot(I(c.edi), c.edx); }
}  // namespace

bool Create() {
    bool ok = eng::Mid(g_sel, eng::kSelWrite, &OnSelect, "weapon selection");
    ok &= eng::Mid(g_selLog, eng::kSelLog, &OnId, "selection log name");
    ok &= eng::Mid(g_inv, eng::kInvGet, &OnId, "inventory id guard");
    ok &= eng::Mid(g_allowed, eng::kAllowed, &OnId, "allowed id guard");
    ok &= eng::Mid(g_delay, eng::kDelay, &OnId, "delay id guard");
    ok &= eng::Mid(g_canUse, eng::kCanUse, &OnCanUse, "can-use id guard");
    return ok;
}

bool Enable(bool on) {
    bool ok = true;
    for (auto* h : kAll) {
        eng::Enable(*h, on);
        ok &= static_cast<bool>(*h) && h->enabled() == on;
    }
    return ok;
}
}  // namespace melange::weapons::vid
