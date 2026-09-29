#pragma once
#include <cstdint>
// Hash contributors behind melange/wormsign.h AddContributor.
namespace melange::wormsign::contrib {
uint64_t HashTick(uint32_t tick);              // FNV over the contributors due this tick; 0 when none
}
