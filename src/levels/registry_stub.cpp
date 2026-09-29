#include "levels/registry.h"

#include <cstdio>

namespace melange::levels::registry {
namespace {
bool Refuse(char* err, size_t errLen) {
    if (err && errLen) snprintf(err, errLen, "level registration is not available in this build");
    return false;
}
}  // namespace

void Install(const Config&) {}
void OnFrame() {}
bool HasModLevels() { return false; }
int List(LevelInfo*, int, bool) { return 0; }
bool Find(const char*, LevelInfo*) { return false; }
Stats GetStats() { return {}; }
bool RegisterTest(const char*, const char*, char* err, size_t errLen) { return Refuse(err, errLen); }
bool Arm(const char*, int) { return false; }
void Disarm() {}
bool Armed(char* key, size_t keyLen) {
    if (key && keyLen) key[0] = 0;
    return false;
}
const char* TakeOverride(const char*) { return nullptr; }
bool Keep(const char*, uint32_t) { return true; }
Online Status(const char*) { return Online::Allowed; }
}  // namespace melange::levels::registry
