#pragma once
#include <cstdint>

#include "melange/weapons.h"

// The clone registry (Declared/Live/ActiveClone behind melange/weapons.h).
namespace melange::weapons::registry {
void OnInit();                                  // from simbridge::OnBeforeModsLoad
void OnMatchEnd();
const CloneInfo* ByDesc(uintptr_t desc);        // nullptr unless desc is a live clone's descriptor
}  // namespace melange::weapons::registry
