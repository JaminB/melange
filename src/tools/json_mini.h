#pragma once
// Minimal JSON object/array builder.
#include <cstdio>
#include <string>
#include <string_view>

namespace melange::jsonmini {

inline std::string Escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

class Obj {
public:
    Obj() { s_ = "{"; }
    Obj& Str(std::string_view k, std::string_view v) {
        Sep();
        s_ += '"';
        s_ += Escape(k);
        s_ += "\":\"";
        s_ += Escape(v);
        s_ += '"';
        return *this;
    }
    Obj& Int(std::string_view k, long long v) {
        Sep();
        s_ += '"';
        s_ += Escape(k);
        s_ += "\":";
        s_ += std::to_string(v);
        return *this;
    }
    Obj& UInt(std::string_view k, unsigned long long v) {
        Sep();
        s_ += '"';
        s_ += Escape(k);
        s_ += "\":";
        s_ += std::to_string(v);
        return *this;
    }
    Obj& Bool(std::string_view k, bool v) {
        Sep();
        s_ += '"';
        s_ += Escape(k);
        s_ += "\":";
        s_ += v ? "true" : "false";
        return *this;
    }
    // v is already JSON; inserted verbatim.
    Obj& Raw(std::string_view k, std::string_view v) {
        Sep();
        s_ += '"';
        s_ += Escape(k);
        s_ += "\":";
        s_ += v;
        return *this;
    }
    std::string End() const { return s_ + "}"; }

private:
    void Sep() {
        if (!first_) s_ += ',';
        first_ = false;
    }
    std::string s_;
    bool first_ = true;
};

class Arr {
public:
    Arr() { s_ = "["; }
    Arr& Str(std::string_view v) {
        Sep();
        s_ += '"';
        s_ += Escape(v);
        s_ += '"';
        return *this;
    }
    Arr& Raw(std::string_view v) {
        Sep();
        s_ += v;
        return *this;
    }
    bool Empty() const { return first_; }
    std::string End() const { return s_ + "]"; }

private:
    void Sep() {
        if (!first_) s_ += ',';
        first_ = false;
    }
    std::string s_;
    bool first_ = true;
};

}  // namespace melange::jsonmini
