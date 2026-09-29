#include "melange/weapons.h"

namespace melange::weapons {
int On(EventFn, void*, int) { return 0; }
void RemoveOn(int) {}
QueueResult QueueExplosion(const float*) { return QueueResult::NotInExplosion; }
}  // namespace melange::weapons
