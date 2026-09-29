#include "erg/jsonio.h"

#include <cmath>
#include <cstdio>

namespace melange::erg::jsonio {
namespace {
void Escape(std::string_view s, std::string& out) {
    out.push_back('"');
    for (unsigned char c : s) {
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
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    out.push_back('"');
}

void Write(const Json& v, std::string& out) {
    switch (v.kind) {
        case Json::Kind::Null: out += "null"; break;
        case Json::Kind::Bool: out += v.boolean ? "true" : "false"; break;
        case Json::Kind::Number: out += v.numLiteral; break;
        case Json::Kind::String: Escape(v.str, out); break;
        case Json::Kind::Array:
            out.push_back('[');
            for (size_t i = 0; i < v.arr.size(); ++i) {
                if (i) out.push_back(',');
                Write(v.arr[i], out);
            }
            out.push_back(']');
            break;
        case Json::Kind::Object:
            out.push_back('{');
            for (size_t i = 0; i < v.obj.size(); ++i) {
                if (i) out.push_back(',');
                Escape(v.obj[i].first, out);
                out.push_back(':');
                Write(v.obj[i].second, out);
            }
            out.push_back('}');
            break;
    }
}

bool Fail(std::string* err, const std::string& path, const std::string& why) {
    if (err) *err = path + ": " + why;
    return false;
}
}  // namespace

std::string Compact(const Json& v) {
    std::string out;
    Write(v, out);
    return out;
}

Json Num(double v) {
    if (!std::isfinite(v)) v = 0;
    if (v == 0) return Json::Int(0);
    if (std::fabs(v) < 1e15 && std::floor(v) == v) return Json::Int(static_cast<int64_t>(v));
    return Json::Num(v);
}

Json Int(int64_t v) { return Json::Int(v); }

Json Vec(const Vec3& v) {
    Json a = Json::Arr();
    for (double c : v) a.arr.push_back(Num(c));
    return a;
}

Json Str(std::string_view s) { return Json::Str(std::string(s)); }

bool Obj(const Json* v, const std::string& path, std::string* err) {
    if (!v || v->kind != Json::Kind::Object) return Fail(err, path, "must be an object");
    return true;
}

bool GetString(const Json& o, const char* key, const std::string& path, std::string* out, std::string* err,
               bool required) {
    const Json* v = o.find(key);
    if (!v) return !required || Fail(err, path + "." + key, "is required");
    if (v->kind != Json::Kind::String) return Fail(err, path + "." + key, "must be a string");
    *out = v->str;
    return true;
}

bool GetInt(const Json& o, const char* key, const std::string& path, int64_t lo, int64_t hi, int64_t* out,
            std::string* err, bool required) {
    const Json* v = o.find(key);
    if (!v) return !required || Fail(err, path + "." + key, "is required");
    bool ok = false;
    const int64_t n = v->kind == Json::Kind::Number ? v->asInt64(&ok) : 0;
    if (!ok || n < lo || n > hi)
        return Fail(err, path + "." + key, "must be an integer in " + std::to_string(lo) + ".." + std::to_string(hi));
    *out = n;
    return true;
}

bool GetNumber(const Json& v, const std::string& path, double lo, double hi, double* out, std::string* err) {
    bool ok = false;
    const double d = v.kind == Json::Kind::Number ? v.asDouble(&ok) : 0;
    if (!ok || !std::isfinite(d) || d < lo || d > hi) return Fail(err, path, "must be a finite number in range");
    *out = d;
    return true;
}

bool GetVec(const Json& o, const char* key, const std::string& path, Vec3* out, std::string* err, bool required,
            double limit) {
    const Json* v = o.find(key);
    if (!v) return !required || Fail(err, path + "." + key, "is required");
    if (v->kind != Json::Kind::Array || v->arr.size() != 3) return Fail(err, path + "." + key, "must be 3 numbers");
    for (int i = 0; i < 3; ++i)
        if (!GetNumber(v->arr[i], path + "." + key, -limit, limit, &(*out)[i], err)) return false;
    return true;
}

bool OnlyKeys(const Json& o, std::initializer_list<std::string_view> keys, const std::string& path, std::string* err) {
    for (const auto& [k, v] : o.obj) {
        bool known = false;
        for (auto kk : keys) known |= kk == k;
        if (!known) return Fail(err, path, "unknown key '" + k + "'");
    }
    return true;
}

bool IsHex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
}  // namespace melange::erg::jsonio
