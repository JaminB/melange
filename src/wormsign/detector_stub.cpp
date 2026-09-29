// Placeholder until the desync detector lands: no divergence is ever reported.
#include "melange/wormsign.h"

namespace melange::wormsign {
int OnDivergence(DivergenceFn, void*) { return 0; }
void RemoveOnDivergence(int) {}
}  // namespace melange::wormsign
