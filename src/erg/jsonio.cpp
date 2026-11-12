#include "erg/jsonio.h"

#include <cmath>
#include <cstdio>
#include <tuple>
#include <utility>

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

bool GetCorners(const Json& o, const std::string& path, const std::array<int, 3>& size, std::vector<double>* out, std::string* err) {
    out->clear();
    const Json* v = o.find("corners");
    if (!v || v->kind == Json::Kind::Null) return true;
    const size_t n = static_cast<size_t>(size[0] + 1) * (size[2] + 1);
    if (v->kind != Json::Kind::Array || v->arr.size() != n)
        return Fail(err, path + ".corners", "must be (X+1)*(Z+1) = " + std::to_string(n) + " numbers");
    out->reserve(n);
    for (size_t i = 0; i < n; ++i) {
        double d = 0;
        if (!GetNumber(v->arr[i], path + ".corners[" + std::to_string(i) + "]", -kMaxCornerOffset, kMaxCornerOffset, &d, err)) return false;
        out->push_back(d);
    }
    return true;
}

Json CornersJson(const std::vector<double>& corners) {
    Json a = Json::Arr();
    for (double c : corners) a.arr.push_back(Num(c));
    return a;
}

bool ParseTrim(const Json& o, const std::string& path, FrameTrim* out, std::string* err) {
    FrameTrim t;
    if (!GetInt(o, "frame", path, 1, 1 << 24, &t.frame, err)) return false;
    for (auto [key, dst, lo] : {std::tuple{"min", &t.min, 0}, std::tuple{"size", &t.size, 1}}) {
        const Json* v = o.find(key);
        if (!v || v->kind != Json::Kind::Array || v->arr.size() != 3) return Fail(err, path + "." + key, "must be 3 integers");
        for (int k = 0; k < 3; ++k) {
            bool ok = false;
            const int64_t n = v->arr[k].kind == Json::Kind::Number ? v->arr[k].asInt64(&ok) : -1;
            if (!ok || n < lo || n > 255) return Fail(err, path + "." + key, "each must be an integer in " + std::to_string(lo) + "..255");
            (*dst)[k] = static_cast<int>(n);
        }
    }
    *out = t;
    return true;
}

void TrimFields(const FrameTrim& t, Json* o) {
    o->set("frame", Int(t.frame));
    for (auto [key, v] : {std::pair{"min", &t.min}, std::pair{"size", &t.size}}) {
        Json a = Json::Arr();
        for (int n : *v) a.arr.push_back(Int(n));
        o->set(key, std::move(a));
    }
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

bool ParseSpawns(const Json* o, SpawnMode* mode, SpawnAreas* areas, std::string* err) {
    std::string m;
    if (!Obj(o, "spawns", err) || !OnlyKeys(*o, {"mode", "areas", "pattern", "opposite"}, "spawns", err) ||
        !GetString(*o, "mode", "spawns", &m, err))
        return false;
    if (!ParseSpawnMode(m, mode)) return Fail(err, "spawns.mode", "must be \"random\", \"knots\", \"teams\" or \"areas\"");
    *areas = {};
    if (*mode != SpawnMode::Areas) {
        for (const char* k : {"areas", "pattern", "opposite"})
            if (o->find(k)) return Fail(err, std::string("spawns.") + k, "is only for spawns.mode \"areas\"");
        return true;
    }
    const Json* arr = o->find("areas");
    if (!arr || arr->kind != Json::Kind::Array || arr->arr.size() > kMaxSpawnAreas)
        return Fail(err, "spawns.areas", "must be an array of 1-" + std::to_string(kMaxSpawnAreas) + " areas");
    for (size_t i = 0; i < arr->arr.size(); ++i) {
        const std::string p = "spawns.areas[" + std::to_string(i) + "]";
        const Json& a = arr->arr[i];
        SpawnArea ar;
        int64_t knots = 0;
        if (!Obj(&a, p, err) || !OnlyKeys(a, {"name", "group", "knots"}, p, err) || !GetString(a, "name", p, &ar.name, err) ||
            !GetString(a, "group", p, &ar.group, err) || !GetInt(a, "knots", p, 1, kMaxAreaKnots, &knots, err))
            return false;
        ar.knots = static_cast<int>(knots);
        areas->areas.push_back(std::move(ar));
    }
    if (const Json* pat = o->find("pattern")) {
        if (pat->kind != Json::Kind::Array || pat->arr.size() > kMaxDealSteps)
            return Fail(err, "spawns.pattern", "must be an array of at most " + std::to_string(kMaxDealSteps) + " steps");
        for (size_t i = 0; i < pat->arr.size(); ++i) {
            const Json& v = pat->arr[i];
            const std::string p = "spawns.pattern[" + std::to_string(i) + "]";
            if (v.kind != Json::Kind::String) return Fail(err, p, "must be a string");
            DealStep st;
            std::string_view g = v.str;
            constexpr std::string_view kOpp = ":opposite";
            if (g.size() > kOpp.size() && g.ends_with(kOpp)) {
                st.opposite = true;
                g.remove_suffix(kOpp.size());
            }
            if (!ValidGroupName(g)) return Fail(err, p, "must be \"<group>\" or \"<group>:opposite\"");
            st.group = std::string(g);
            areas->pattern.push_back(std::move(st));
        }
    }
    if (const Json* opp = o->find("opposite")) {
        if (opp->kind != Json::Kind::Array || opp->arr.size() > kMaxSpawnAreas)
            return Fail(err, "spawns.opposite", "must be an array of at most " + std::to_string(kMaxSpawnAreas) + " pairs");
        for (size_t i = 0; i < opp->arr.size(); ++i) {
            const Json& v = opp->arr[i];
            const std::string p = "spawns.opposite[" + std::to_string(i) + "]";
            if (v.kind != Json::Kind::Array || v.arr.size() != 2) return Fail(err, p, "must be [a, b], two area indexes");
            std::array<int, 2> pair{};
            for (int k = 0; k < 2; ++k) {
                bool ok = false;
                const int64_t n = v.arr[k].kind == Json::Kind::Number ? v.arr[k].asInt64(&ok) : -1;
                if (!ok || n < 0 || n >= static_cast<int64_t>(kMaxSpawnAreas)) return Fail(err, p, "must be [a, b], two area indexes");
                pair[k] = static_cast<int>(n);
            }
            areas->opposite.push_back(pair);
        }
    }
    return ValidateSpawnAreas(*areas, err);
}

Json SpawnsJson(SpawnMode mode, const SpawnAreas& areas) {
    Json sp = Json::Obj();
    sp.set("mode", Str(SpawnModeName(mode)));
    if (mode != SpawnMode::Areas) return sp;
    Json arr = Json::Arr();
    for (const auto& a : areas.areas) {
        Json o = Json::Obj();
        o.set("name", Str(a.name));
        o.set("group", Str(a.group));
        o.set("knots", Int(a.knots));
        arr.arr.push_back(std::move(o));
    }
    sp.set("areas", std::move(arr));
    if (!areas.pattern.empty()) {
        Json pat = Json::Arr();
        for (const auto& st : areas.pattern) pat.arr.push_back(Str(st.opposite ? st.group + ":opposite" : st.group));
        sp.set("pattern", std::move(pat));
    }
    if (!areas.opposite.empty()) {
        Json opp = Json::Arr();
        for (const auto& [x, y] : areas.opposite) {
            Json pr = Json::Arr();
            pr.arr.push_back(Int(x));
            pr.arr.push_back(Int(y));
            opp.arr.push_back(std::move(pr));
        }
        sp.set("opposite", std::move(opp));
    }
    return sp;
}

bool IsHex64(std::string_view s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
}  // namespace melange::erg::jsonio
