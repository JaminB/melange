#include "erg/pack.h"

namespace melange::erg::pack {
bool WritePack(const PackSpec&, const std::wstring&, std::vector<std::string>* files, std::string* err) {
    if (files) files->clear();
    if (err) *err = "export is not available in this build";
    return false;
}
}  // namespace melange::erg::pack

namespace melange::erg::luagen {
bool Needed(const Scene&) { return false; }
std::string Chunk(const Scene&) { return {}; }
}  // namespace melange::erg::luagen
