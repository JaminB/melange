// Placeholder until the recorder lands: no captures, no RNG taps, an empty library.
#include "melange/wormsign.h"
#include "wormsign/capture.h"
#include "wormsign/rngtap.h"

namespace melange::wormsign {
int Library(ReplayInfo*, int) { return 0; }
bool Pin(const wchar_t*, bool) { return false; }
namespace capture {
void SetGate(GateFn) {}
bool InjectSend(int, uint16_t, uint32_t, uint32_t, const char*, uint32_t) { return false; }
bool LocalOnly(uint16_t) { return false; }
}  // namespace capture
namespace rngtap {
bool Install() { return false; }
void SetForcedDraw(ForcedDrawFn) {}
void SetForcedSeed(ForcedSeedFn) {}
}  // namespace rngtap
}  // namespace melange::wormsign
