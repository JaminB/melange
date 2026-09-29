// Placeholder until the contributor registry lands: no contributors, mods hash 0.
#include "melange/wormsign.h"
#include "wormsign/contrib.h"

namespace melange::wormsign {
int AddContributor(const char*, ContribFn, void*, const ContribOptions&) { return 0; }
void RemoveContributor(int) {}
namespace contrib {
uint64_t HashTick(uint32_t) { return 0; }
}
}  // namespace melange::wormsign
