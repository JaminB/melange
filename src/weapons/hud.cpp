#include "weapons/hud.h"

#include <safetyhook.hpp>

#include "weapons/engine.h"
#include "weapons/registry.h"
#include "weapons/registry_core.h"

namespace melange::weapons::hud {
namespace {
namespace eng = engine;
SafetyHookMid g_hud;
bool g_wanted = false;

// [esp+4] = the XString holding the icon file name about to be loaded.
void OnHud(safetyhook::Context& c) {
    if (const char* n = registry::Core().HudName()) eng::AssignXString(c.esp + 4, n);
}
}  // namespace

bool Create() {
    const auto& r = registry::Core();
    for (int k = 0; k < r.Count(); ++k)
        g_wanted |= !r.At(k)->decl.hudIcon.empty();
    return !g_wanted || eng::Mid(g_hud, eng::kHudIcon, &OnHud, "HUD icon");
}

bool Enable(bool on) {
    if (!g_wanted) return true;
    eng::Enable(g_hud, on);
    return static_cast<bool>(g_hud) && g_hud.enabled() == on;
}
}  // namespace melange::weapons::hud
