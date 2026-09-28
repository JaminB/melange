#include "melange/gamestate.h"

namespace melange::gamestate {
bool Available() { return false; }
bool Read(Snapshot*) { return false; }
bool Latest(Snapshot*) { return false; }
void Want(uint32_t) {}
int Entities(Entity*, int) { return 0; }
int Vars(Var*, int, const char*) { return 0; }
bool Var1(const char*, Var*) { return false; }
bool Peek(uintptr_t, void*, uint32_t) { return false; }
}  // namespace melange::gamestate
