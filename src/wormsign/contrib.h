#pragma once
#include <cstdint>
#include <string>
// Hash contributors behind melange/wormsign.h AddContributor.
namespace melange::wormsign::contrib {
uint64_t HashTick(uint32_t tick);              // FNV over the contributors due this tick; 0 when none
// The contributor set as "name@version,..." in name order, "" when none. A replay compares the recorded mods hashes
// only when the recording's HEAD lists the same set; while any contributor has inReplayCompare=false this returns a
// key no recording has ("!" prefix), so the mods hashes are not compared.
std::string ReplayKey();
}
