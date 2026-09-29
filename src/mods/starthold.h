#pragma once
#include <cstdint>
#include <string>

// The host-side start hold shared by weapons and maps. The host's WaitingGameStart compares the players that have a
// team with all players before it posts the begin-game message; while any registered reason holds, that compare is
// made to fail, so the start waits and nothing is sent. Nothing here kicks anyone. Main thread.
namespace melange::mods::starthold {
using ReasonFn = bool (*)(std::string* why, void* user);   // true = hold the start now
int Add(const char* name, ReasonFn fn, void* user);        // M5's weapon gate is re-registered through this
void Remove(int handle);
bool Holding(std::string* why);

bool Available();                                          // false once creating the hook failed
bool HookEnabled(int* state);                              // state: -1 not created, 0 off, 1 on
uint32_t HeldFrames();                                     // frames the begin-game compare was failed so far
uint32_t HeldFrames(const char* name);                     // ...while this reason held
}  // namespace melange::mods::starthold
