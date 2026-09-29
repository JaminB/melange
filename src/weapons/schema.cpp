#include <cstring>

#include "weapons/fields.h"
#include "xom.h"

namespace melange::weapons::fields {
namespace {
FieldType FromXom(const xom::FieldDef& f) {
    if (f.isArray()) return FieldType::None;
    switch (f.type) {
        case xom::Type::F32: return FieldType::F32;
        case xom::Type::I32: return FieldType::I32;
        case xom::Type::U32: return FieldType::U32;
        case xom::Type::U16: return FieldType::U16;
        case xom::Type::U8: return FieldType::U8;
        case xom::Type::Bool: return FieldType::Bool;
        case xom::Type::String: return FieldType::String;
        default: return FieldType::None;
    }
}

}  // namespace

FieldType SchemaType(const char* className, const char* field) {
    if (!className || !field) return FieldType::None;
    for (const xom::ClassDef* c = xom::findClass(className); c; c = xom::classParent(*c)) {
        const xom::FieldDef* f = xom::classFields(*c);
        for (uint16_t i = 0; i < c->fieldCount; ++i)
            if (std::strcmp(f[i].name, field) == 0) return FromXom(f[i]);
    }
    return FieldType::None;
}

}  // namespace melange::weapons::fields
