#pragma once
#include <cstdint>

// The WormsMayhem.exe builds Melange supports, shared by the game guard and the launcher.
namespace melange::game {
struct KnownProfile {
    uint32_t size;
    uint32_t timestamp;
    const char* sha256;
    const char* name;
};
// Steam depot build 64890, the exe WUMPatch calls "Steam/GOG #1077".
inline constexpr KnownProfile kProfiles[] = {
    {5713408, 1367508505, "041c8c6eb3b9f4fbaf367748f713ccb8f7bef68d13e825472c88c1ecf711ab7d", "Steam/GOG #1077"},
};
}  // namespace melange::game
