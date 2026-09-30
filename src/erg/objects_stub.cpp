// Level objects are not available in this build.
#include "erg/objects.h"

namespace melange::erg::objects {
bool LoadCatalog(const std::filesystem::path&, Catalog*, std::string* err) {
    if (err) *err = "level objects are not available in this version";
    return false;
}

bool Validate(const Scene&, const Catalog&, std::vector<std::string>*, std::string* err) {
    if (err) *err = "level objects are not available in this version";
    return false;
}

std::string NextKnot(const Scene&, ObjectType, int) { return {}; }
}  // namespace melange::erg::objects
