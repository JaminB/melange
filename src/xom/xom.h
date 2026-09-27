// wumfix::xom - portable reader/writer for Worms Ultimate Mayhem "MOIK" XOM files.
//
// C++17, standard library only, no exceptions required, no OS calls: builds for
// Win32, Linux and WebAssembly (Emscripten) alike. Mirrors tools/xom/xom.py; see
// tools/xom/CPP_PORT.md for the design and re/notes/framework/xom-format.md for
// the format. The class schemas are compiled in (xom_schema.inc, generated from
// tools/xom/schema.json by tools/xom/gen_cpp_schema.py).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wumfix::xom {

enum class Type : uint8_t {
    Void, Bool, U8, I8, U16, I16, U32, I32, U64, I64, F32, F64, Enum, Bitfield32, Bitfield64,
    String, Ref, Pointer, Interface,
    Math,    // fixed-size struct (Vector3f, Color4ub, Matrix4f, ...): see MathDef
    Guid,    // 16 raw bytes (custom classes only)
    Struct,  // named members (custom classes only)
};

struct MathDef {
    const char* name;
    char elem;       // 'f' float32, 'h' int16, 'H' uint16, 'b' int8, 'B' uint8
    uint8_t count;   // number of elements
};

struct FieldDef {
    const char* name;
    Type type;
    uint16_t math;       // index into math table when type == Math
    uint8_t flags;       // engine flags: 0x01 array, 0x04 transient, 0x20 obsolete
    int8_t schemaFrom;   // "Schema FromVersion" attribute, -1 if absent
    int8_t obsoleteFrom; // "Obsolete FromVersion" attribute, -1 if absent
    bool isArray() const { return flags & 0x01; }
};

struct ClassDef {
    const char* name;
    const char* guid;    // 32 hex chars as stored in the TYPE table
    int16_t parent;      // index into the class table, -1 for root
    uint16_t firstField;
    uint16_t fieldCount;
};

// Schema lookup (compiled-in tables).
const ClassDef* findClass(std::string_view name);
const ClassDef* findClassByGuid(std::string_view guidHex);
const MathDef& mathDef(uint16_t index);
const FieldDef* classFields(const ClassDef& c);
const ClassDef* classParent(const ClassDef& c);

// A decoded value. Scalars keep their exact bit pattern in `bits` so a round
// trip is lossless; use the typed accessors to read/write them.
struct Value {
    Type type = Type::Void;
    uint16_t math = 0;                 // Math: index into the math table
    bool array = false;                // true: elements are in `items`
    uint64_t bits = 0;                 // Bool/ints/Enum/Bitfield: value; F32/F64: IEEE bits; Ref: index
    std::string str;                   // String
    std::array<uint8_t, 16> guid{};    // Guid
    std::vector<uint8_t> raw;          // Math: little-endian element bytes; also the packed
                                       // elements of arrays of fixed-size types (see below)
    std::vector<Value> items;          // elements of String/Ref/Struct arrays
    std::vector<std::pair<std::string, Value>> members;  // Struct

    int64_t asInt() const;
    uint64_t asUInt() const { return bits; }
    double asFloat() const;            // F32/F64
    bool asBool() const { return bits != 0; }
    uint32_t asRef() const { return static_cast<uint32_t>(bits); }  // 1-based object index, 0 = null
    void setInt(int64_t v);
    void setFloat(double v);
    // Math components as doubles (exact for every supported element type).
    std::vector<double> components() const;
    void setComponents(const std::vector<double>& v);
    const Value* member(std::string_view name) const;

    // Arrays. Elements of fixed-size types (numbers, bools, enums, math structs)
    // are stored packed in `raw` (a 4 MB texture would otherwise cost ~500 MB as
    // individual Values); String/Ref/Struct elements live in `items`. These
    // accessors hide the difference.
    bool packed() const;
    size_t size() const;
    Value at(size_t i) const;
    void set(size_t i, const Value& v);
    void resize(size_t n);
};

struct Object {
    std::string type;                  // schema class name (resolved via GUID)
    bool container = true;             // XContainer subclass (CTNR tag) vs custom serialiser
    uint8_t internalFlags = 0, userFlags = 0, dxFieldCount = 0;
    std::vector<std::pair<std::string, Value>> fields;  // stream order
    bool opaque = false;               // payload kept as `raw` (no schema / decode failed)
    bool inTail = false;               // part of Document::tailRaw (undelimitable region)
    std::vector<uint8_t> raw;
    std::string error;

    Value* field(std::string_view name);
    const Value* field(std::string_view name) const;
};

struct TypeEntry {
    std::string name;                  // as stored (truncated to 31 chars)
    std::string cls;                   // resolved schema class ("" = unknown)
    uint32_t version = 0, count = 0, c = 0;
    std::array<uint8_t, 16> guid{};
    std::array<uint8_t, 32> rawName{};
    const std::string& className() const { return cls.empty() ? name : cls; }
};

struct Document {
    std::array<uint8_t, 4> version{{0, 0, 0, 2}};
    std::array<uint8_t, 16> reserved08{};
    std::array<uint8_t, 28> reserved24{};
    std::vector<TypeEntry> types;
    std::array<uint32_t, 3> guidRec{}, schmRec{};
    std::vector<std::string> strings;  // STRS table; index = string id
    std::vector<uint8_t> strsRaw;      // original STRS bytes if not canonical (never seen)
    uint32_t root = 0;                 // 1-based object index
    std::vector<Object> objects;       // 1-based index = position + 1
    std::vector<uint8_t> tailRaw;      // bytes of objects that could not be delimited
    std::vector<uint8_t> trailer;      // bytes after the last object (never seen)

    Object* object(uint32_t ref) { return ref && ref <= objects.size() ? &objects[ref - 1] : nullptr; }
    const Object* object(uint32_t ref) const { return ref && ref <= objects.size() ? &objects[ref - 1] : nullptr; }
};

struct ParseOptions {
    bool strict = false;               // fail instead of keeping undecodable objects opaque
};

// Returns false and fills *error on malformed input.
bool parse(const uint8_t* data, size_t size, Document& out, std::string* error = nullptr,
           const ParseOptions& opt = {});
// Returns false and fills *error if the document is inconsistent (e.g. objects not
// grouped in TYPE-table order, missing fields).
bool serialize(const Document& doc, std::vector<uint8_t>& out, std::string* error = nullptr);

}  // namespace wumfix::xom
