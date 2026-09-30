// Surround painting is not available in this build.
#include "erg/hmp.h"

namespace melange::erg::hmp {
bool Read(const std::vector<uint8_t>&, Surround*, std::string* err) {
    if (err) *err = "surround painting is not available in this version";
    return false;
}

std::vector<uint8_t> Write(const Surround&) { return {}; }

bool ApplyRuns(Surround&, const std::vector<HmpRun>&, const std::vector<HmpRun>&, std::string* err) {
    if (err) *err = "surround painting is not available in this version";
    return false;
}
}  // namespace melange::erg::hmp
