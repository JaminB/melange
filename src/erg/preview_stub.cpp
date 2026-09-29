#include "erg/preview.h"
#include "oasis/providers.h"

namespace melange::erg::preview {
bool Available() { return false; }
std::string DetailKey(const std::string&, const std::string&) { return {}; }
}  // namespace melange::erg::preview

namespace melange::oasis::providers {
void InstallErgAssets() {}
}  // namespace melange::oasis::providers
