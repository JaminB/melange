#include "erg/bank.h"
#include "oasis/providers.h"

namespace melange::oasis::providers {
void InstallLevels() {}
}  // namespace melange::oasis::providers

namespace melange::erg::bank {
std::vector<uint8_t> RegistryBank(const xom::Document&, const std::vector<Entry>&, std::string* err) {
    if (err) *err = "the level service is not available in this build";
    return {};
}
}  // namespace melange::erg::bank
