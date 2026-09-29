#include "mods/weapon_gate.h"

namespace melange::mods {
bool CloneLobbyOk(std::string* why) {
    if (why) why->clear();
    return true;
}
}  // namespace melange::mods
