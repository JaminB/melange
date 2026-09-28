#include "tools/json_read.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace melange::json {
namespace {
class Parser {
public:
    Parser(std::string_view s, Error* err) : s_(s), err_(err) {}

    bool Document(Value* out) {
        if (s_.size() >= 3 && s_.substr(0, 3) == "\xEF\xBB\xBF") Advance(3);
        Ws();
        if (!ParseValue(out, 0)) return false;
        Ws();
        if (i_ < s_.size()) return Fail("unexpected content after the top-level value");
        return true;
    }

private:
    std::string_view s_;
    Error* err_;
    size_t i_ = 0, lineStart_ = 0;
    int line_ = 1;

    int Col() const { return static_cast<int>(i_ - lineStart_) + 1; }
    bool Fail(const std::string& text) { return FailAt(line_, Col(), text); }
    bool FailAt(int line, int col, const std::string& text) {
        if (err_) *err_ = {line, col, text};
        return false;
    }
    void Advance(size_t n) { i_ += n; }
    bool End() const { return i_ >= s_.size(); }
    char Peek() const { return s_[i_]; }

    void Ws() {
        while (!End()) {
            const char c = Peek();
            if (c == '\n') {
                ++i_;
                ++line_;
                lineStart_ = i_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool Unexpected() {
        if (End()) return Fail("unexpected end of input");
        const unsigned char c = static_cast<unsigned char>(Peek());
        char b[48];
        if (c >= 0x20 && c < 0x7f)
            snprintf(b, sizeof b, "unexpected character '%c'", c);
        else
            snprintf(b, sizeof b, "unexpected byte 0x%02x", c);
        return Fail(b);
    }

    bool Literal(const char* word) {
        const std::string_view w(word);
        if (s_.substr(i_, w.size()) != w) return Unexpected();
        Advance(w.size());
        return true;
    }

    bool ParseValue(Value* v, int depth) {
        if (depth > kMaxDepth) return Fail("nesting deeper than " + std::to_string(kMaxDepth));
        if (End()) return Fail("unexpected end of input");
        *v = Value{};
        v->line = line_;
        v->col = Col();
        switch (Peek()) {
        case '{': return ParseObject(v, depth);
        case '[': return ParseArray(v, depth);
        case '"': v->type = Type::String; return ParseString(&v->string);
        case 't': v->type = Type::Bool; v->boolean = true; return Literal("true");
        case 'f': v->type = Type::Bool; return Literal("false");
        case 'n': return Literal("null");
        default:
            if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) return ParseNumber(v);
            return Unexpected();
        }
    }

    bool ParseObject(Value* v, int depth) {
        v->type = Type::Object;
        Advance(1);
        Ws();
        if (!End() && Peek() == '}') {
            Advance(1);
            return true;
        }
        for (;;) {
            if (End() || Peek() != '"') return End() ? Fail("unexpected end of input") : Fail("expected a string key");
            const int kl = line_, kc = Col();
            std::string key;
            if (!ParseString(&key)) return false;
            for (auto& m : v->members)
                if (m.first == key) return FailAt(kl, kc, "duplicate key \"" + key + "\"");
            Ws();
            if (End() || Peek() != ':') return End() ? Fail("unexpected end of input") : Fail("expected ':'");
            Advance(1);
            Ws();
            v->members.emplace_back(std::move(key), Value{});
            if (!ParseValue(&v->members.back().second, depth + 1)) return false;
            Ws();
            if (End()) return Fail("unexpected end of input");
            if (Peek() == ',') {
                Advance(1);
                Ws();
                continue;
            }
            if (Peek() == '}') {
                Advance(1);
                return true;
            }
            return Fail("expected ',' or '}'");
        }
    }

    bool ParseArray(Value* v, int depth) {
        v->type = Type::Array;
        Advance(1);
        Ws();
        if (!End() && Peek() == ']') {
            Advance(1);
            return true;
        }
        for (;;) {
            v->items.emplace_back();
            if (!ParseValue(&v->items.back(), depth + 1)) return false;
            Ws();
            if (End()) return Fail("unexpected end of input");
            if (Peek() == ',') {
                Advance(1);
                Ws();
                continue;
            }
            if (Peek() == ']') {
                Advance(1);
                return true;
            }
            return Fail("expected ',' or ']'");
        }
    }

    bool ParseNumber(Value* v) {
        const size_t start = i_;
        auto digits = [this] {
            size_t n = 0;
            while (!End() && Peek() >= '0' && Peek() <= '9') ++i_, ++n;
            return n;
        };
        if (Peek() == '-') Advance(1);
        if (End()) return Fail("unexpected end of input");
        if (Peek() == '0') {
            Advance(1);
            if (!End() && Peek() >= '0' && Peek() <= '9') return Fail("leading zeros are not allowed");
        } else if (!digits()) {
            return Unexpected();
        }
        if (!End() && Peek() == '.') {
            Advance(1);
            if (!digits()) return End() ? Fail("unexpected end of input") : Fail("expected a digit after '.'");
        }
        if (!End() && (Peek() == 'e' || Peek() == 'E')) {
            Advance(1);
            if (!End() && (Peek() == '+' || Peek() == '-')) Advance(1);
            if (!digits()) return End() ? Fail("unexpected end of input") : Fail("expected a digit in the exponent");
        }
        const char* b = s_.data() + start;
        const char* e = s_.data() + i_;
        auto [p, ec] = std::from_chars(b, e, v->number);
        if (ec != std::errc() || p != e || !std::isfinite(v->number)) {
            i_ = start;
            return Fail("number out of range");
        }
        v->type = Type::Number;
        return true;
    }

    static void Utf8(uint32_t cp, std::string* out) {
        if (cp < 0x80) {
            out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out->push_back(static_cast<char>(0xc0 | cp >> 6));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out->push_back(static_cast<char>(0xe0 | cp >> 12));
            out->push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            out->push_back(static_cast<char>(0xf0 | cp >> 18));
            out->push_back(static_cast<char>(0x80 | (cp >> 12 & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (cp >> 6 & 0x3f)));
            out->push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
    }

    bool Hex4(uint32_t* cp) {
        if (s_.size() - i_ < 4) return Fail("unexpected end of input");
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = Peek();
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return Fail("expected 4 hex digits after \\u");
            v = v << 4 | static_cast<uint32_t>(d);
            Advance(1);
        }
        *cp = v;
        return true;
    }

    // One UTF-8 sequence starting at i_; appends it and advances.
    bool Utf8Seq(std::string* out) {
        const unsigned char c = static_cast<unsigned char>(Peek());
        int n;
        uint32_t cp, min;
        if (c >= 0xc2 && c <= 0xdf) n = 1, cp = c & 0x1f, min = 0x80;
        else if (c >= 0xe0 && c <= 0xef) n = 2, cp = c & 0x0f, min = 0x800;
        else if (c >= 0xf0 && c <= 0xf4) n = 3, cp = c & 0x07, min = 0x10000;
        else return Fail("invalid UTF-8");
        if (s_.size() - i_ <= static_cast<size_t>(n)) return Fail("invalid UTF-8");
        for (int k = 1; k <= n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s_[i_ + k]);
            if ((d & 0xc0) != 0x80) return Fail("invalid UTF-8");
            cp = cp << 6 | (d & 0x3f);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return Fail("invalid UTF-8");
        out->append(s_.data() + i_, static_cast<size_t>(n) + 1);
        Advance(static_cast<size_t>(n) + 1);
        return true;
    }

    bool ParseString(std::string* out) {
        Advance(1);
        for (;;) {
            if (End()) return Fail("unterminated string");
            const unsigned char c = static_cast<unsigned char>(Peek());
            if (c == '"') {
                Advance(1);
                return true;
            }
            if (c < 0x20) return Fail(c == '\n' ? "unterminated string" : "control character in string");
            if (c >= 0x80) {
                if (!Utf8Seq(out)) return false;
                continue;
            }
            if (c != '\\') {
                out->push_back(static_cast<char>(c));
                Advance(1);
                continue;
            }
            Advance(1);
            if (End()) return Fail("unterminated string");
            const char e = Peek();
            Advance(1);
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t cp;
                if (!Hex4(&cp)) return false;
                if (cp >= 0xdc00 && cp <= 0xdfff) return Fail("unpaired surrogate in \\u escape");
                if (cp >= 0xd800 && cp <= 0xdbff) {
                    uint32_t lo;
                    if (s_.substr(i_, 2) != "\\u") return Fail("unpaired surrogate in \\u escape");
                    Advance(2);
                    if (!Hex4(&lo)) return false;
                    if (lo < 0xdc00 || lo > 0xdfff) return Fail("unpaired surrogate in \\u escape");
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                }
                Utf8(cp, out);
                break;
            }
            default:
                i_ -= 2;
                return Fail("invalid escape sequence");
            }
        }
    }
};
}  // namespace

bool Value::IsInteger() const {
    return type == Type::Number && std::floor(number) == number && std::fabs(number) <= 9007199254740992.0;
}

const Value* Value::Get(std::string_view key) const {
    if (type != Type::Object) return nullptr;
    for (auto& m : members)
        if (m.first == key) return &m.second;
    return nullptr;
}

const char* TypeName(Type t) {
    switch (t) {
    case Type::Null: return "null";
    case Type::Bool: return "boolean";
    case Type::Number: return "number";
    case Type::String: return "string";
    case Type::Array: return "array";
    case Type::Object: return "object";
    }
    return "?";
}

bool Parse(std::string_view text, Value* out, Error* err, size_t maxBytes) {
    if (err) *err = {};
    if (!out) return false;
    *out = Value{};
    if (text.size() > maxBytes) {
        if (err) *err = {1, 1, "input larger than " + std::to_string(maxBytes) + " bytes"};
        return false;
    }
    try {
        Parser p(text, err);
        if (p.Document(out)) return true;
    } catch (...) {
        if (err) err->text = "out of memory";
    }
    *out = Value{};
    return false;
}

bool ParseFile(const std::wstring& path, Value* out, Error* err, size_t maxBytes) {
    if (err) *err = {};
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (err) err->text = "cannot open the file";
        return false;
    }
    std::string data;
    bool tooBig = false;
    try {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            if (data.size() + n > maxBytes) {
                tooBig = true;
                break;
            }
            data.append(buf, n);
        }
    } catch (...) {
        fclose(f);
        if (err) err->text = "out of memory";
        return false;
    }
    fclose(f);
    if (tooBig) {
        if (err) *err = {1, 1, "file larger than " + std::to_string(maxBytes) + " bytes"};
        return false;
    }
    return Parse(data, out, err, maxBytes);
}
}  // namespace melange::json
