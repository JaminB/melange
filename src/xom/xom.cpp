// wumfix::xom - see xom.h. Mirrors tools/xom/xom.py.
#include "xom.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace wumfix::xom {

#include "xom_schema.inc"

namespace {

constexpr size_t kNumClasses = sizeof(kClasses) / sizeof(kClasses[0]);
constexpr size_t kNumMath = sizeof(kMath) / sizeof(kMath[0]);

size_t elemSize(char e) {
    switch (e) {
        case 'f': return 4;
        case 'h': case 'H': return 2;
        default: return 1;
    }
}

size_t mathSize(uint16_t m) { return elemSize(kMath[m].elem) * kMath[m].count; }

size_t scalarSize(Type t) {
    switch (t) {
        case Type::Bool: case Type::U8: case Type::I8: return 1;
        case Type::U16: case Type::I16: return 2;
        case Type::U32: case Type::I32: case Type::F32: case Type::Enum: case Type::Bitfield32: return 4;
        case Type::U64: case Type::I64: case Type::F64: case Type::Bitfield64: return 8;
        default: return 0;
    }
}

// Size of one element of a fixed-size type, 0 for varint/variable types.
size_t fixedSize(Type t, uint16_t math) { return t == Type::Math ? mathSize(math) : scalarSize(t); }

// ---------------------------------------------------------------- custom classes
// Non-XContainer XomObjects with hand-written serialisers (no CTNR tag); field
// order from the engine read functions (vtable slot 5), base class first.
struct CField {
    const char* name;
    Type type;                       // Struct => array of `elem` records
    char countKind;                  // for Struct arrays: 'v' varint, 'I' u32
    const std::vector<CField>* elem;
};

const std::vector<CField> kGraphEntry = {
    {"Guid", Type::Guid, 0, nullptr}, {"Graph", Type::Ref, 0, nullptr}, {"Name", Type::String, 0, nullptr}};
const std::vector<CField> kTextChar = {
    {"Index", Type::U16, 0, nullptr}, {"MappedVal", Type::U16, 0, nullptr}, {"Unicode", Type::U16, 0, nullptr}};

std::vector<CField> descBase(std::vector<CField> extra) {
    std::vector<CField> v = {{"ResourceId", Type::String, 0, nullptr}, {"SectionId", Type::U16, 0, nullptr}};
    v.insert(v.end(), extra.begin(), extra.end());
    return v;
}

const std::map<std::string, std::vector<CField>, std::less<>>& customClasses() {
    static const std::map<std::string, std::vector<CField>, std::less<>> m = {
        {"XGraphSet", {{"Graphs", Type::Struct, 'v', &kGraphEntry}}},                       // FUN_006df234
        {"XBaseResourceDescriptor", descBase({})},                                          // FUN_006b53e0
        {"XNullDescriptor", descBase({})},
        {"XMeshDescriptor", descBase({{"GraphSet", Type::Ref, 0, nullptr}, {"Flags", Type::U16, 0, nullptr}})},
        {"XBitmapDescriptor", descBase({{"SpriteScene", Type::Ref, 0, nullptr},
                                        {"ImageWidth", Type::U16, 0, nullptr},
                                        {"ImageHeight", Type::U16, 0, nullptr}})},
        {"XSpriteSetDescriptor", descBase({{"SpriteSetGroup", Type::Ref, 0, nullptr}})},
        {"XParticleSetDescriptor", descBase({{"ParticleSetGroup", Type::Ref, 0, nullptr}})},
        {"XCustomDescriptor", descBase({{"Flags", Type::U16, 0, nullptr}})},
        {"XTextDescriptor", descBase({{"TextGroup", Type::Ref, 0, nullptr},
                                      {"Chars", Type::Struct, 'I', &kTextChar}})},
    };
    return m;
}

// XAnimClipLibrary (FUN_007afee0 / FUN_007b2468 for "XCULLED..." names): layout is
// data-dependent, so it has hand-written code (Reader::animlib / Writer::animlib).
const std::vector<CField> kAnimLibMarker = {};
const std::vector<CField> kAnimChannel = {
    {"Byte4", Type::U8, 0, nullptr}, {"Word6", Type::U16, 0, nullptr},
    {"Byte5", Type::U8, 0, nullptr}, {"Name", Type::String, 0, nullptr}};

const std::vector<CField>* findCustom(std::string_view name) {
    if (name == "XAnimClipLibrary") return &kAnimLibMarker;
    auto& m = customClasses();
    auto it = m.find(name);
    return it == m.end() ? nullptr : &it->second;
}

bool isContainerClass(std::string_view name) {
    return !findCustom(name) && findClass(name) != nullptr;
}

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

// ---------------------------------------------------------------- reader

struct Reader {
    const uint8_t* d;
    size_t n;
    size_t o = 0;
    const std::vector<std::string>* strings = nullptr;
    std::string err;

    bool fail(const std::string& e) {
        if (err.empty()) err = e;
        return false;
    }
    bool need(size_t k) { return o + k <= n ? true : fail("short read"); }
    uint64_t le(size_t k) {
        uint64_t v = 0;
        for (size_t i = 0; i < k; ++i) v |= uint64_t(d[o + i]) << (8 * i);
        o += k;
        return v;
    }
    bool varint(uint64_t& v) {
        // 7-bit groups, least significant first, high bit = more (FUN_0063e939)
        v = 0;
        for (unsigned sh = 0;; sh += 7) {
            if (!need(1)) return false;
            uint8_t b = d[o++];
            if (sh < 64) v |= uint64_t(b & 0x7f) << sh;
            if (!(b & 0x80)) return true;
        }
    }

    bool scalar(Type t, uint16_t math, Value& v) {
        v.type = t;
        v.math = math;
        switch (t) {
            case Type::String: {
                uint64_t id;
                if (!varint(id)) return false;
                if (id >= strings->size()) return fail("string id out of range");
                v.str = (*strings)[size_t(id)];
                return true;
            }
            case Type::Ref:
                return varint(v.bits);
            case Type::Guid:
                if (!need(16)) return false;
                std::memcpy(v.guid.data(), d + o, 16);
                o += 16;
                return true;
            case Type::Math: {
                size_t k = mathSize(math);
                if (!need(k)) return false;
                v.raw.assign(d + o, d + o + k);
                o += k;
                return true;
            }
            default: {
                size_t k = scalarSize(t);
                if (!k) return fail("cannot decode field type");
                if (!need(k)) return false;
                v.bits = le(k);
                return true;
            }
        }
    }

    bool field(const FieldDef& f, Value& v) {
        if (!f.isArray()) return scalar(f.type, f.math, v);
        // XMF*Descriptor (FUN_006c5bf4): varint count, then elements
        uint64_t cnt;
        if (!varint(cnt)) return false;
        if (cnt > n - o) return fail("array count too large");
        v.type = f.type;
        v.math = f.math;
        v.array = true;
        if (size_t es = fixedSize(f.type, f.math)) {
            if (!need(size_t(cnt) * es)) return false;
            v.raw.assign(d + o, d + o + size_t(cnt) * es);
            o += size_t(cnt) * es;
            return true;
        }
        v.items.resize(size_t(cnt));
        for (auto& it : v.items)
            if (!scalar(f.type, f.math, it)) return false;
        return true;
    }

    bool u32v(Value& v) {
        v.type = Type::U32;
        if (!need(4)) return false;
        v.bits = le(4);
        return true;
    }
    bool animTrack(bool culled, Value& t) {
        t.type = Type::Struct;
        static const char* flags[4] = {"Flag1", "Flag8", "Flag4", "Flag2"};
        for (auto* fl : flags) {
            Value b;
            if (!scalar(Type::Bool, 0, b)) return false;
            t.members.emplace_back(fl, std::move(b));
        }
        if (culled) {
            Value c;
            if (!scalar(Type::U16, 0, c)) return false;
            t.members.emplace_back("Channel", std::move(c));
        }
        Value a, b, keys;
        if (!u32v(a) || !u32v(b) || !need(4)) return false;
        uint64_t nk = le(4);
        if (nk > (n - o) / 24) return fail("key count too large");
        keys.type = Type::F32;  // packed: 6 floats per key, in file order
        keys.array = true;
        keys.raw.assign(d + o, d + o + size_t(nk) * 24);
        o += size_t(nk) * 24;
        t.members.emplace_back("BitsA", std::move(a));
        t.members.emplace_back("BitsB", std::move(b));
        t.members.emplace_back("Keys", std::move(keys));
        return true;
    }
    bool animlib(std::vector<std::pair<std::string, Value>>& out) {
        Value name;
        if (!scalar(Type::String, 0, name)) return false;
        bool culled = name.str.compare(0, 7, "XCULLED") == 0;
        out.emplace_back("Name", std::move(name));
        std::vector<CField> chan = {{"Channels", Type::Struct, 'I', &kAnimChannel}};
        if (!custom(chan, out)) return false;
        size_t nchan = out.back().second.items.size();
        if (!need(4)) return false;
        uint64_t nclips = le(4);
        if (nclips > n - o) return fail("clip count too large");
        Value clips;
        clips.type = Type::Struct;
        clips.array = true;
        clips.items.resize(size_t(nclips));
        for (auto& c : clips.items) {
            c.type = Type::Struct;
            Value dur, cname, tracks;
            if (!scalar(Type::F32, 0, dur) || !scalar(Type::String, 0, cname)) return false;
            tracks.type = Type::Struct;
            tracks.array = true;
            size_t nt = nchan;
            if (culled) {
                if (!need(4)) return false;
                uint64_t x = le(4);
                if (x > n - o) return fail("track count too large");
                nt = size_t(x);
            }
            tracks.items.resize(nt);
            for (auto& t : tracks.items)
                if (!animTrack(culled, t)) return false;
            c.members.emplace_back("Duration", std::move(dur));
            c.members.emplace_back("Name", std::move(cname));
            c.members.emplace_back("Tracks", std::move(tracks));
        }
        out.emplace_back("Clips", std::move(clips));
        return true;
    }

    bool custom(const std::vector<CField>& defs, std::vector<std::pair<std::string, Value>>& out) {
        if (&defs == &kAnimLibMarker) return animlib(out);
        for (const auto& cf : defs) {
            Value v;
            if (cf.type == Type::Struct) {
                uint64_t cnt;
                if (cf.countKind == 'v') {
                    if (!varint(cnt)) return false;
                } else {
                    if (!need(4)) return false;
                    cnt = le(4);
                }
                if (cnt > n - o) return fail("array count too large");
                v.type = Type::Struct;
                v.array = true;
                v.items.resize(size_t(cnt));
                for (auto& it : v.items) {
                    it.type = Type::Struct;
                    if (!custom(*cf.elem, it.members)) return false;
                }
            } else if (!scalar(cf.type, 0, v)) {
                return false;
            }
            out.emplace_back(cf.name, std::move(v));
        }
        return true;
    }
};

bool fieldPresent(const FieldDef& f, uint32_t version) {
    // FUN_006c72b4
    if (f.flags & 0x04) return false;  // transient
    if (f.flags & 0x20) return f.obsoleteFrom >= 0 && int(version) < f.obsoleteFrom;
    if (f.schemaFrom >= 0) return int(version) >= f.schemaFrom;
    return true;
}

using VersionMap = std::unordered_map<std::string, uint32_t>;

struct FieldRef {
    std::string key;
    const FieldDef* def;
};

bool containerFields(std::string_view cls, const VersionMap& versions, std::vector<FieldRef>& out) {
    // most-derived class first, as the engine's read loop (FUN_006c4199) walks it
    const ClassDef* c = findClass(cls);
    if (!c) return false;
    std::set<std::string> seen;
    for (; c; c = classParent(*c)) {
        auto vi = versions.find(c->name);
        uint32_t ver = vi == versions.end() ? 0 : vi->second;
        const FieldDef* f = classFields(*c);
        for (unsigned i = 0; i < c->fieldCount; ++i) {
            if (!fieldPresent(f[i], ver)) continue;
            std::string key = f[i].name;
            if (seen.count(key)) key = std::string(c->name) + "." + key;
            seen.insert(key);
            out.push_back({key, &f[i]});
        }
    }
    return true;
}

bool decodeObject(Reader& r, const std::string& cls, const VersionMap& versions, Object& obj) {
    obj.type = cls;
    if (auto* cd = findCustom(cls)) {
        obj.container = false;
        return r.custom(*cd, obj.fields);
    }
    if (!r.need(7) || std::memcmp(r.d + r.o, "CTNR", 4) != 0) return r.fail("expected CTNR");
    r.o += 4;
    obj.internalFlags = r.d[r.o];
    obj.userFlags = r.d[r.o + 1];
    obj.dxFieldCount = r.d[r.o + 2];
    r.o += 3;
    if (obj.dxFieldCount) return r.fail("DxFieldCount != 0 not supported");
    std::vector<FieldRef> fr;
    if (!containerFields(cls, versions, fr)) return r.fail("class not in schema: " + cls);
    for (auto& f : fr) {
        Value v;
        if (!r.field(*f.def, v)) return false;
        obj.fields.emplace_back(f.key, std::move(v));
    }
    return true;
}

size_t nextCtnr(const uint8_t* d, size_t n, size_t o) {
    for (size_t i = o; i + 4 <= n; ++i)
        if (d[i] == 'C' && std::memcmp(d + i, "CTNR", 4) == 0) return i;
    return n;
}

// ---------------------------------------------------------------- writer

struct Writer {
    std::vector<uint8_t>& out;
    std::vector<std::string>& strings;
    std::unordered_map<std::string, uint32_t> sid;
    std::string err;

    Writer(std::vector<uint8_t>& o, std::vector<std::string>& s) : out(o), strings(s) {
        for (uint32_t i = 0; i < strings.size(); ++i) sid.emplace(strings[i], i);
    }
    bool fail(const std::string& e) {
        if (err.empty()) err = e;
        return false;
    }
    void le(uint64_t v, size_t k) {
        for (size_t i = 0; i < k; ++i) out.push_back(uint8_t(v >> (8 * i)));
    }
    void varint(uint64_t v) {
        do {
            uint8_t b = v & 0x7f;
            v >>= 7;
            out.push_back(v ? (b | 0x80) : b);
        } while (v);
    }
    uint32_t stringId(const std::string& s) {
        auto it = sid.find(s);
        if (it != sid.end()) return it->second;
        uint32_t id = uint32_t(strings.size());
        strings.push_back(s);
        sid.emplace(s, id);
        return id;
    }
    bool scalar(Type t, uint16_t math, const Value& v) {
        switch (t) {
            case Type::String: varint(stringId(v.str)); return true;
            case Type::Ref: varint(v.bits); return true;
            case Type::Guid: out.insert(out.end(), v.guid.begin(), v.guid.end()); return true;
            case Type::Math:
                if (v.raw.size() != mathSize(math)) return fail("math value has wrong size");
                out.insert(out.end(), v.raw.begin(), v.raw.end());
                return true;
            default: {
                size_t k = scalarSize(t);
                if (!k) return fail("cannot encode field type");
                le(v.bits, k);
                return true;
            }
        }
    }
    bool field(const FieldDef& f, const Value& v) {
        if (!f.isArray()) return scalar(f.type, f.math, v);
        if (size_t es = fixedSize(f.type, f.math)) {
            if (v.raw.size() % es) return fail("packed array has partial element");
            varint(v.raw.size() / es);
            out.insert(out.end(), v.raw.begin(), v.raw.end());
            return true;
        }
        varint(v.items.size());
        for (auto& it : v.items)
            if (!scalar(f.type, f.math, it)) return false;
        return true;
    }
    bool animlib(const std::vector<std::pair<std::string, Value>>& in) {
        if (in.size() != 3) return fail("XAnimClipLibrary: expected Name, Channels, Clips");
        const Value& name = in[0].second;
        bool culled = name.str.compare(0, 7, "XCULLED") == 0;
        scalar(Type::String, 0, name);
        std::vector<CField> chan = {{"Channels", Type::Struct, 'I', &kAnimChannel}};
        std::vector<std::pair<std::string, Value>> ch = {in[1]};
        if (!custom(chan, ch)) return false;
        size_t nchan = in[1].second.items.size();
        const Value& clips = in[2].second;
        le(clips.items.size(), 4);
        for (auto& c : clips.items) {
            if (c.members.size() != 3) return fail("XAnimClipLibrary: bad clip");
            scalar(Type::F32, 0, c.members[0].second);
            scalar(Type::String, 0, c.members[1].second);
            const Value& tracks = c.members[2].second;
            if (culled) le(tracks.items.size(), 4);
            else if (tracks.items.size() != nchan) return fail("XAnimClipLibrary: need one track per channel");
            for (auto& t : tracks.items) {
                if (t.members.size() != (culled ? 8u : 7u)) return fail("XAnimClipLibrary: bad track");
                size_t k = 0;
                for (; k < (culled ? 5u : 4u); ++k)
                    scalar(t.members[k].second.type, 0, t.members[k].second);
                le(t.members[k].second.bits, 4);
                le(t.members[k + 1].second.bits, 4);
                const Value& keys = t.members[k + 2].second;
                if (keys.raw.size() % 24) return fail("XAnimClipLibrary: keys must be 6 floats each");
                le(keys.raw.size() / 24, 4);
                out.insert(out.end(), keys.raw.begin(), keys.raw.end());
            }
        }
        return true;
    }
    bool custom(const std::vector<CField>& defs, const std::vector<std::pair<std::string, Value>>& in) {
        if (&defs == &kAnimLibMarker) return animlib(in);
        if (in.size() != defs.size()) return fail("custom object has wrong member count");
        for (size_t i = 0; i < defs.size(); ++i) {
            const auto& cf = defs[i];
            const Value& v = in[i].second;
            if (cf.type == Type::Struct) {
                if (cf.countKind == 'v') varint(v.items.size());
                else le(v.items.size(), 4);
                for (auto& it : v.items)
                    if (!custom(*cf.elem, it.members)) return false;
            } else if (!scalar(cf.type, 0, v)) {
                return false;
            }
        }
        return true;
    }
};

std::vector<uint8_t> strsBytes(const std::vector<std::string>& strings) {
    // Canonical STRS: blob = "\0" + sorted unique non-empty strings (FUN_0063cc92's tree walk)
    std::vector<std::string> uniq;
    for (auto& s : strings)
        if (!s.empty()) uniq.push_back(s);
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    std::unordered_map<std::string, uint32_t> pos;
    std::vector<uint8_t> blob(1, 0);
    pos[""] = 0;
    for (auto& s : uniq) {
        pos[s] = uint32_t(blob.size());
        blob.insert(blob.end(), s.begin(), s.end());
        blob.push_back(0);
    }
    std::vector<uint8_t> out = {'S', 'T', 'R', 'S'};
    auto put = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
    put(uint32_t(strings.size()));
    put(uint32_t(blob.size()));
    for (auto& s : strings) put(pos[s]);
    out.insert(out.end(), blob.begin(), blob.end());
    return out;
}

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

}  // namespace

// ---------------------------------------------------------------- schema access

const ClassDef* findClass(std::string_view name) {
    for (size_t i = 0; i < kNumClasses; ++i)
        if (name == kClasses[i].name) return &kClasses[i];
    return nullptr;
}
const ClassDef* findClassByGuid(std::string_view g) {
    if (g.empty()) return nullptr;
    for (size_t i = 0; i < kNumClasses; ++i)
        if (g == kClasses[i].guid) return &kClasses[i];
    return nullptr;
}
const MathDef& mathDef(uint16_t i) { return kMath[i < kNumMath ? i : 0]; }
const FieldDef* classFields(const ClassDef& c) { return &kFields[c.firstField]; }
const ClassDef* classParent(const ClassDef& c) { return c.parent < 0 ? nullptr : &kClasses[c.parent]; }

// ---------------------------------------------------------------- Value helpers

int64_t Value::asInt() const {
    size_t k = scalarSize(type);
    if (k == 0 || k == 8) return int64_t(bits);
    uint64_t m = uint64_t(1) << (8 * k - 1);
    return int64_t((bits ^ m) - m);  // sign-extend
}
double Value::asFloat() const {
    if (type == Type::F64) {
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
    uint32_t b = uint32_t(bits);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
void Value::setInt(int64_t v) {
    size_t k = scalarSize(type);
    bits = (k == 0 || k == 8) ? uint64_t(v) : (uint64_t(v) & ((uint64_t(1) << (8 * k)) - 1));
}
void Value::setFloat(double v) {
    if (type == Type::F64) {
        std::memcpy(&bits, &v, 8);
    } else {
        float f = float(v);
        uint32_t b;
        std::memcpy(&b, &f, 4);
        bits = b;
    }
}
std::vector<double> Value::components() const {
    std::vector<double> r;
    if (type != Type::Math) return r;
    const MathDef& m = mathDef(math);
    size_t es = elemSize(m.elem);
    for (size_t i = 0; i + es <= raw.size(); i += es) {
        const uint8_t* p = raw.data() + i;
        switch (m.elem) {
            case 'f': { uint32_t b = rd32(p); float f; std::memcpy(&f, &b, 4); r.push_back(f); break; }
            case 'h': r.push_back(int16_t(p[0] | (p[1] << 8))); break;
            case 'H': r.push_back(uint16_t(p[0] | (p[1] << 8))); break;
            case 'b': r.push_back(int8_t(p[0])); break;
            default: r.push_back(p[0]); break;
        }
    }
    return r;
}
void Value::setComponents(const std::vector<double>& v) {
    const MathDef& m = mathDef(math);
    raw.clear();
    for (size_t i = 0; i < m.count; ++i) {
        double x = i < v.size() ? v[i] : 0.0;
        switch (m.elem) {
            case 'f': { float f = float(x); uint32_t b; std::memcpy(&b, &f, 4);
                        for (int k = 0; k < 4; ++k) raw.push_back(uint8_t(b >> (8 * k))); break; }
            case 'h': case 'H': { uint16_t s = uint16_t(int32_t(x)); raw.push_back(uint8_t(s)); raw.push_back(uint8_t(s >> 8)); break; }
            default: raw.push_back(uint8_t(int32_t(x))); break;
        }
    }
}
const Value* Value::member(std::string_view name) const {
    for (auto& m : members)
        if (m.first == name) return &m.second;
    return nullptr;
}

bool Value::packed() const { return array && fixedSize(type, math) != 0; }
size_t Value::size() const {
    if (!array) return 1;
    size_t es = fixedSize(type, math);
    return es ? raw.size() / es : items.size();
}
Value Value::at(size_t i) const {
    if (!array) return *this;
    if (!packed()) return items.at(i);
    size_t es = fixedSize(type, math);
    Value v;
    v.type = type;
    v.math = math;
    const uint8_t* p = raw.data() + i * es;
    if (type == Type::Math) {
        v.raw.assign(p, p + es);
    } else {
        for (size_t k = 0; k < es; ++k) v.bits |= uint64_t(p[k]) << (8 * k);
    }
    return v;
}
void Value::set(size_t i, const Value& v) {
    if (!array) {
        *this = v;
        return;
    }
    if (!packed()) {
        items.at(i) = v;
        return;
    }
    size_t es = fixedSize(type, math);
    uint8_t* p = raw.data() + i * es;
    if (type == Type::Math) {
        if (v.raw.size() == es) std::memcpy(p, v.raw.data(), es);
    } else {
        for (size_t k = 0; k < es; ++k) p[k] = uint8_t(v.bits >> (8 * k));
    }
}
void Value::resize(size_t n) {
    if (packed()) raw.resize(n * fixedSize(type, math));
    else items.resize(n);
}

Value* Object::field(std::string_view name) {
    for (auto& f : fields)
        if (f.first == name) return &f.second;
    return nullptr;
}
const Value* Object::field(std::string_view name) const {
    for (auto& f : fields)
        if (f.first == name) return &f.second;
    return nullptr;
}

// ---------------------------------------------------------------- parse

bool parse(const uint8_t* d, size_t n, Document& doc, std::string* error, const ParseOptions& opt) {
    auto bad = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    doc = Document{};
    if (n < 0x40 || std::memcmp(d, "MOIK", 4) != 0) return bad("bad magic");
    std::memcpy(doc.version.data(), d + 4, 4);
    std::memcpy(doc.reserved08.data(), d + 8, 16);
    std::memcpy(doc.reserved24.data(), d + 0x24, 28);
    uint32_t ntypes = rd32(d + 0x18), nobjects = rd32(d + 0x1c);
    doc.root = rd32(d + 0x20);
    size_t o = 0x40;
    if (ntypes > (n - o) / 0x40) return bad("TYPE table past end");
    for (uint32_t i = 0; i < ntypes; ++i, o += 0x40) {
        const uint8_t* p = d + o;
        if (std::memcmp(p, "TYPE", 4) != 0) return bad("expected TYPE");
        TypeEntry t;
        t.version = rd32(p + 4);
        t.count = rd32(p + 8);
        t.c = rd32(p + 12);
        std::memcpy(t.guid.data(), p + 0x10, 16);
        std::memcpy(t.rawName.data(), p + 0x20, 32);
        t.name.assign(reinterpret_cast<const char*>(p + 0x20), strnlen(reinterpret_cast<const char*>(p + 0x20), 32));
        // TYPE names are truncated to 31 chars (FUN_0063c49f): resolve by GUID first
        if (auto* c = findClassByGuid(hex(t.guid.data(), 16))) {
            if (t.name != c->name) t.cls = c->name;
        }
        doc.types.push_back(std::move(t));
    }
    for (int k = 0; k < 2; ++k, o += 16) {
        if (o + 16 > n || std::memcmp(d + o, k ? "SCHM" : "GUID", 4) != 0) return bad("expected GUID/SCHM");
        auto& rec = k ? doc.schmRec : doc.guidRec;
        for (int j = 0; j < 3; ++j) rec[j] = rd32(d + o + 4 + 4 * j);
    }
    if (o + 12 > n || std::memcmp(d + o, "STRS", 4) != 0) return bad("expected STRS");
    size_t s0 = o;
    uint32_t scount = rd32(d + o + 4), bsize = rd32(d + o + 8);
    if (scount > (n - o - 12) / 4 || o + 12 + size_t(scount) * 4 + bsize > n) return bad("STRS past end");
    size_t b0 = o + 12 + size_t(scount) * 4;
    for (uint32_t i = 0; i < scount; ++i) {
        uint32_t off = rd32(d + o + 12 + 4 * i);
        if (off >= bsize) return bad("string offset out of range");
        const char* s = reinterpret_cast<const char*>(d + b0 + off);
        doc.strings.emplace_back(s, strnlen(s, bsize - off));
    }
    o = b0 + bsize;
    std::vector<uint8_t> canon = strsBytes(doc.strings);
    if (canon.size() != o - s0 || std::memcmp(canon.data(), d + s0, canon.size()) != 0)
        doc.strsRaw.assign(d + s0, d + o);

    std::vector<std::string> seq;
    VersionMap versions;
    for (auto& t : doc.types) {
        versions[t.className()] = t.version;
        for (uint32_t i = 0; i < t.count; ++i) {
            if (seq.size() >= nobjects) return bad("TYPE counts exceed nobjects");
            seq.push_back(t.className());
        }
    }
    if (seq.size() != nobjects) return bad("TYPE counts != nobjects");

    Reader r{d, n};
    r.strings = &doc.strings;
    for (size_t i = 0; i < seq.size(); ++i) {
        bool last = i + 1 == seq.size();
        bool nextCtnr_ = !last && isContainerClass(seq[i + 1]);
        Object obj;
        r.o = o;
        r.err.clear();
        bool ok = decodeObject(r, seq[i], versions, obj);
        if (ok) {
            if (last && r.o != n) ok = r.fail("trailing bytes after last object");
            else if (nextCtnr_ && (r.o + 4 > n || std::memcmp(d + r.o, "CTNR", 4) != 0))
                ok = r.fail("decoded size mismatch");
        }
        if (ok) {
            doc.objects.push_back(std::move(obj));
            o = r.o;
            continue;
        }
        if (opt.strict) return bad("object #" + std::to_string(i + 1) + " " + seq[i] + ": " + r.err);
        if (o + 4 <= n && std::memcmp(d + o, "CTNR", 4) == 0 && (nextCtnr_ || last)) {
            size_t nx = last ? n : nextCtnr(d, n, o + 4);
            Object op;
            op.type = seq[i];
            op.opaque = true;
            op.error = r.err;
            op.raw.assign(d + o + 4, d + nx);
            doc.objects.push_back(std::move(op));
            o = nx;
        } else {
            doc.tailRaw.assign(d + o, d + n);
            for (size_t j = i; j < seq.size(); ++j) {
                Object t;
                t.type = seq[j];
                t.inTail = true;
                t.error = j == i ? r.err : std::string();
                doc.objects.push_back(std::move(t));
            }
            return true;
        }
    }
    if (o != n) doc.trailer.assign(d + o, d + n);
    return true;
}

// ---------------------------------------------------------------- serialize

bool serialize(const Document& doc, std::vector<uint8_t>& out, std::string* error) {
    auto bad = [&](const std::string& e) {
        if (error) *error = e;
        return false;
    };
    std::vector<std::string> strings = doc.strings;
    std::vector<uint8_t> body;
    Writer w(body, strings);
    VersionMap versions;
    std::vector<std::string> order;
    for (auto& t : doc.types) {
        versions[t.className()] = t.version;
        order.push_back(t.className());
    }
    std::unordered_map<std::string, uint32_t> counts;
    size_t pos = 0;
    for (auto& ob : doc.objects) {
        auto it = std::find(order.begin(), order.end(), ob.type);
        if (it == order.end()) return bad("object type missing from TYPE table: " + ob.type);
        size_t p = size_t(it - order.begin());
        if (p < pos) return bad("objects are not grouped in TYPE-table order: " + ob.type);
        pos = p;
        counts[ob.type]++;
        if (ob.inTail) continue;
        if (ob.opaque) {
            body.insert(body.end(), {'C', 'T', 'N', 'R'});
            body.insert(body.end(), ob.raw.begin(), ob.raw.end());
            continue;
        }
        if (auto* cd = findCustom(ob.type)) {
            if (!w.custom(*cd, ob.fields)) return bad(ob.type + ": " + w.err);
            continue;
        }
        body.insert(body.end(), {'C', 'T', 'N', 'R', ob.internalFlags, ob.userFlags, ob.dxFieldCount});
        std::vector<FieldRef> fr;
        if (!containerFields(ob.type, versions, fr)) return bad("class not in schema: " + ob.type);
        if (fr.size() != ob.fields.size()) return bad(ob.type + ": field count does not match schema");
        for (size_t i = 0; i < fr.size(); ++i) {
            if (ob.fields[i].first != fr[i].key) return bad(ob.type + ": field order/name mismatch at " + fr[i].key);
            if (!w.field(*fr[i].def, ob.fields[i].second)) return bad(ob.type + "." + fr[i].key + ": " + w.err);
        }
    }

    out.clear();
    auto put = [&](uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); };
    out.insert(out.end(), {'M', 'O', 'I', 'K'});
    out.insert(out.end(), doc.version.begin(), doc.version.end());
    out.insert(out.end(), doc.reserved08.begin(), doc.reserved08.end());
    put(uint32_t(doc.types.size()));
    put(uint32_t(doc.objects.size()));
    put(doc.root);
    out.insert(out.end(), doc.reserved24.begin(), doc.reserved24.end());
    for (auto& t : doc.types) {
        out.insert(out.end(), {'T', 'Y', 'P', 'E'});
        put(t.version);
        put(counts[t.className()]);
        put(t.c);
        out.insert(out.end(), t.guid.begin(), t.guid.end());
        out.insert(out.end(), t.rawName.begin(), t.rawName.end());
    }
    out.insert(out.end(), {'G', 'U', 'I', 'D'});
    for (auto v : doc.guidRec) put(v);
    out.insert(out.end(), {'S', 'C', 'H', 'M'});
    for (auto v : doc.schmRec) put(v);
    if (!doc.strsRaw.empty() && strings.size() == doc.strings.size()) {
        out.insert(out.end(), doc.strsRaw.begin(), doc.strsRaw.end());
    } else {
        auto s = strsBytes(strings);
        out.insert(out.end(), s.begin(), s.end());
    }
    out.insert(out.end(), body.begin(), body.end());
    out.insert(out.end(), doc.tailRaw.begin(), doc.tailRaw.end());
    out.insert(out.end(), doc.trailer.begin(), doc.trailer.end());
    return true;
}

}  // namespace wumfix::xom
