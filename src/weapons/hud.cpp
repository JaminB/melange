#include "weapons/hud.h"

#include <safetyhook.hpp>

#include <string>

#include "core/log.h"
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
    const auto& r = registry::Core();
    const char* n = r.HudName();
    // A vanilla weapon's replacement is keyed by the file the game is about to load, so no notion of the active weapon
    // (a spectator's view, a weapon chosen by the scheme) can pick the wrong icon. Only read the name when a rule is armed.
    if (!n && r.HudIconsLive()) {
        const std::string cur = eng::XStringValue(c.esp + 4);
        if (!cur.empty()) n = r.HudNameFor(cur.c_str());
        static int logged = 0;
        if (n && logged < 8) LOG_INFO("[weapons] HUD icon %s -> %s (%d)", cur.c_str(), n, ++logged);
    }
    if (n) eng::AssignXString(c.esp + 4, n);
}
}  // namespace

bool Create() {
    const auto& r = registry::Core();
    for (int k = 0; k < r.Count(); ++k)
        g_wanted |= !r.At(k)->decl.hudIcon.empty();
    for (int i = 0; i < r.IconCount(); ++i)
        g_wanted |= !r.IconAt(i)->decl.hudIcon.empty();
    return !g_wanted || eng::Mid(g_hud, eng::kHudIcon, &OnHud, "HUD icon");
}

bool Enable(bool on) {
    if (!g_wanted) return true;
    eng::Enable(g_hud, on);
    return static_cast<bool>(g_hud) && g_hud.enabled() == on;
}
}  // namespace melange::weapons::hud
