#include "weapons/fields.h"

#include <cstring>
#include <map>
#include <string_view>

#include "core/log.h"
#include "core/mem.h"
#include "weapons/engine.h"

namespace melange::weapons::fields {
namespace {
struct Verified {
    const char* name;
    uint32_t offset;
    FieldType type;
};
constexpr Verified kVerified[] = {
    {"WormDamageMagnitude", 0x15c, FieldType::F32}, {"WormDamageRadius", 0x164, FieldType::F32},
    {"LandDamageRadius", 0x168, FieldType::F32},    {"ImpulseRadius", 0x16c, FieldType::F32},
    {"ImpulseMagnitude", 0xc0, FieldType::F32},     {"PayloadGraphicsResourceID", 0xd4, FieldType::String},
    {"DisplayName", 0x2c, FieldType::String},
};
constexpr size_t kMaxFields = 512;

std::map<uintptr_t, std::vector<Entry>> g_cache;
int g_check = -1;

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

bool Walk(uintptr_t cls, std::vector<Entry>* out) {
    const std::string cname = engine::ClassName(cls);
    int depth = 0;
    for (uintptr_t c = cls; c && depth < 16; c = engine::ClassParent(c), ++depth) {
        const uintptr_t b = Rd<uintptr_t>(c + 0x44), e = Rd<uintptr_t>(c + 0x48);
        if (e < b || (e - b) / 4 > kMaxFields) return false;
        if (!b) continue;
        for (uintptr_t p = b; p < e; p += 4) {
            const uintptr_t prop = Rd<uintptr_t>(p);
            const uintptr_t d = prop ? Rd<uintptr_t>(prop + 4) : 0;
            if (!d) continue;
            Entry en;
            en.name = engine::ReadCString(Rd<uintptr_t>(d), 64);
            en.offset = Rd<uint16_t>(d + 4);
            en.flags = Rd<uint8_t>(d + 7);
            if (en.name.empty()) continue;
            bool dup = false;
            for (auto& x : *out) dup |= x.name == en.name;
            if (dup) continue;
            en.type = SchemaType(cname.c_str(), en.name.c_str());
            out->push_back(std::move(en));
        }
        if (out->size() > kMaxFields) return false;
    }
    return !out->empty();
}

const Verified* FindVerified(const char* field) {
    for (auto& v : kVerified)
        if (std::strcmp(v.name, field) == 0) return &v;
    return nullptr;
}
}  // namespace

const std::vector<Entry>* ForClass(uintptr_t cls) {
    if (!cls) return nullptr;
    auto it = g_cache.find(cls);
    if (it != g_cache.end()) return it->second.empty() ? nullptr : &it->second;
    std::vector<Entry> v;
    if (!Walk(cls, &v)) {
        LOG_ERROR("[weapons] the field list of class %08x ('%s') could not be read", static_cast<unsigned>(cls),
                  engine::ClassName(cls).c_str());
        v.clear();
    }
    auto& slot = g_cache[cls] = std::move(v);
    return slot.empty() ? nullptr : &slot;
}

bool SelfCheck() {
    if (g_check >= 0) return g_check == 1;
    const auto* v = ForClass(kPayloadClass);
    int bad = 0;
    for (auto& k : kVerified) {
        const Entry* e = nullptr;
        if (v)
            for (auto& x : *v)
                if (x.name == k.name) e = &x;
        if (!e || e->offset != k.offset || e->type != k.type) {
            ++bad;
            LOG_ERROR("[weapons] field self-check: %s is %s%x, expected +%x", k.name, e ? "+" : "missing ",
                      e ? e->offset : 0u, k.offset);
        }
    }
    g_check = bad == 0 ? 1 : 0;
    LOG_INFO("[weapons] field self-check on '%s': %zu fields, %s", engine::ClassName(kPayloadClass).c_str(),
             v ? v->size() : 0u, g_check ? "the seven verified offsets agree" : "disagreement: serving the seven only");
    return g_check == 1;
}

bool Trusted() { return SelfCheck(); }

FieldType Find(uintptr_t cls, const char* field, uint32_t* offset) {
    if (!cls || !field) return FieldType::None;
    if (!Trusted()) {
        const Verified* v = cls == kPayloadClass ? FindVerified(field) : nullptr;
        if (!v) return FieldType::None;
        if (offset) *offset = v->offset;
        return v->type;
    }
    const auto* list = ForClass(cls);
    if (!list) return FieldType::None;
    for (auto& e : *list)
        if (e.name == field) {
            if (e.type == FieldType::None || (e.flags & 0x01)) return FieldType::None;
            if (offset) *offset = e.offset;
            return e.type;
        }
    return FieldType::None;
}
}  // namespace melange::weapons::fields
