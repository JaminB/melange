// melange::xom::json - see json.h. Mirrors tools/xom/xom.py's loads()/dumps()/to_json()
// closely enough that DocumentToJson() output is byte-identical to Python's to_json(load(f)).
#include "json.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace melange::xom {
namespace {

// ---------------------------------------------------------------- hex helpers

std::string HexLower(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}
std::string HexLowerSpaced(const uint8_t* p, size_t n) {
    // matches Python bytes.hex(' '): one space between successive byte pairs, none at the ends
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        if (i) s.push_back(' ');
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}
int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool HexDecode(std::string_view s, std::vector<uint8_t>& out) {
    // accepts both contiguous and single-space-separated hex (superset of what we ever write)
    out.clear();
    int hi = -1;
    for (char c : s) {
        if (c == ' ') continue;
        int v = HexVal(c);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
        } else {
            out.push_back(uint8_t((hi << 4) | v));
            hi = -1;
        }
    }
    return hi < 0;
}
bool HexDecodeFixed(std::string_view s, uint8_t* out, size_t n) {
    std::vector<uint8_t> b;
    if (!HexDecode(s, b) || b.size() != n) return false;
    std::memcpy(out, b.data(), n);
    return true;
}

// ---------------------------------------------------------------- Latin-1 <-> UTF-8
// xom strings are raw bytes decoded as Latin-1 by the Python reference (see xom.py's
// `.decode('latin1')` / `.encode('latin1')`); JSON text needs real Unicode text, so a
// byte b >= 0x80 becomes the 2-byte UTF-8 encoding of code point U+00b, and back again.

std::string LatinToUtf8(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char b : raw) {
        if (b < 0x80) {
            out.push_back(char(b));
        } else {
            out.push_back(char(0xC0 | (b >> 6)));
            out.push_back(char(0x80 | (b & 0x3F)));
        }
    }
    return out;
}
// Decodes UTF-8 text (as produced by ParseJson) back to raw Latin-1 bytes. false if any
// code point exceeds U+00FF (not representable in the XOM string table, exactly as
// Python's `str.encode('latin1')` would raise).
bool Utf8ToLatin1(const std::string& s, std::string& out) {
    out.clear();
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c0 = s[i];
        uint32_t cp;
        size_t len;
        if (c0 < 0x80) { cp = c0; len = 1; }
        else if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; len = 2; }
        else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; len = 3; }
        else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; len = 4; }
        else return false;
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            unsigned char c = s[i + k];
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (cp > 0xFF) return false;
        out.push_back(char(cp));
        i += len;
    }
    return true;
}

// ---------------------------------------------------------------- Python float repr
// Reproduces CPython's repr(float) exactly: the shortest decimal string that reads back
// to the same IEEE double, formatted fixed or scientific by the same thresholds
// (Python/pystrtod.c format_float_short, mode 'r'). std::to_chars gives the shortest
// correctly-rounded digit string; this just re-renders it with Python's own rules.

std::string PyFloatRepr(double v) {
    if (v == 0.0) return std::signbit(v) ? "-0.0" : "0.0";
    bool neg = v < 0;
    double av = neg ? -v : v;
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof(buf), av, std::chars_format::scientific);
    std::string s(buf, r.ptr);
    size_t epos = s.find('e');
    std::string mantissa = s.substr(0, epos);
    std::string expPart = s.substr(epos + 1);
    int exp = std::atoi(expPart.c_str());
    std::string digits;
    for (char c : mantissa)
        if (c != '.') digits.push_back(c);
    int decpt = exp + 1;  // value == 0.digits * 10^decpt
    int L = int(digits.size());
    bool useExp = (decpt <= -4) || (decpt > 16);
    std::string body;
    if (!useExp) {
        if (decpt <= 0) {
            body = "0." + std::string(size_t(-decpt), '0') + digits;
        } else if (decpt >= L) {
            body = digits + std::string(size_t(decpt - L), '0') + ".0";
        } else {
            body = digits.substr(0, size_t(decpt)) + "." + digits.substr(size_t(decpt));
        }
    } else {
        std::string mant = L == 1 ? digits : digits.substr(0, 1) + "." + digits.substr(1);
        int e = decpt - 1;
        int ae = e < 0 ? -e : e;
        std::string es = std::to_string(ae);
        if (es.size() < 2) es = "0" + es;
        body = mant + "e" + (e < 0 ? "-" : "+") + es;
    }
    return neg ? "-" + body : body;
}

// ---------------------------------------------------------------- JSON string escaping

void WriteJsonString(const std::string& utf8, std::string& out) {
    out.push_back('"');
    for (unsigned char c : utf8) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    static const char* hexd = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hexd[c >> 4]);
                    out.push_back(hexd[c & 15]);
                } else {
                    out.push_back(char(c));  // ASCII printable, or a UTF-8 continuation/lead byte
                }
        }
    }
    out.push_back('"');
}

void WriteJsonValue(const Json& v, int depth, std::string& out);

void WriteJsonIndent(int depth, std::string& out) { out.append(size_t(depth), ' '); }

void WriteJsonArray(const std::vector<Json>& a, int depth, std::string& out) {
    if (a.empty()) { out += "[]"; return; }
    out += "[\n";
    for (size_t i = 0; i < a.size(); ++i) {
        WriteJsonIndent(depth + 1, out);
        WriteJsonValue(a[i], depth + 1, out);
        out += (i + 1 < a.size()) ? ",\n" : "\n";
    }
    WriteJsonIndent(depth, out);
    out += "]";
}
void WriteJsonObject(const std::vector<std::pair<std::string, Json>>& o, int depth, std::string& out) {
    if (o.empty()) { out += "{}"; return; }
    out += "{\n";
    for (size_t i = 0; i < o.size(); ++i) {
        WriteJsonIndent(depth + 1, out);
        WriteJsonString(o[i].first, out);
        out += ": ";
        WriteJsonValue(o[i].second, depth + 1, out);
        out += (i + 1 < o.size()) ? ",\n" : "\n";
    }
    WriteJsonIndent(depth, out);
    out += "}";
}
void WriteJsonValue(const Json& v, int depth, std::string& out) {
    switch (v.kind) {
        case Json::Kind::Null: out += "null"; break;
        case Json::Kind::Bool: out += v.boolean ? "true" : "false"; break;
        case Json::Kind::Number: out += v.numLiteral; break;
        case Json::Kind::String: WriteJsonString(v.str, out); break;
        case Json::Kind::Array: WriteJsonArray(v.arr, depth, out); break;
        case Json::Kind::Object: WriteJsonObject(v.obj, depth, out); break;
    }
}

// ---------------------------------------------------------------- JSON parser

struct JsonParser {
    std::string_view s;
    size_t o = 0;
    std::string err;
    int depth = 0;
    static constexpr int kMaxDepth = 512;  // deep enough for any real document, shallow enough to never overflow the stack

    bool fail(const std::string& e) {
        if (err.empty()) err = e + " at byte " + std::to_string(o);
        return false;
    }
    void skipWs() {
        while (o < s.size() && (s[o] == ' ' || s[o] == '\t' || s[o] == '\n' || s[o] == '\r')) ++o;
    }
    bool lit(std::string_view text) {
        if (s.compare(o, text.size(), text) != 0) return false;
        o += text.size();
        return true;
    }
    bool parseValue(Json& out) {
        skipWs();
        if (o >= s.size()) return fail("unexpected end of input");
        char c = s[o];
        if (c == '{' || c == '[') {
            if (depth >= kMaxDepth) return fail("nesting too deep");
            ++depth;
            bool ok = c == '{' ? parseObject(out) : parseArray(out);
            --depth;
            return ok;
        }
        if (c == '"') return parseString(out);
        if (c == 't') { if (!lit("true")) return fail("bad literal"); out = Json::Bool(true); return true; }
        if (c == 'f') { if (!lit("false")) return fail("bad literal"); out = Json::Bool(false); return true; }
        if (c == 'n') { if (!lit("null")) return fail("bad literal"); out = Json::Null_(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber(out);
        return fail("unexpected character");
    }
    bool parseObject(Json& out) {
        out = Json::Obj();
        ++o;  // '{'
        skipWs();
        if (o < s.size() && s[o] == '}') { ++o; return true; }
        for (;;) {
            skipWs();
            if (o >= s.size() || s[o] != '"') return fail("expected string key");
            Json key;
            if (!parseString(key)) return false;
            skipWs();
            if (o >= s.size() || s[o] != ':') return fail("expected ':'");
            ++o;
            Json val;
            if (!parseValue(val)) return false;
            out.obj.emplace_back(key.str, std::move(val));
            skipWs();
            if (o < s.size() && s[o] == ',') { ++o; continue; }
            if (o < s.size() && s[o] == '}') { ++o; return true; }
            return fail("expected ',' or '}'");
        }
    }
    bool parseArray(Json& out) {
        out = Json::Arr();
        ++o;  // '['
        skipWs();
        if (o < s.size() && s[o] == ']') { ++o; return true; }
        for (;;) {
            Json val;
            if (!parseValue(val)) return false;
            out.arr.push_back(std::move(val));
            skipWs();
            if (o < s.size() && s[o] == ',') { ++o; continue; }
            if (o < s.size() && s[o] == ']') { ++o; return true; }
            return fail("expected ',' or ']'");
        }
    }
    bool utf8Encode(uint32_t cp, std::string& out) {
        if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xC0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xE0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(char(0x80 | (cp & 0x3F)));
        }
        return true;
    }
    bool parseString(Json& out) {
        out = Json::Str("");
        ++o;  // opening quote
        std::string& r = out.str;
        for (;;) {
            if (o >= s.size()) return fail("unterminated string");
            unsigned char c = s[o];
            if (c == '"') { ++o; return true; }
            if (c == '\\') {
                ++o;
                if (o >= s.size()) return fail("unterminated escape");
                char e = s[o++];
                switch (e) {
                    case '"': r.push_back('"'); break;
                    case '\\': r.push_back('\\'); break;
                    case '/': r.push_back('/'); break;
                    case 'b': r.push_back('\b'); break;
                    case 'f': r.push_back('\f'); break;
                    case 'n': r.push_back('\n'); break;
                    case 'r': r.push_back('\r'); break;
                    case 't': r.push_back('\t'); break;
                    case 'u': {
                        if (o + 4 > s.size()) return fail("bad \\u escape");
                        uint32_t cp = 0;
                        for (int k = 0; k < 4; ++k) {
                            int v = HexVal(s[o + k]);
                            if (v < 0) return fail("bad \\u escape");
                            cp = (cp << 4) | uint32_t(v);
                        }
                        o += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && o + 6 <= s.size() && s[o] == '\\' && s[o + 1] == 'u') {
                            uint32_t lo = 0;
                            bool ok = true;
                            for (int k = 0; k < 4; ++k) {
                                int v = HexVal(s[o + 2 + k]);
                                if (v < 0) { ok = false; break; }
                                lo = (lo << 4) | uint32_t(v);
                            }
                            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                o += 6;
                            }
                        }
                        utf8Encode(cp, r);
                        break;
                    }
                    default: return fail("bad escape");
                }
            } else {
                r.push_back(char(c));
                ++o;
            }
        }
    }
    bool parseNumber(Json& out) {
        size_t start = o;
        if (o < s.size() && s[o] == '-') ++o;
        if (o >= s.size() || !std::isdigit(uint8_t(s[o]))) return fail("bad number");
        if (s[o] == '0') { ++o; } else { while (o < s.size() && std::isdigit(uint8_t(s[o]))) ++o; }
        if (o < s.size() && s[o] == '.') {
            ++o;
            if (o >= s.size() || !std::isdigit(uint8_t(s[o]))) return fail("bad number");
            while (o < s.size() && std::isdigit(uint8_t(s[o]))) ++o;
        }
        if (o < s.size() && (s[o] == 'e' || s[o] == 'E')) {
            ++o;
            if (o < s.size() && (s[o] == '+' || s[o] == '-')) ++o;
            if (o >= s.size() || !std::isdigit(uint8_t(s[o]))) return fail("bad number");
            while (o < s.size() && std::isdigit(uint8_t(s[o]))) ++o;
        }
        out = Json{};
        out.kind = Json::Kind::Number;
        out.numLiteral.assign(s.data() + start, o - start);
        return true;
    }
};

}  // namespace

// ---------------------------------------------------------------- Json

Json Json::Int(int64_t v) { Json j; j.kind = Kind::Number; j.numLiteral = std::to_string(v); return j; }
Json Json::UInt(uint64_t v) { Json j; j.kind = Kind::Number; j.numLiteral = std::to_string(v); return j; }
Json Json::Num(double v) {
    Json j;
    j.kind = Kind::Number;
    j.numLiteral = std::isfinite(v) ? PyFloatRepr(v) : "0";  // callers must handle non-finite themselves
    return j;
}

const Json* Json::find(std::string_view key) const {
    for (auto& [k, v] : obj)
        if (k == key) return &v;
    return nullptr;
}
void Json::set(std::string key, Json v) {
    for (auto& [k, existing] : obj)
        if (k == key) { existing = std::move(v); return; }
    obj.emplace_back(std::move(key), std::move(v));
}
int64_t Json::asInt64(bool* ok) const {
    if (ok) *ok = true;
    char* end = nullptr;
    long long v = std::strtoll(numLiteral.c_str(), &end, 10);
    if (end == numLiteral.c_str() || *end) { if (ok) *ok = false; return 0; }
    return v;
}
uint64_t Json::asUInt64(bool* ok) const {
    if (ok) *ok = true;
    if (!numLiteral.empty() && numLiteral[0] == '-') { if (ok) *ok = false; return 0; }
    char* end = nullptr;
    unsigned long long v = std::strtoull(numLiteral.c_str(), &end, 10);
    if (end == numLiteral.c_str() || *end) { if (ok) *ok = false; return 0; }
    return v;
}
double Json::asDouble(bool* ok) const {
    if (ok) *ok = true;
    char* end = nullptr;
    double v = std::strtod(numLiteral.c_str(), &end);
    if (end == numLiteral.c_str()) { if (ok) *ok = false; return 0; }
    return v;
}

bool ParseJson(std::string_view text, Json& out, std::string* error) {
    JsonParser p{text};
    if (!p.parseValue(out)) {
        if (error) *error = p.err;
        return false;
    }
    p.skipWs();
    if (p.o != text.size()) {
        if (error) *error = "trailing data after JSON value";
        return false;
    }
    return true;
}

std::string WriteJson(const Json& v) {
    std::string out;
    WriteJsonValue(v, 0, out);
    return out;
}

// ================================================================== melange-xom/1

namespace {

// -------- mirrors xom.cpp's CUSTOM class tables (kept in sync by hand; see xom.cpp) --------

struct CField {
    const char* name;
    Type type;         // Struct => nested record/array of `elem`
    char countKind;     // for Struct arrays: 'v' varint, 'I' u32
    const std::vector<CField>* elem;
};

const std::vector<CField> kGraphEntry = {
    {"Guid", Type::Guid, 0, nullptr}, {"Graph", Type::Ref, 0, nullptr}, {"Name", Type::String, 0, nullptr}};
const std::vector<CField> kTextChar = {
    {"Index", Type::U16, 0, nullptr}, {"MappedVal", Type::U16, 0, nullptr}, {"Unicode", Type::U16, 0, nullptr}};

std::vector<CField> DescBase(std::vector<CField> extra) {
    std::vector<CField> v = {{"ResourceId", Type::String, 0, nullptr}, {"SectionId", Type::U16, 0, nullptr}};
    v.insert(v.end(), extra.begin(), extra.end());
    return v;
}

const std::unordered_map<std::string, std::vector<CField>>& CustomClasses() {
    static const std::unordered_map<std::string, std::vector<CField>> m = {
        {"XGraphSet", {{"Graphs", Type::Struct, 'v', &kGraphEntry}}},
        {"XBaseResourceDescriptor", DescBase({})},
        {"XNullDescriptor", DescBase({})},
        {"XMeshDescriptor", DescBase({{"GraphSet", Type::Ref, 0, nullptr}, {"Flags", Type::U16, 0, nullptr}})},
        {"XBitmapDescriptor", DescBase({{"SpriteScene", Type::Ref, 0, nullptr},
                                        {"ImageWidth", Type::U16, 0, nullptr},
                                        {"ImageHeight", Type::U16, 0, nullptr}})},
        {"XSpriteSetDescriptor", DescBase({{"SpriteSetGroup", Type::Ref, 0, nullptr}})},
        {"XParticleSetDescriptor", DescBase({{"ParticleSetGroup", Type::Ref, 0, nullptr}})},
        {"XCustomDescriptor", DescBase({{"Flags", Type::U16, 0, nullptr}})},
        {"XTextDescriptor", DescBase({{"TextGroup", Type::Ref, 0, nullptr},
                                      {"Chars", Type::Struct, 'I', &kTextChar}})},
    };
    return m;
}
bool IsAnimLib(std::string_view name) { return name == "XAnimClipLibrary"; }
const std::vector<CField>* FindCustom(std::string_view name) {
    if (IsAnimLib(name)) return nullptr;  // handled separately (data-dependent layout)
    auto& m = CustomClasses();
    auto it = m.find(std::string(name));
    return it == m.end() ? nullptr : &it->second;
}
bool IsCustomOrAnim(std::string_view name) { return IsAnimLib(name) || FindCustom(name) != nullptr; }
bool IsContainerClass(std::string_view name) { return !IsCustomOrAnim(name) && findClass(name) != nullptr; }

// -------- container field walk (mirrors xom.cpp's fieldPresent()/containerFields()) --------

bool FieldPresent(const FieldDef& f, uint32_t version) {
    if (f.flags & 0x04) return false;
    if (f.flags & 0x20) return f.obsoleteFrom >= 0 && int(version) < f.obsoleteFrom;
    if (f.schemaFrom >= 0) return int(version) >= f.schemaFrom;
    return true;
}
using VersionMap = std::unordered_map<std::string, uint32_t>;
struct FieldRef { std::string key; const FieldDef* def; };
bool ContainerFieldList(std::string_view cls, const VersionMap& versions, std::vector<FieldRef>& out) {
    const ClassDef* c = findClass(cls);
    if (!c) return false;
    std::vector<const ClassDef*> chain;
    for (; c; c = classParent(*c)) chain.push_back(c);
    std::vector<std::string> seen;
    for (auto* cd : chain) {
        auto it = versions.find(cd->name);
        uint32_t ver = it == versions.end() ? 0 : it->second;
        const FieldDef* f = classFields(*cd);
        for (unsigned i = 0; i < cd->fieldCount; ++i) {
            if (!FieldPresent(f[i], ver)) continue;
            std::string key = f[i].name;
            if (std::find(seen.begin(), seen.end(), key) != seen.end())
                key = std::string(cd->name) + "." + key;
            seen.push_back(key);
            out.push_back({key, &f[i]});
        }
    }
    return true;
}

// -------- Value -> Json --------

Json MathComponentJson(Type elemAsMathChar, char elemChar, const uint8_t* p) {
    (void)elemAsMathChar;
    switch (elemChar) {
        case 'f': {
            uint32_t bits;
            std::memcpy(&bits, p, 4);
            float f;
            std::memcpy(&f, &bits, 4);
            double d = double(f);
            if (std::isfinite(d)) return Json::Num(d);
            Json o = Json::Obj();
            o.set("f32", Json::Str(HexLower(p, 4)));
            return o;
        }
        case 'h': { int16_t v = int16_t(uint16_t(p[0]) | (uint16_t(p[1]) << 8)); return Json::Int(v); }
        case 'H': { uint16_t v = uint16_t(p[0]) | (uint16_t(p[1]) << 8); return Json::UInt(v); }
        case 'b': return Json::Int(int8_t(p[0]));
        default: return Json::UInt(p[0]);
    }
}

Json MathValueToJson(const Value& v) {
    const MathDef& m = mathDef(v.math);
    size_t es = (m.elem == 'f') ? 4 : (m.elem == 'h' || m.elem == 'H') ? 2 : 1;
    Json a = Json::Arr();
    for (uint8_t i = 0; i < m.count; ++i) a.arr.push_back(MathComponentJson(Type::Math, m.elem, v.raw.data() + size_t(i) * es));
    return a;
}

Json ScalarValueToJson(const Value& v) {
    switch (v.type) {
        case Type::Bool:
            if (v.bits == 0) return Json::Bool(false);
            if (v.bits == 1) return Json::Bool(true);
            return Json::UInt(v.bits);
        case Type::U8: case Type::U16: case Type::U32: case Type::U64:
        case Type::Enum: case Type::Bitfield32: case Type::Bitfield64:
            return Json::UInt(v.asUInt());
        case Type::I8: case Type::I16: case Type::I32: case Type::I64:
            return Json::Int(v.asInt());
        case Type::F32: case Type::F64: {
            double d = v.asFloat();
            if (std::isfinite(d)) return Json::Num(d);
            Json o = Json::Obj();
            if (v.type == Type::F32) {
                uint32_t bits = uint32_t(v.bits);
                o.set("f32", Json::Str(HexLower(reinterpret_cast<const uint8_t*>(&bits), 4)));
            } else {
                uint64_t bits = v.bits;
                o.set("f64", Json::Str(HexLower(reinterpret_cast<const uint8_t*>(&bits), 8)));
            }
            return o;
        }
        case Type::String: return Json::Str(LatinToUtf8(v.str));
        case Type::Ref: { Json o = Json::Obj(); o.set("ref", Json::UInt(v.asRef())); return o; }
        case Type::Guid: return Json::Str(HexLower(v.guid.data(), 16));
        case Type::Math: return MathValueToJson(v);
        case Type::Struct: {
            Json o = Json::Obj();
            for (auto& [k, mv] : v.members) o.set(k, ScalarValueToJson(mv));
            return o;
        }
        default: return Json::Null_();
    }
}

// isTopField: only a top-level container/custom FieldDef-declared array of U8 is hex-packed
// (matches xom.py's `field()`, which only special-cases the outermost array).
Json ValueToJson(const Value& v, bool isTopField) {
    if (v.array) {
        if (isTopField && v.type == Type::U8) {
            Json o = Json::Obj();
            o.set("hex", Json::Str(HexLower(v.raw.data(), v.raw.size())));
            return o;
        }
        Json a = Json::Arr();
        size_t n = v.size();
        for (size_t i = 0; i < n; ++i) a.arr.push_back(ValueToJson(v.at(i), false));
        return a;
    }
    return ScalarValueToJson(v);
}

// -------- XAnimClipLibrary: hand-rolled, matching xom.py's _animlib/Writer.animlib --------

Json AnimLibToJson(const Object& obj) {
    // obj.fields: Name(String), Channels(Struct array), Clips(Struct array of {Duration,Name,Tracks})
    Json o = Json::Obj();
    const Value& name = obj.field("Name") ? *obj.field("Name") : Value{};
    bool culled = name.str.compare(0, 7, "XCULLED") == 0;
    o.set("Name", Json::Str(LatinToUtf8(name.str)));
    const Value* channels = obj.field("Channels");
    Json chArr = Json::Arr();
    size_t nchan = channels ? channels->items.size() : 0;
    if (channels)
        for (auto& ch : channels->items) {
            Json co = Json::Obj();
            for (auto& [k, mv] : ch.members) co.set(k, ScalarValueToJson(mv));
            chArr.arr.push_back(std::move(co));
        }
    o.set("Channels", std::move(chArr));
    const Value* clips = obj.field("Clips");
    Json clipsArr = Json::Arr();
    if (clips)
        for (auto& c : clips->items) {
            Json co = Json::Obj();
            const Value* dur = c.member("Duration");
            const Value* cname = c.member("Name");
            const Value* tracks = c.member("Tracks");
            co.set("Duration", dur ? ScalarValueToJson(*dur) : Json::Num(0));
            co.set("Name", cname ? Json::Str(LatinToUtf8(cname->str)) : Json::Str(""));
            Json trArr = Json::Arr();
            if (tracks)
                for (auto& t : tracks->items) {
                    Json to = Json::Obj();
                    for (auto& [k, mv] : t.members) {
                        if (k == "Keys") {
                            Json keysArr = Json::Arr();
                            size_t n = mv.raw.size() / 24;
                            for (size_t i = 0; i < n; ++i) {
                                Json one = Json::Arr();
                                const uint8_t* p = mv.raw.data() + i * 24;
                                for (int j = 0; j < 6; ++j) one.arr.push_back(MathComponentJson(Type::Math, 'f', p + j * 4));
                                keysArr.arr.push_back(std::move(one));
                            }
                            to.set("Keys", std::move(keysArr));
                        } else {
                            to.set(k, ScalarValueToJson(mv));
                        }
                    }
                    trArr.arr.push_back(std::move(to));
                }
            (void)nchan;
            (void)culled;
            co.set("Tracks", std::move(trArr));
            clipsArr.arr.push_back(std::move(co));
        }
    o.set("Clips", std::move(clipsArr));
    return o;
}

// -------- Object -> Json --------

Json ObjectToJson(const Object& ob) {
    Json o = Json::Obj();
    o.set("type", Json::Str(LatinToUtf8(ob.type)));
    if (ob.inTail) {
        o.set("in_tail", Json::Bool(true));
        return o;
    }
    if (ob.opaque) {
        o.set("raw", Json::Str(HexLowerSpaced(ob.raw.data(), ob.raw.size())));
        o.set("error", Json::Str(LatinToUtf8(ob.error)));
        return o;
    }
    if (ob.type == "XAnimClipLibrary") {
        o.set("fields", AnimLibToJson(ob));
        return o;
    }
    if (!ob.container) {
        Json f = Json::Obj();
        for (auto& [k, v] : ob.fields) f.set(k, ValueToJson(v, true));
        o.set("fields", std::move(f));
        return o;
    }
    o.set("iflags", Json::UInt(ob.internalFlags));
    o.set("uflags", Json::UInt(ob.userFlags));
    o.set("dxcount", Json::UInt(ob.dxFieldCount));
    Json f = Json::Obj();
    for (auto& [k, v] : ob.fields) f.set(k, ValueToJson(v, true));
    o.set("fields", std::move(f));
    return o;
}

}  // namespace

std::string DocumentToJson(const Document& doc) {
    Json root = Json::Obj();
    root.set("format", Json::Str("melange-xom/1"));

    Json header = Json::Obj();
    header.set("version", Json::Str(HexLower(doc.version.data(), 4)));
    bool allZero08 = std::all_of(doc.reserved08.begin(), doc.reserved08.end(), [](uint8_t b) { return b == 0; });
    bool allZero24 = std::all_of(doc.reserved24.begin(), doc.reserved24.end(), [](uint8_t b) { return b == 0; });
    if (!allZero08 || !allZero24) {
        header.set("reserved_08", Json::Str(HexLower(doc.reserved08.data(), doc.reserved08.size())));
        header.set("reserved_24", Json::Str(HexLower(doc.reserved24.data(), doc.reserved24.size())));
    }
    root.set("header", std::move(header));

    Json types = Json::Arr();
    for (auto& t : doc.types) {
        Json to = Json::Obj();
        to.set("name", Json::Str(LatinToUtf8(t.name)));
        to.set("version", Json::UInt(t.version));
        to.set("count", Json::UInt(t.count));
        to.set("guid", Json::Str(HexLower(t.guid.data(), 16)));
        if (t.c) to.set("c", Json::UInt(t.c));
        std::string expectRaw = t.name;
        expectRaw.resize(32, '\0');
        std::string actualRaw(reinterpret_cast<const char*>(t.rawName.data()), 32);
        if (actualRaw != expectRaw) to.set("rawname", Json::Str(HexLower(t.rawName.data(), 32)));
        if (!t.cls.empty()) to.set("class", Json::Str(LatinToUtf8(t.cls)));
        types.arr.push_back(std::move(to));
    }
    root.set("types", std::move(types));

    auto recArr = [](const std::array<uint32_t, 3>& r) {
        Json a = Json::Arr();
        for (auto v : r) a.arr.push_back(Json::UInt(v));
        return a;
    };
    root.set("guid_rec", recArr(doc.guidRec));
    root.set("schm_rec", recArr(doc.schmRec));

    Json strings = Json::Arr();
    for (auto& s : doc.strings) strings.arr.push_back(Json::Str(LatinToUtf8(s)));
    root.set("strings", std::move(strings));

    root.set("root", Json::UInt(doc.root));

    Json objects = Json::Arr();
    for (auto& ob : doc.objects) objects.arr.push_back(ObjectToJson(ob));
    root.set("objects", std::move(objects));

    if (!doc.strsRaw.empty()) root.set("strs_raw", Json::Str(HexLower(doc.strsRaw.data(), doc.strsRaw.size())));

    // Tail: the first in_tail object carries the raw reader error (see xom.h); reconstruct
    // Python's single formatted "tail_error" message from it.
    if (!doc.tailRaw.empty()) {
        root.set("tail_raw", Json::Str(HexLower(doc.tailRaw.data(), doc.tailRaw.size())));
        size_t firstTail = 0;
        for (; firstTail < doc.objects.size(); ++firstTail)
            if (doc.objects[firstTail].inTail) break;
        std::string msg = "object #" + std::to_string(firstTail + 1) + " " + doc.objects[firstTail].type + ": " +
                           doc.objects[firstTail].error;
        root.set("tail_error", Json::Str(LatinToUtf8(msg)));
    } else if (!doc.trailer.empty()) {
        root.set("trailer", Json::Str(HexLower(doc.trailer.data(), doc.trailer.size())));
    }
    return WriteJson(root);
}

// ================================================================== Json -> Document

namespace {

bool JErr(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

bool JsonToValue(const Json& j, Type type, uint16_t math, Value& out, std::string* error, const std::string& where);

bool JsonToMath(const Json& j, uint16_t math, Value& out, std::string* error, const std::string& where) {
    if (j.kind != Json::Kind::Array) return JErr(error, where + ": expected an array");
    const MathDef& m = mathDef(math);
    if (j.arr.size() != m.count)
        return JErr(error, where + ": expected " + std::to_string(m.count) + " components");
    out.type = Type::Math;
    out.math = math;
    out.raw.clear();
    for (uint8_t i = 0; i < m.count; ++i) {
        const Json& c = j.arr[i];
        if (m.elem == 'f') {
            double d;
            if (c.kind == Json::Kind::Object) {
                const Json* hex = c.find("f32");
                if (!hex || hex->kind != Json::Kind::String) return JErr(error, where + ": expected {\"f32\":hex}");
                std::vector<uint8_t> b;
                if (!HexDecode(hex->str, b) || b.size() != 4) return JErr(error, where + ": bad f32 hex");
                out.raw.insert(out.raw.end(), b.begin(), b.end());
                continue;
            }
            if (c.kind != Json::Kind::Number) return JErr(error, where + ": expected a number");
            bool ok; d = c.asDouble(&ok);
            if (!ok) return JErr(error, where + ": bad number");
            float f = float(d);
            uint32_t bits;
            std::memcpy(&bits, &f, 4);
            for (int k = 0; k < 4; ++k) out.raw.push_back(uint8_t(bits >> (8 * k)));
        } else {
            if (c.kind != Json::Kind::Number) return JErr(error, where + ": expected an integer");
            bool ok; int64_t iv = c.asInt64(&ok);
            if (!ok) return JErr(error, where + ": bad integer");
            size_t es = (m.elem == 'h' || m.elem == 'H') ? 2 : 1;
            uint32_t u = uint32_t(iv);
            for (size_t k = 0; k < es; ++k) out.raw.push_back(uint8_t(u >> (8 * k)));
        }
    }
    return true;
}

bool JsonToValue(const Json& j, Type type, uint16_t math, Value& out, std::string* error, const std::string& where) {
    out = Value{};
    out.type = type;
    out.math = math;
    switch (type) {
        case Type::Bool: {
            if (j.kind == Json::Kind::Bool) { out.bits = j.boolean ? 1 : 0; return true; }
            if (j.kind == Json::Kind::Number) { bool ok; out.bits = j.asUInt64(&ok); return ok || JErr(error, where + ": bad bool"); }
            return JErr(error, where + ": expected bool or int");
        }
        case Type::U8: case Type::U16: case Type::U32: case Type::U64:
        case Type::Enum: case Type::Bitfield32: case Type::Bitfield64: {
            if (j.kind != Json::Kind::Number) return JErr(error, where + ": expected an unsigned integer");
            bool ok; out.bits = j.asUInt64(&ok);
            return ok || JErr(error, where + ": bad unsigned integer");
        }
        case Type::I8: case Type::I16: case Type::I32: case Type::I64: {
            if (j.kind != Json::Kind::Number) return JErr(error, where + ": expected an integer");
            bool ok; int64_t v = j.asInt64(&ok);
            if (!ok) return JErr(error, where + ": bad integer");
            out.setInt(v);
            return true;
        }
        case Type::F32: case Type::F64: {
            if (j.kind == Json::Kind::Object) {
                const char* key = type == Type::F32 ? "f32" : "f64";
                const Json* hex = j.find(key);
                if (!hex || hex->kind != Json::Kind::String) return JErr(error, where + ": expected {\"" + std::string(key) + "\":hex}");
                std::vector<uint8_t> b;
                size_t n = type == Type::F32 ? 4 : 8;
                if (!HexDecode(hex->str, b) || b.size() != n) return JErr(error, where + ": bad float hex");
                uint64_t bits = 0;
                for (size_t k = 0; k < n; ++k) bits |= uint64_t(b[k]) << (8 * k);
                out.bits = bits;
                return true;
            }
            if (j.kind != Json::Kind::Number) return JErr(error, where + ": expected a float");
            bool ok; double d = j.asDouble(&ok);
            if (!ok) return JErr(error, where + ": bad float");
            out.setFloat(d);
            return true;
        }
        case Type::String: {
            if (j.kind != Json::Kind::String) return JErr(error, where + ": expected a string");
            if (!Utf8ToLatin1(j.str, out.str)) return JErr(error, where + ": string has a character outside Latin-1");
            return true;
        }
        case Type::Ref: {
            uint64_t r = 0;
            if (j.kind == Json::Kind::Object) {
                const Json* rf = j.find("ref");
                if (!rf || rf->kind != Json::Kind::Number) return JErr(error, where + ": expected {\"ref\":n}");
                bool ok; r = rf->asUInt64(&ok);
                if (!ok) return JErr(error, where + ": bad ref");
            } else if (j.kind == Json::Kind::Number) {
                bool ok; r = j.asUInt64(&ok);
                if (!ok) return JErr(error, where + ": bad ref");
            } else {
                return JErr(error, where + ": expected a ref");
            }
            out.bits = r;
            return true;
        }
        case Type::Guid: {
            if (j.kind != Json::Kind::String) return JErr(error, where + ": expected a guid string");
            if (!HexDecodeFixed(j.str, out.guid.data(), 16)) return JErr(error, where + ": bad guid");
            return true;
        }
        case Type::Math: return JsonToMath(j, math, out, error, where);
        default: return JErr(error, where + ": unsupported field type");
    }
}

bool JsonToArrayValue(const Json& j, Type elemType, uint16_t math, Value& out, std::string* error, const std::string& where) {
    out = Value{};
    out.type = elemType;
    out.math = math;
    out.array = true;
    if (elemType == Type::U8) {
        if (j.kind != Json::Kind::Object) return JErr(error, where + ": expected {\"hex\":...}");
        const Json* hex = j.find("hex");
        if (!hex || hex->kind != Json::Kind::String) return JErr(error, where + ": expected {\"hex\":...}");
        if (!HexDecode(hex->str, out.raw)) return JErr(error, where + ": bad hex");
        return true;
    }
    if (j.kind != Json::Kind::Array) return JErr(error, where + ": expected an array");
    // Must match xom.cpp's fixedSize(): every fixed-size element type is packed into `raw`
    // (Value::packed()), regardless of whether Python's JSON model happens to special-case it
    // (only u8, handled above, gets the {"hex":...} shorthand there).
    size_t es = 0;
    switch (elemType) {
        case Type::Bool: case Type::I8: es = 1; break;
        case Type::U16: case Type::I16: es = 2; break;
        case Type::U32: case Type::I32: case Type::F32: case Type::Enum: case Type::Bitfield32: es = 4; break;
        case Type::U64: case Type::I64: case Type::F64: case Type::Bitfield64: es = 8; break;
        case Type::Math: {
            const MathDef& m = mathDef(math);
            size_t elemSz = m.elem == 'f' ? 4 : (m.elem == 'h' || m.elem == 'H') ? 2 : 1;
            es = elemSz * m.count;
            break;
        }
        default: es = 0;  // String, Ref, Guid, Struct -> items
    }
    if (es) {
        out.raw.reserve(j.arr.size() * es);
        for (auto& e : j.arr) {
            Value one;
            if (!JsonToValue(e, elemType, math, one, error, where)) return false;
            if (elemType == Type::Math) {
                if (one.raw.size() != es) return JErr(error, where + ": math element has the wrong size");
                out.raw.insert(out.raw.end(), one.raw.begin(), one.raw.end());
            } else {
                for (size_t k = 0; k < es; ++k) out.raw.push_back(uint8_t(one.bits >> (8 * k)));
            }
        }
    } else {
        out.items.resize(j.arr.size());
        for (size_t i = 0; i < j.arr.size(); ++i)
            if (!JsonToValue(j.arr[i], elemType, math, out.items[i], error, where + "[" + std::to_string(i) + "]"))
                return false;
    }
    return true;
}

bool JsonToCustom(const std::vector<CField>& defs, const Json& fields, std::vector<std::pair<std::string, Value>>& out,
                   std::string* error, const std::string& where) {
    for (auto& cf : defs) {
        const Json* fv = fields.find(cf.name);
        if (!fv) return JErr(error, where + ": missing field " + cf.name);
        Value v;
        if (cf.type == Type::Struct) {
            if (fv->kind != Json::Kind::Array) return JErr(error, where + "." + cf.name + ": expected an array");
            v.type = Type::Struct;
            v.array = true;
            v.items.resize(fv->arr.size());
            for (size_t i = 0; i < fv->arr.size(); ++i) {
                v.items[i].type = Type::Struct;
                if (!JsonToCustom(*cf.elem, fv->arr[i], v.items[i].members, error,
                                   where + "." + cf.name + "[" + std::to_string(i) + "]"))
                    return false;
            }
        } else if (!JsonToValue(*fv, cf.type, 0, v, error, where + "." + cf.name)) {
            return false;
        }
        out.emplace_back(cf.name, std::move(v));
    }
    return true;
}

bool JsonToAnimLib(const Json& fields, Object& obj, std::string* error, const std::string& where) {
    const Json* nameJ = fields.find("Name");
    const Json* chJ = fields.find("Channels");
    const Json* clipsJ = fields.find("Clips");
    if (!nameJ || !chJ || !clipsJ) return JErr(error, where + ": XAnimClipLibrary needs Name, Channels, Clips");
    std::string name;
    if (nameJ->kind != Json::Kind::String || !Utf8ToLatin1(nameJ->str, name))
        return JErr(error, where + ".Name: bad string");
    bool culled = name.compare(0, 7, "XCULLED") == 0;
    Value nameV; nameV.type = Type::String; nameV.str = name;
    obj.fields.emplace_back("Name", std::move(nameV));

    static const std::vector<CField> kAnimChannel = {
        {"Byte4", Type::U8, 0, nullptr}, {"Word6", Type::U16, 0, nullptr},
        {"Byte5", Type::U8, 0, nullptr}, {"Name", Type::String, 0, nullptr}};
    if (chJ->kind != Json::Kind::Array) return JErr(error, where + ".Channels: expected an array");
    Value chV; chV.type = Type::Struct; chV.array = true; chV.items.resize(chJ->arr.size());
    for (size_t i = 0; i < chJ->arr.size(); ++i) {
        chV.items[i].type = Type::Struct;
        Json wrap = Json::Obj();
        for (auto& [k, v] : chJ->arr[i].obj) wrap.set(k, v);
        if (!JsonToCustom(kAnimChannel, chJ->arr[i], chV.items[i].members, error, where + ".Channels[" + std::to_string(i) + "]"))
            return false;
    }
    size_t nchan = chV.items.size();
    obj.fields.emplace_back("Channels", std::move(chV));

    if (clipsJ->kind != Json::Kind::Array) return JErr(error, where + ".Clips: expected an array");
    Value clipsV; clipsV.type = Type::Struct; clipsV.array = true; clipsV.items.resize(clipsJ->arr.size());
    for (size_t ci = 0; ci < clipsJ->arr.size(); ++ci) {
        const Json& cj = clipsJ->arr[ci];
        Value& cv = clipsV.items[ci];
        cv.type = Type::Struct;
        std::string cw = where + ".Clips[" + std::to_string(ci) + "]";
        const Json* dur = cj.find("Duration");
        const Json* cname = cj.find("Name");
        const Json* tracks = cj.find("Tracks");
        if (!dur || !cname || !tracks) return JErr(error, cw + ": expected Duration, Name, Tracks");
        Value durV;
        if (!JsonToValue(*dur, Type::F32, 0, durV, error, cw + ".Duration")) return false;
        cv.members.emplace_back("Duration", std::move(durV));
        Value cnameV; cnameV.type = Type::String;
        if (cname->kind != Json::Kind::String || !Utf8ToLatin1(cname->str, cnameV.str))
            return JErr(error, cw + ".Name: bad string");
        cv.members.emplace_back("Name", std::move(cnameV));
        if (tracks->kind != Json::Kind::Array) return JErr(error, cw + ".Tracks: expected an array");
        size_t need = culled ? tracks->arr.size() : nchan;
        if (!culled && tracks->arr.size() != nchan)
            return JErr(error, cw + ".Tracks: need one track per channel");
        Value trV; trV.type = Type::Struct; trV.array = true; trV.items.resize(tracks->arr.size());
        for (size_t ti = 0; ti < tracks->arr.size(); ++ti) {
            const Json& tj = tracks->arr[ti];
            Value& tv = trV.items[ti];
            tv.type = Type::Struct;
            std::string tw = cw + ".Tracks[" + std::to_string(ti) + "]";
            static const char* flagNames[4] = {"Flag1", "Flag8", "Flag4", "Flag2"};
            for (auto* fn : flagNames) {
                const Json* fv = tj.find(fn);
                if (!fv) return JErr(error, tw + ": missing " + std::string(fn));
                Value b;
                if (!JsonToValue(*fv, Type::Bool, 0, b, error, tw + "." + fn)) return false;
                tv.members.emplace_back(fn, std::move(b));
            }
            if (culled) {
                const Json* ch = tj.find("Channel");
                if (!ch) return JErr(error, tw + ": missing Channel");
                Value c;
                if (!JsonToValue(*ch, Type::U16, 0, c, error, tw + ".Channel")) return false;
                tv.members.emplace_back("Channel", std::move(c));
            }
            const Json* ba = tj.find("BitsA");
            const Json* bb = tj.find("BitsB");
            const Json* keys = tj.find("Keys");
            if (!ba || !bb || !keys) return JErr(error, tw + ": missing BitsA/BitsB/Keys");
            Value baV, bbV;
            if (!JsonToValue(*ba, Type::U32, 0, baV, error, tw + ".BitsA")) return false;
            if (!JsonToValue(*bb, Type::U32, 0, bbV, error, tw + ".BitsB")) return false;
            tv.members.emplace_back("BitsA", std::move(baV));
            tv.members.emplace_back("BitsB", std::move(bbV));
            if (keys->kind != Json::Kind::Array) return JErr(error, tw + ".Keys: expected an array");
            Value keysV; keysV.type = Type::F32; keysV.array = true;
            for (size_t ki = 0; ki < keys->arr.size(); ++ki) {
                const Json& kj = keys->arr[ki];
                if (kj.kind != Json::Kind::Array || kj.arr.size() != 6)
                    return JErr(error, tw + ".Keys[" + std::to_string(ki) + "]: expected 6 numbers");
                for (int c = 0; c < 6; ++c) {
                    Value one;
                    if (!JsonToValue(kj.arr[c], Type::F32, 0, one, error, tw + ".Keys"))
                        return false;
                    for (int k = 0; k < 4; ++k) keysV.raw.push_back(uint8_t(one.bits >> (8 * k)));
                }
            }
            tv.members.emplace_back("Keys", std::move(keysV));
        }
        (void)need;
        cv.members.emplace_back("Tracks", std::move(trV));
    }
    obj.fields.emplace_back("Clips", std::move(clipsV));
    return true;
}

}  // namespace

bool JsonToDocument(std::string_view jsonText, Document& out, std::string* error) {
    Json root;
    if (!ParseJson(jsonText, root, error)) return false;
    if (root.kind != Json::Kind::Object) return JErr(error, "top level must be an object");
    const Json* fmt = root.find("format");
    if (!fmt || fmt->kind != Json::Kind::String || fmt->str != "melange-xom/1")
        return JErr(error, "not a melange-xom/1 document");

    out = Document{};
    const Json* header = root.find("header");
    if (!header) return JErr(error, "missing header");
    const Json* ver = header->find("version");
    if (!ver || !HexDecodeFixed(ver->str, out.version.data(), 4)) return JErr(error, "header.version: bad hex");
    const Json* r08 = header->find("reserved_08");
    const Json* r24 = header->find("reserved_24");
    if (r08 && !HexDecodeFixed(r08->str, out.reserved08.data(), 16)) return JErr(error, "header.reserved_08: bad hex");
    if (r24 && !HexDecodeFixed(r24->str, out.reserved24.data(), 28)) return JErr(error, "header.reserved_24: bad hex");

    const Json* types = root.find("types");
    if (!types || types->kind != Json::Kind::Array) return JErr(error, "missing types");
    for (auto& tj : types->arr) {
        TypeEntry t;
        const Json* name = tj.find("name");
        const Json* version = tj.find("version");
        const Json* count = tj.find("count");
        const Json* guid = tj.find("guid");
        if (!name || !version || !count || !guid) return JErr(error, "types[]: missing a field");
        if (!Utf8ToLatin1(name->str, t.name)) return JErr(error, "types[].name: bad string");
        bool ok;
        t.version = uint32_t(version->asUInt64(&ok));
        t.count = uint32_t(count->asUInt64(&ok));
        if (!HexDecodeFixed(guid->str, t.guid.data(), 16)) return JErr(error, "types[].guid: bad hex");
        if (const Json* c = tj.find("c")) t.c = uint32_t(c->asUInt64(&ok));
        if (const Json* rn = tj.find("rawname")) {
            if (!HexDecodeFixed(rn->str, t.rawName.data(), 32)) return JErr(error, "types[].rawname: bad hex");
        } else {
            std::string padded = t.name;
            padded.resize(32, '\0');
            std::memcpy(t.rawName.data(), padded.data(), 32);
        }
        if (const Json* cls = tj.find("class")) {
            if (!Utf8ToLatin1(cls->str, t.cls)) return JErr(error, "types[].class: bad string");
        }
        out.types.push_back(std::move(t));
    }

    auto readRec = [&](const char* key, std::array<uint32_t, 3>& rec) {
        const Json* r = root.find(key);
        if (!r || r->kind != Json::Kind::Array || r->arr.size() != 3) return false;
        bool ok;
        for (int i = 0; i < 3; ++i) rec[i] = uint32_t(r->arr[i].asUInt64(&ok));
        return true;
    };
    if (!readRec("guid_rec", out.guidRec)) return JErr(error, "missing guid_rec");
    if (!readRec("schm_rec", out.schmRec)) return JErr(error, "missing schm_rec");

    const Json* strings = root.find("strings");
    if (!strings || strings->kind != Json::Kind::Array) return JErr(error, "missing strings");
    for (auto& s : strings->arr) {
        std::string raw;
        if (s.kind != Json::Kind::String || !Utf8ToLatin1(s.str, raw)) return JErr(error, "strings[]: bad string");
        out.strings.push_back(std::move(raw));
    }

    const Json* root_ = root.find("root");
    if (!root_) return JErr(error, "missing root");
    bool ok;
    out.root = uint32_t(root_->asUInt64(&ok));

    VersionMap versions;
    for (auto& t : out.types) versions[t.className()] = t.version;

    const Json* objects = root.find("objects");
    if (!objects || objects->kind != Json::Kind::Array) return JErr(error, "missing objects");
    for (size_t i = 0; i < objects->arr.size(); ++i) {
        const Json& oj = objects->arr[i];
        std::string objWhere = "objects[" + std::to_string(i) + "]";
        const Json* type = oj.find("type");
        if (!type || type->kind != Json::Kind::String) return JErr(error, objWhere + ": missing type");
        Object ob;
        if (!Utf8ToLatin1(type->str, ob.type)) return JErr(error, objWhere + ".type: bad string");
        if (oj.find("in_tail")) {
            ob.inTail = true;
            out.objects.push_back(std::move(ob));
            continue;
        }
        if (const Json* raw = oj.find("raw")) {
            ob.opaque = true;
            if (raw->kind != Json::Kind::String || !HexDecode(raw->str, ob.raw))
                return JErr(error, objWhere + ".raw: bad hex");
            if (const Json* e = oj.find("error")) Utf8ToLatin1(e->str, ob.error);
            out.objects.push_back(std::move(ob));
            continue;
        }
        const Json* fields = oj.find("fields");
        if (!fields || fields->kind != Json::Kind::Object) return JErr(error, objWhere + ": missing fields");
        if (ob.type == "XAnimClipLibrary") {
            ob.container = false;
            if (!JsonToAnimLib(*fields, ob, error, objWhere)) return false;
            out.objects.push_back(std::move(ob));
            continue;
        }
        if (auto* cd = FindCustom(ob.type)) {
            ob.container = false;
            if (!JsonToCustom(*cd, *fields, ob.fields, error, objWhere)) return false;
            out.objects.push_back(std::move(ob));
            continue;
        }
        if (!IsContainerClass(ob.type)) return JErr(error, objWhere + ": class not in schema: " + ob.type);
        const Json* iflags = oj.find("iflags");
        const Json* uflags = oj.find("uflags");
        const Json* dxcount = oj.find("dxcount");
        if (!iflags || !uflags || !dxcount) return JErr(error, objWhere + ": missing iflags/uflags/dxcount");
        ob.internalFlags = uint8_t(iflags->asUInt64(&ok));
        ob.userFlags = uint8_t(uflags->asUInt64(&ok));
        ob.dxFieldCount = uint8_t(dxcount->asUInt64(&ok));
        std::vector<FieldRef> fr;
        if (!ContainerFieldList(ob.type, versions, fr)) return JErr(error, objWhere + ": class not in schema: " + ob.type);
        for (auto& f : fr) {
            const Json* fv = fields->find(f.key);
            if (!fv) return JErr(error, objWhere + ".fields." + f.key + ": missing field");
            Value v;
            std::string w = objWhere + ".fields." + f.key;
            bool okField = f.def->isArray() ? JsonToArrayValue(*fv, f.def->type, f.def->math, v, error, w)
                                             : JsonToValue(*fv, f.def->type, f.def->math, v, error, w);
            if (!okField) return false;
            ob.fields.emplace_back(f.key, std::move(v));
        }
        out.objects.push_back(std::move(ob));
    }

    if (const Json* sr = root.find("strs_raw")) {
        if (sr->kind != Json::Kind::String || !HexDecode(sr->str, out.strsRaw))
            return JErr(error, "strs_raw: bad hex");
    }
    if (const Json* tr = root.find("tail_raw")) {
        if (tr->kind != Json::Kind::String || !HexDecode(tr->str, out.tailRaw))
            return JErr(error, "tail_raw: bad hex");
        for (auto& ob : out.objects)
            if (ob.inTail) { ob.raw.clear(); break; }
    }
    if (const Json* trailer = root.find("trailer")) {
        if (trailer->kind != Json::Kind::String || !HexDecode(trailer->str, out.trailer))
            return JErr(error, "trailer: bad hex");
    }
    return true;
}

}  // namespace melange::xom
