#pragma once
#include <cstdint>

#include "melange/weapons.h"

// Clone behaviour events (fire, tick, impact, explosion) and extra explosions. The core (behaviour_core.cpp) holds the
// observers, the explosion window and the counters without touching the game; behaviour.cpp owns the engine hooks.
namespace melange::weapons::behaviour {
constexpr int kMaxExtraCap = 8;
constexpr float kMaxOffset = 2000.0f;

void SetExtraLimit(int n);                      // clamped to 0..kMaxExtraCap
int ExtraLimit();
void Raise(const EventArgs& a);                 // On() observers, ascending order, then registration

// The window in which QueueExplosion() accepts offsets: opened for one explosion event, closed before the original
// CreateExplosion runs. Extras are absolute positions (origin + d).
void OpenExplosion(const float origin[3]);
int CloseExplosion(float out[][3], int max);    // returns the queued extras and clears them
bool InExplosion();

struct Counters {
    uint32_t fires, ticks, impacts, explosions, extras;
};
Counters GetCounters();
void CountEvent(Event e);
void CountExtra();
void ResetCounters();

// behaviour.cpp (the plugin): hooks created disabled at Install, enabled only while Live().
void InstallLua();                              // wum.sim.weapons reaches the registry
bool Install(int extraLimit, bool logEvents);
void OnMatchBegin();
void OnMatchEnd();
bool HooksEnabled();
}  // namespace melange::weapons::behaviour
