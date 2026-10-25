#pragma once
#include <string>
#include <vector>

// Sim fixes that must run on every machine of an online match or on none (handshake_internal.h: the "mlg.fix" member
// key and the owner's "mlg.fix.on" lobby key). A fix's module advertises it at install; the Handshake publishes the
// list with this machine's member data and, as the lobby owner, keeps the agreed list in the lobby data; the module
// asks Agreed() once when a match starts and tells the rest of Melange (the Wormsign recorder) what it runs with
// SetActive(). Main thread, except Advertised() (the Handshake's publish thread).
namespace melange::handshake::simfix {
constexpr const char* kDrumFuse = "drum";                // [NetSession] FixDrumFuse

void Advertise(const char* name);                       // this machine runs `name` online when every member does
std::vector<std::string> Advertised();
bool Agreed(const char* name);                          // the lobby's agreed list, written by its current owner, names it
std::string Why(const char* name);                      // for the log when it is not agreed: who lacks it
void SetActive(const std::vector<std::string>& names);  // the fixes running in the current match ([] outside one)
std::vector<std::string> Active();
}  // namespace melange::handshake::simfix
