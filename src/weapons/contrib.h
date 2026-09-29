#pragma once
#include <cstdint>

#include "melange/wormsign.h"

// The Wormsign contributor "melange.weapons", registered only when clones are declared. Per tick it feeds Live(),
// ActiveClone(), each clone's base name slot (swapped or not), the fire/explosion/extra counts of the match, and the
// raw bytes of every clone field written by the manifest's `set` or by wum.sim.weapon (string fields by content).
namespace melange::weapons::contrib {
constexpr const char* kName = "melange.weapons";
bool Register();                 // no-op without declared clones; captures the vanilla name slots
void Unregister();
void Feed(wormsign::Hasher& h);  // one tick's bytes (the contributor body)
}  // namespace melange::weapons::contrib
