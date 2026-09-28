#include "core/config_schema.h"

#include <mutex>

namespace melange::config::schema {
namespace {
std::mutex& Mx() {
    static std::mutex m;
    return m;
}
std::vector<Key>& Keys() {
    static std::vector<Key> k;
    return k;
}
bool Same(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }
Key* Find(const char* section, const char* key) {
    for (auto& k : Keys())
        if (Same(k.section, section) && Same(k.key, key)) return &k;
    return nullptr;
}
}  // namespace

void Record(const char* section, const char* key, const char* def) {
    if (!section || !key) return;
    std::lock_guard lk(Mx());
    if (Key* k = Find(section, key)) {
        k->def = def ? def : "";
        return;
    }
    Keys().push_back(Key{section, key, def ? def : "", false});
}

void MarkLive(const char* section, const char* key) {
    if (!section || !key) return;
    std::lock_guard lk(Mx());
    if (Key* k = Find(section, key)) k->live = true;
    else Keys().push_back(Key{section, key, "", true});
}

std::vector<Key> All() {
    std::lock_guard lk(Mx());
    return Keys();
}
}  // namespace melange::config::schema
