#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The lobby as Handshake tracks it, for other Melange features that talk to lobby members. Calls the Steam
// matchmaking interface directly; installs nothing. Any thread.
namespace melange::handshake::lobby {
uint64_t Current();                                   // 0 outside a lobby
uint64_t Me();
uint64_t Owner();                                     // 0 outside a lobby
std::vector<uint64_t> Members();                      // every member other than us
std::string MemberData(uint64_t member, const char* key);
void SetMyData(const char* key, const char* value);   // no-op outside a lobby
std::string Name(uint64_t member);                    // persona name, or the id as text
}  // namespace melange::handshake::lobby
