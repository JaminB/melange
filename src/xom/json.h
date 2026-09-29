// melange::xom::json - the "melange-xom/1" JSON document model, and a small
// generic JSON reader/writer used to get there (and reused by gltf.h).
// C++17, standard library only. Mirrors tools/xom/xom.py's loads()/dumps()/to_json().
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "xom.h"

namespace melange::xom {

// A generic JSON value (subset needed here: no Unicode surrogate pairs beyond
// what glTF/our own output uses). Numbers keep their exact source text so an
// integer field (e.g. a u64) round-trips without going through a double.
struct Json {
    enum class Kind { Null, Bool, Number, String, Array, Object };
    Kind kind = Kind::Null;
    bool boolean = false;
    std::string numLiteral;  // e.g. "120", "-3.5", "1e+20"; valid when kind == Number
    std::string str;         // valid when kind == String
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;  // insertion order preserved

    static Json Null_() { return Json{}; }
    static Json Bool(bool b) { Json j; j.kind = Kind::Bool; j.boolean = b; return j; }
    static Json Int(int64_t v);
    static Json UInt(uint64_t v);
    static Json Num(double v);  // used only for values with no exact-integer requirement (glTF)
    static Json Str(std::string s) { Json j; j.kind = Kind::String; j.str = std::move(s); return j; }
    static Json Arr() { Json j; j.kind = Kind::Array; return j; }
    static Json Obj() { Json j; j.kind = Kind::Object; return j; }

    bool isNull() const { return kind == Kind::Null; }
    const Json* find(std::string_view key) const;
    void set(std::string key, Json v);  // object: replace in place, else append

    // Parses numLiteral. ok is optional; on overflow/format error it is set false and 0 returned.
    int64_t asInt64(bool* ok = nullptr) const;
    uint64_t asUInt64(bool* ok = nullptr) const;
    double asDouble(bool* ok = nullptr) const;
};

// Parses JSON text (RFC 8259, plus bare NaN/Infinity are rejected like Python's
// strict json.loads). Returns false and fills *error with a byte offset on failure.
bool ParseJson(std::string_view text, Json& out, std::string* error = nullptr);

// Serializes with json.dumps(value, indent=1, ensure_ascii=False)'s exact formatting:
// 1-space indent per level, ": " after object keys, no space after ",", no trailing
// newline, and multi-byte UTF-8 for any codepoint >= 0x80 (never \uXXXX escapes
// except for control characters, which use lowercase hex).
std::string WriteJson(const Json& v);

// doc -> the melange-xom/1 JSON text, byte-identical to Python's to_json(loads(bytes)).
std::string DocumentToJson(const Document& doc);

// The inverse: parses melange-xom/1 JSON text into a Document ready for serialize().
// Returns false and fills *error (naming the object index and field on a schema
// mismatch) on malformed or inconsistent input.
bool JsonToDocument(std::string_view jsonText, Document& out, std::string* error = nullptr);

}  // namespace melange::xom
