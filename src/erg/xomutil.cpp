#include "erg/xomutil.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <set>

namespace melange::erg::xomutil {
namespace {
using xom::Type;
using xom::Value;

size_t ElemSize(char e) { return e == 'f' ? 4 : (e == 'h' || e == 'H') ? 2 : 1; }

bool Present(const xom::FieldDef& f, uint32_t version) {
    if (f.flags & 0x04) return false;
    if (f.flags & 0x20) return f.obsoleteFrom >= 0 && static_cast<int>(version) < f.obsoleteFrom;
    if (f.schemaFrom >= 0) return static_cast<int>(version) >= f.schemaFrom;
    return true;
}

uint32_t VersionOf(const xom::Document& doc, std::string_view cls) {
    for (const auto& t : doc.types)
        if (t.className() == cls) return t.version;
    return 0;
}

Value Zero(const xom::FieldDef& f) {
    Value v;
    v.type = f.type;
    v.math = f.math;
    v.array = f.isArray();
    if (!v.array && f.type == Type::Math) {
        const xom::MathDef& m = xom::mathDef(f.math);
        v.raw.assign(ElemSize(m.elem) * m.count, 0);
    }
    return v;
}

void Walk(Value& v, const std::function<void(Value&)>& fn) {
    if (v.type == Type::Ref) {
        if (v.array)
            for (auto& it : v.items) fn(it);
        else
            fn(v);
        return;
    }
    for (auto& it : v.items) Walk(it, fn);
    for (auto& m : v.members) Walk(m.second, fn);
}

bool Remap(xom::Document& doc, const std::function<uint32_t(uint32_t)>& map) {
    for (const auto& o : doc.objects)
        if (o.opaque || o.inTail) return false;
    for (auto& o : doc.objects)
        for (auto& f : o.fields) Walk(f.second, [&](Value& r) { r.bits = r.bits ? map(static_cast<uint32_t>(r.bits)) : 0; });
    doc.root = doc.root ? map(doc.root) : 0;
    return true;
}
}  // namespace

bool NewObject(const xom::Document& doc, std::string_view cls, xom::Object* out, std::string* err) {
    const xom::ClassDef* c = xom::findClass(cls);
    if (!c) {
        if (err) *err = "class not in the schema: " + std::string(cls);
        return false;
    }
    xom::Object o;
    o.type = std::string(cls);
    std::set<std::string> seen;
    for (; c; c = xom::classParent(*c)) {
        const uint32_t ver = VersionOf(doc, c->name);
        const xom::FieldDef* f = xom::classFields(*c);
        for (unsigned i = 0; i < c->fieldCount; ++i) {
            if (!Present(f[i], ver)) continue;
            std::string key = f[i].name;
            if (seen.count(key)) key = std::string(c->name) + "." + key;
            seen.insert(key);
            o.fields.emplace_back(key, Zero(f[i]));
        }
    }
    *out = std::move(o);
    return true;
}

bool EnsureType(xom::Document& doc, std::string_view cls, std::string_view before) {
    for (const auto& t : doc.types)
        if (t.className() == cls) return true;
    const xom::ClassDef* c = xom::findClass(cls);
    if (!c || std::strlen(c->guid) != 32 || cls.size() > 31) return false;
    xom::TypeEntry t;
    t.name = std::string(cls);
    for (size_t i = 0; i < 16; ++i) {
        auto nib = [](char h) { return static_cast<uint8_t>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10); };
        t.guid[i] = static_cast<uint8_t>(nib(c->guid[2 * i]) << 4 | nib(c->guid[2 * i + 1]));
    }
    std::memcpy(t.rawName.data(), cls.data(), cls.size());
    auto it = std::find_if(doc.types.begin(), doc.types.end(), [&](const xom::TypeEntry& e) { return e.className() == before; });
    doc.types.insert(it, std::move(t));
    return true;
}

bool InsertObject(xom::Document& doc, uint32_t at, xom::Object obj) {
    if (at < 1 || at > doc.objects.size() + 1) return false;
    if (!Remap(doc, [at](uint32_t r) { return r >= at ? r + 1 : r; })) return false;
    doc.objects.insert(doc.objects.begin() + (at - 1), std::move(obj));
    return true;
}

bool RemoveObject(xom::Document& doc, uint32_t at) {
    if (at < 1 || at > doc.objects.size() || at == doc.root) return false;
    if (!Remap(doc, [at](uint32_t r) { return r == at ? 0u : r > at ? r - 1 : r; })) return false;
    doc.objects.erase(doc.objects.begin() + (at - 1));
    return true;
}

Value RefValue(uint32_t ref) {
    Value v;
    v.type = Type::Ref;
    v.bits = ref;
    return v;
}

std::string Str(const xom::Object& o, std::string_view field) {
    const Value* v = o.field(field);
    return v && v->type == Type::String && !v->array ? v->str : std::string();
}

bool SetStr(xom::Object& o, std::string_view field, std::string_view value) {
    Value* v = o.field(field);
    if (!v || v->type != Type::String || v->array) return false;
    v->str = std::string(value);
    return true;
}

bool GetVec(const xom::Object& o, std::string_view field, Vec3* out) {
    const Value* v = o.field(field);
    if (!v || v->type != Type::Math || v->array) return false;
    const std::vector<double> c = v->components();
    if (c.size() != 3) return false;
    *out = {c[0], c[1], c[2]};
    return true;
}

bool SetVec(xom::Object& o, std::string_view field, const Vec3& in) {
    Value* v = o.field(field);
    if (!v || v->type != Type::Math || v->array || xom::mathDef(v->math).count != 3) return false;
    v->setComponents({in[0], in[1], in[2]});
    return true;
}

int64_t Int(const xom::Object& o, std::string_view field, int64_t def) {
    const Value* v = o.field(field);
    if (!v || v->array) return def;
    switch (v->type) {
        case Type::Bool: case Type::U8: case Type::I8: case Type::U16: case Type::I16: case Type::U32: case Type::I32:
        case Type::U64: case Type::I64: case Type::Enum: case Type::Bitfield32: case Type::Bitfield64: return v->asInt();
        default: return def;
    }
}
}  // namespace melange::erg::xomutil
