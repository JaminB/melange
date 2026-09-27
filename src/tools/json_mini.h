#pragma once
// Minimal JSON object/array builder shared by log_export.cpp and sysinfo.cpp. Not a frozen sdk header:
// it is a private helper local to src/tools/, not included by any other component.
#include <cstdio>
#include <string>
#include <string_view>

namespace wf::jsonmini {

// Escapes a string for use inside a JSON string literal (quotes not included).
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

// Builds one JSON object. Values that are themselves JSON (nested objects/arrays already serialized by a
// nested Obj/Arr) are added with Raw(); everything else is escaped and quoted as needed.
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
    // v already fully-formed JSON (object, array, string, number, null): inserted verbatim.
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
    // v already fully-formed JSON.
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

}  // namespace wf::jsonmini
