#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Small strict JSON (RFC 8259) DOM reader with line/column positions. Never throws.
namespace melange::json {
constexpr size_t kMaxBytes = 1 << 20;
constexpr int kMaxDepth = 64;

enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;                                   // UTF-8
    std::vector<Value> items;                             // Array
    std::vector<std::pair<std::string, Value>> members;   // Object, in document order; keys are unique
    int line = 0, col = 0;                                // 1-based position of the value's first character

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
    bool IsArray() const { return type == Type::Array; }
    bool IsObject() const { return type == Type::Object; }
    bool IsInteger() const;                               // a number with no fractional part within +-2^53
    const Value* Get(std::string_view key) const;         // object member or nullptr
};
const char* TypeName(Type t);

struct Error {
    int line = 0, col = 0;                                // 1-based; col counts bytes
    std::string text;
};

bool Parse(std::string_view text, Value* out, Error* err, size_t maxBytes = kMaxBytes);
bool ParseFile(const std::wstring& path, Value* out, Error* err, size_t maxBytes = kMaxBytes);
}  // namespace melange::json
