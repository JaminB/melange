#pragma once
// A small JSON reader for yt-dlp's -J output, which is far larger than a pipe line (a video's format list runs to
// hundreds of KB). Bounded: input size, nesting depth and value count are capped, and it never throws. The pipe uses
// pipe_codec.h's own stricter reader; this one only ever sees yt-dlp's stdout.
#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace holo::json {
enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };
struct Value {
    Type type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;
    const Value* Get(std::string_view k) const {
        if (type != Type::Object) return nullptr;
        for (const auto& [key, v] : members)
            if (key == k) return &v;
        return nullptr;
    }
    std::string Str(std::string_view k) const {
        const Value* v = Get(k);
        return v && v->type == Type::String ? v->s : std::string();
    }
    double Num(std::string_view k, double dflt = 0) const {
        const Value* v = Get(k);
        return v && v->type == Type::Number ? v->n : dflt;
    }
};

inline constexpr size_t kMaxInput = 64u << 20;
inline constexpr int kMaxDepth = 64;
inline constexpr size_t kMaxValues = 4u << 20;

class Reader {
public:
    explicit Reader(std::string_view t) : p_(t.data()), e_(t.data() + t.size()) {}
    bool Parse(Value* out) {
        if (static_cast<size_t>(e_ - p_) > kMaxInput) return false;
        Ws();
        if (!Val(out, 0)) return false;
        Ws();
        return p_ == e_;
    }

private:
    void Ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool Lit(std::string_view w) {
        if (static_cast<size_t>(e_ - p_) < w.size() || std::string_view(p_, w.size()) != w) return false;
        p_ += w.size();
        return true;
    }
    static void PutUtf8(std::string* s, uint32_t cp) {
        if (cp < 0x80) {
            s->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    bool Hex4(uint32_t* v) {
        if (e_ - p_ < 4) return false;
        *v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p_++;
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') *v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') *v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool Str(std::string* out) {
        ++p_;  // the opening quote
        while (p_ < e_) {
            const char c = *p_++;
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (p_ >= e_) return false;
            const char x = *p_++;
            switch (x) {
                case '"': case '\\': case '/': out->push_back(x); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(&cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {
                        uint32_t lo;
                        if (e_ - p_ < 6 || p_[0] != '\\' || p_[1] != 'u') return false;
                        p_ += 2;
                        if (!Hex4(&lo) || lo < 0xDC00 || lo >= 0xE000) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return false;
                    }
                    PutUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool Val(Value* v, int depth) {
        if (depth > kMaxDepth || ++count_ > kMaxValues || p_ >= e_) return false;
        const char c = *p_;
        if (c == '{' || c == '[') {
            const bool obj = c == '{';
            v->type = obj ? Type::Object : Type::Array;
            ++p_;
            Ws();
            if (p_ < e_ && *p_ == (obj ? '}' : ']')) {
                ++p_;
                return true;
            }
            for (;;) {
                Ws();
                if (obj) {
                    if (p_ >= e_ || *p_ != '"') return false;
                    std::string key;
                    if (!Str(&key)) return false;
                    Ws();
                    if (p_ >= e_ || *p_++ != ':') return false;
                    Ws();
                    v->members.emplace_back(std::move(key), Value{});
                    if (!Val(&v->members.back().second, depth + 1)) return false;
                } else {
                    v->items.emplace_back();
                    if (!Val(&v->items.back(), depth + 1)) return false;
                }
                Ws();
                if (p_ >= e_) return false;
                const char d = *p_++;
                if (d == ',') continue;
                return d == (obj ? '}' : ']');
            }
        }
        if (c == '"') {
            v->type = Type::String;
            return Str(&v->s);
        }
        if (Lit("true")) { v->type = Type::Bool; v->b = true; return true; }
        if (Lit("false")) { v->type = Type::Bool; return true; }
        if (Lit("null")) return true;
        if (Lit("NaN") || Lit("Infinity") || Lit("-Infinity")) return true;  // Python's json.dumps can emit these: null
        const char* s = p_;
        while (p_ < e_ && (std::string_view("+-0123456789.eE").find(*p_) != std::string_view::npos)) ++p_;
        if (s == p_) return false;
        v->type = Type::Number;
        const auto r = std::from_chars(s, p_, v->n);
        return r.ec == std::errc() && r.ptr == p_;
    }

    const char* p_;
    const char* e_;
    size_t count_ = 0;
};
inline bool Parse(std::string_view text, Value* out) { return Reader(text).Parse(out); }
}  // namespace holo::json
