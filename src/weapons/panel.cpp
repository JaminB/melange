#include "weapons/panel.h"

#include <safetyhook.hpp>

#include "weapons/engine.h"
#include "weapons/registry.h"
#include "weapons/registry_core.h"

namespace melange::weapons::panel {
namespace {
namespace eng = engine;
SafetyHookMid g_text, g_help;

// "Text.%s": esi = the cell's id, ecx = the name read from the name table.
void OnText(safetyhook::Context& c) { c.ecx = registry::Core().TextName(static_cast<int32_t>(c.esi), c.ecx, false); }

// "HelpText.%s%d": eax = the id, edx = the name.
void OnHelp(safetyhook::Context& c) { c.edx = registry::Core().TextName(static_cast<int32_t>(c.eax), c.edx, true); }
}  // namespace

bool Create() {
    bool ok = eng::Mid(g_text, eng::kText, &OnText, "panel name");
    ok &= eng::Mid(g_help, eng::kHelp, &OnHelp, "panel help");
    return ok;
}

bool Enable(bool on) {
    bool ok = true;
    for (auto* h : {&g_text, &g_help}) {
        eng::Enable(*h, on);
        ok &= static_cast<bool>(*h) && h->enabled() == on;
    }
    return ok;
}
}  // namespace melange::weapons::panel
