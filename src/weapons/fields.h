#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "melange/weapons.h"

// Field offsets of weapon container classes, read from the engine's class field list (class+0x44..0x48, each entry
// a property whose +4 is a descriptor {char* name, u16 offset, u8 index, u8 flags}), typed by src/xom's schema.
// Cached per class for the launch. The first walk checks the seven offsets verified at runtime; if any disagrees,
// only those seven are served.
namespace melange::weapons::fields {
struct Entry {
    std::string name;
    uint32_t offset;
    FieldType type;
    uint8_t flags;
};
FieldType Find(uintptr_t cls, const char* field, uint32_t* offset);
const std::vector<Entry>* ForClass(uintptr_t cls);  // nullptr if the class could not be walked
bool SelfCheck();                                   // walks the payload container class; logs the verdict once
bool Trusted();                                     // SelfCheck() passed
FieldType SchemaType(const char* className, const char* field);  // offline: the schema's view (None if unknown)
constexpr uintptr_t kPayloadClass = 0x9688e0;
}  // namespace melange::weapons::fields
