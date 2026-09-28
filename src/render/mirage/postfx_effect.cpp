// effect.ini parser, GLSL source assembly and the [MiragePostFX] value formats. No GL here.
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "render/mirage/postfx_internal.h"

namespace melange::mirage::postfx {
namespace {
bool Space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && Space(s.front())) s.remove_prefix(1);
    while (!s.empty() && Space(s.back())) s.remove_suffix(1);
    return s;
}

// "value ; comment" -> "value"; a ';' only starts a comment at the line start or after whitespace
std::string_view StripComment(std::string_view v) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i] == ';' && (i == 0 || v[i - 1] == ' ' || v[i - 1] == '\t')) return v.substr(0, i);
    return v;
}

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return r;
}

bool IEq(std::string_view a, std::string_view b) {
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

// Rejects a relative path that could climb out of the effect's own folder (mirrors the #include check below);
// used for every effect.ini value that is later joined onto e.dir and opened (shader=, file=, #include "...").
bool StaysInFolder(std::string_view name) {
    return !name.empty() && name.find("..") == std::string_view::npos && name.find(':') == std::string_view::npos &&
           name.front() != '/' && name.front() != '\\';
}

bool IsIdent(std::string_view s) {
    if (s.empty() || isdigit(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

std::vector<std::string_view> Split(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t p = 0;
    while (p <= s.size()) {
        size_t q = s.find(sep, p);
        if (q == std::string_view::npos) q = s.size();
        std::string_view item = Trim(s.substr(p, q - p));
        if (!item.empty()) out.push_back(item);
        p = q + 1;
    }
    return out;
}

bool ToFloat(std::string_view s, float* v) {
    s = Trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    auto r = std::from_chars(s.data(), s.data() + s.size(), *v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() && std::isfinite(*v);
}

bool ToInt(std::string_view s, int* v) {
    s = Trim(s);
    if (!s.empty() && s.front() == '+') s.remove_prefix(1);
    auto r = std::from_chars(s.data(), s.data() + s.size(), *v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

int TypeCount(ParamType t) {
    switch (t) {
    case ParamType::Vec2: return 2;
    case ParamType::Vec3: return 3;
    case ParamType::Color: return 3;
    default: return 1;
    }
}

struct Parser {
    EffectDesc* d;
    std::string* err;
    int line = 0;
    enum class Sec { None, Effect, Param, Texture, Pass } sec = Sec::None;
    int index = -1;
    bool sawEffect = false, sawDefault = false;
    std::vector<std::string> rawInputs;  // per pass, resolved at the end
    std::vector<int> inputLines;

    bool Fail(const std::string& what) {
        if (err) {
            char b[32];
            snprintf(b, sizeof b, "effect.ini(%d): ", line);
            *err = b + what;
        }
        return false;
    }

    bool EndParam() {
        if (sec != Sec::Param) return true;
        ParamDesc& p = d->params[index];
        if (!sawDefault) {
            for (int i = 0; i < 4; ++i) p.def[i] = p.type == ParamType::Color ? 1.f : 0.f;
            if (p.type == ParamType::Color) p.n = 3;
        }
        if (p.hasRange && p.min > p.max) return Fail("min > max in [param." + p.name + "]");
        return true;
    }

    bool Section(std::string_view s) {
        if (!EndParam()) return false;
        std::string l = Lower(s);
        size_t dot = l.find('.');
        std::string kind = l.substr(0, dot), name = dot == std::string::npos ? "" : std::string(s.substr(dot + 1));
        if (kind == "effect" && dot == std::string::npos) {
            if (sawEffect) return Fail("duplicate [effect]");
            sawEffect = true;
            sec = Sec::Effect;
            return true;
        }
        if (dot == std::string::npos || !IsIdent(name)) return Fail("bad section [" + std::string(s) + "]");
        auto dup = [&](auto& v) {
            for (auto& x : v)
                if (IEq(x.name, name)) return true;
            return false;
        };
        if (kind == "param") {
            if (dup(d->params)) return Fail("duplicate [param." + name + "]");
            d->params.push_back({});
            d->params.back().name = name;
            d->params.back().label = name;
            sec = Sec::Param;
            index = static_cast<int>(d->params.size()) - 1;
            sawDefault = false;
            return true;
        }
        if (kind == "texture") {
            if (dup(d->textures)) return Fail("duplicate [texture." + name + "]");
            d->textures.push_back({});
            d->textures.back().name = name;
            sec = Sec::Texture;
            index = static_cast<int>(d->textures.size()) - 1;
            return true;
        }
        if (kind == "pass") {
            if (dup(d->passes)) return Fail("duplicate [pass." + name + "]");
            d->passes.push_back({});
            d->passes.back().name = name;
            rawInputs.push_back("prev");
            inputLines.push_back(line);
            sec = Sec::Pass;
            index = static_cast<int>(d->passes.size()) - 1;
            return true;
        }
        return Fail("unknown section [" + std::string(s) + "]");
    }

    bool Key(std::string_view key, std::string_view v) {
        std::string k = Lower(key);
        auto unknown = [&] { return Fail("unknown key '" + std::string(key) + "'"); };
        switch (sec) {
        case Sec::None: return Fail("key outside a section");
        case Sec::Effect:
            if (k == "title") d->title = v;
            else if (k == "stage") {
                if (IEq(v, "PostWorld")) d->stage = Stage::PostWorld;
                else if (IEq(v, "Final")) d->stage = Stage::Final;
                else return Fail("stage must be PostWorld or Final");
            } else if (k == "order") {
                if (!ToInt(v, &d->order)) return Fail("order must be an integer");
            } else if (k == "enabled") {
                if (v != "0" && v != "1") return Fail("enabled must be 0 or 1");
                d->enabled = v == "1";
            } else return unknown();
            return true;
        case Sec::Param: {
            ParamDesc& p = d->params[index];
            if (k == "type") {
                static const struct { const char* n; ParamType t; } kTypes[] = {
                    {"float", ParamType::Float}, {"vec2", ParamType::Vec2}, {"vec3", ParamType::Vec3},
                    {"color", ParamType::Color}, {"int", ParamType::Int},   {"bool", ParamType::Bool}};
                bool found = false;
                for (auto& t : kTypes)
                    if (IEq(v, t.n)) {
                        p.type = t.t;
                        p.n = TypeCount(t.t);
                        found = true;
                    }
                if (!found) return Fail("type must be float, vec2, vec3, color, int or bool");
                if (sawDefault) return Fail("type must come before default");
                if (p.type == ParamType::Bool) {
                    p.min = 0;
                    p.max = 1;
                }
            } else if (k == "default") {
                auto parts = Split(v, ',');
                int want = p.n;
                bool colorOk = p.type == ParamType::Color && (parts.size() == 3 || parts.size() == 4);
                if (!colorOk && static_cast<int>(parts.size()) != want) {
                    char b[64];
                    snprintf(b, sizeof b, "default needs %d value%s", want, want == 1 ? "" : "s");
                    return Fail(b);
                }
                if (colorOk) p.n = static_cast<int>(parts.size());
                for (size_t i = 0; i < parts.size(); ++i)
                    if (!ToFloat(parts[i], &p.def[i])) return Fail("bad number '" + std::string(parts[i]) + "'");
                sawDefault = true;
            } else if (k == "min" || k == "max") {
                float f = 0;
                if (!ToFloat(v, &f)) return Fail(k + " must be a number");
                (k == "min" ? p.min : p.max) = f;
                p.hasRange = true;
            } else if (k == "label") p.label = v;
            else return unknown();
            return true;
        }
        case Sec::Texture: {
            TextureDesc& t = d->textures[index];
            if (k == "file") t.file = v;
            else if (k == "filter") {
                if (IEq(v, "linear")) t.linear = true;
                else if (IEq(v, "nearest")) t.linear = false;
                else return Fail("filter must be linear or nearest");
            } else if (k == "wrap") {
                if (IEq(v, "clamp")) t.repeat = false;
                else if (IEq(v, "repeat")) t.repeat = true;
                else return Fail("wrap must be clamp or repeat");
            } else return unknown();
            return true;
        }
        case Sec::Pass: {
            PassDesc& p = d->passes[index];
            if (k == "shader") p.shader = v;
            else if (k == "scale") {
                if (!ToFloat(v, &p.scale) || p.scale <= 0.f || p.scale > 4.f) return Fail("scale must be in (0, 4]");
            } else if (k == "format") {
                static const struct { const char* n; Format f; } kFormats[] = {
                    {"rgba8", Format::Rgba8}, {"rgba16f", Format::Rgba16f}, {"r8", Format::R8}, {"rg8", Format::Rg8}};
                bool found = false;
                for (auto& f : kFormats)
                    if (IEq(v, f.n)) {
                        p.format = f.f;
                        found = true;
                    }
                if (!found) return Fail("format must be rgba8, rgba16f, r8 or rg8");
            } else if (k == "inputs") {
                rawInputs[index] = v;
                inputLines[index] = line;
            } else if (k == "defines") {
                for (std::string_view def : Split(v, ',')) {
                    size_t eq = def.find('=');
                    std::string_view name = Trim(def.substr(0, eq));
                    if (!IsIdent(name)) return Fail("bad define '" + std::string(def) + "'");
                    std::string out(name);
                    if (eq != std::string_view::npos) out += " " + std::string(Trim(def.substr(eq + 1)));
                    p.defines.push_back(out);
                }
            } else return unknown();
            return true;
        }
        }
        return unknown();
    }

    bool Finish() {
        if (!EndParam()) return false;
        if (!sawEffect) return Fail("missing [effect]");
        if (d->passes.empty()) return Fail("no [pass.*] section");
        for (size_t i = 0; i < d->passes.size(); ++i) {
            PassDesc& p = d->passes[i];
            line = inputLines[i];
            if (p.shader.empty()) return Fail("[pass." + p.name + "] has no shader=");
            if (!StaysInFolder(p.shader)) return Fail("[pass." + p.name + "] shader= must stay in the effect folder");
            for (std::string_view item : Split(rawInputs[i], ',')) {
                Input in;
                std::string l = Lower(item);
                if (l == "scene") in.kind = InputKind::Scene;
                else if (l == "depth") in.kind = InputKind::Depth;
                else if (l == "prev") in.kind = InputKind::Prev;
                else if (l.rfind("pass.", 0) == 0) {
                    in.kind = InputKind::Pass;
                    in.name = item.substr(5);
                    bool earlier = false;
                    for (size_t j = 0; j < i; ++j)
                        if (IEq(d->passes[j].name, in.name)) {
                            in.name = d->passes[j].name;
                            earlier = true;
                        }
                    if (!earlier) return Fail("input '" + std::string(item) + "' is not an earlier pass");
                } else if (l.rfind("texture.", 0) == 0) {
                    in.kind = InputKind::Texture;
                    in.name = item.substr(8);
                    bool found = false;
                    for (const TextureDesc& t : d->textures)
                        if (IEq(t.name, in.name)) {
                            in.name = t.name;
                            found = true;
                        }
                    if (!found) return Fail("input '" + std::string(item) + "' names no [texture.*]");
                } else return Fail("unknown input '" + std::string(item) + "'");
                bool dup = false;
                for (const Input& x : p.inputs) dup |= x.kind == in.kind && x.name == in.name;
                if (!dup) p.inputs.push_back(in);
            }
        }
        for (const TextureDesc& t : d->textures) {
            if (t.file.empty()) return Fail("[texture." + t.name + "] has no file=");
            if (!StaysInFolder(t.file)) return Fail("[texture." + t.name + "] file= must stay in the effect folder");
        }
        if (d->title.empty()) d->title = "(untitled)";
        return true;
    }
};

// Skips whitespace and comments; returns the offset of the first token.
size_t FirstToken(std::string_view s, size_t p) {
    while (p < s.size()) {
        if (isspace(static_cast<unsigned char>(s[p]))) ++p;
        else if (s.compare(p, 2, "//") == 0) {
            while (p < s.size() && s[p] != '\n') ++p;
        } else if (s.compare(p, 2, "/*") == 0) {
            size_t e = s.find("*/", p + 2);
            p = e == std::string_view::npos ? s.size() : e + 2;
        } else break;
    }
    return p;
}

// GLSL < 330: "#line N" makes the next line N+1; 330+: the next line is N.
std::string LineDirective(int version, int nextLine, int source) {
    char b[48];
    snprintf(b, sizeof b, "#line %d %d\n", version >= 330 ? nextLine : nextLine - 1, source);
    return b;
}

bool Expand(std::string_view text, int source, int version, int depth, IncludeFn include, void* user, Source* out,
            std::string* err) {
    int lineNo = 0;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        size_t next = e == std::string_view::npos ? text.size() : e + 1;
        std::string_view ln = text.substr(p, next - p);
        ++lineNo;
        p = next;
        std::string_view t = Trim(ln);
        if (t.size() > 1 && t[0] == '#') {
            std::string_view d = Trim(t.substr(1));
            if (d.rfind("include", 0) == 0 && d.size() > 7 && (d[7] == ' ' || d[7] == '\t' || d[7] == '"')) {
                std::string_view arg = Trim(d.substr(7));
                if (arg.size() < 3 || arg.front() != '"' || arg.back() != '"') {
                    *err = out->files[source] + "(" + std::to_string(lineNo) + "): #include needs \"file\"";
                    return false;
                }
                std::string name(arg.substr(1, arg.size() - 2));
                if (depth >= 4) {
                    *err = out->files[source] + "(" + std::to_string(lineNo) + "): #include nested too deep";
                    return false;
                }
                if (!StaysInFolder(name)) {
                    *err = out->files[source] + "(" + std::to_string(lineNo) + "): #include must stay in the effect folder";
                    return false;
                }
                std::string inc;
                if (!include || !include(name, &inc, user)) {
                    *err = out->files[source] + "(" + std::to_string(lineNo) + "): cannot open \"" + name + "\"";
                    return false;
                }
                int id = static_cast<int>(out->files.size());
                out->files.push_back(name);
                out->text += LineDirective(version, 1, id);
                if (!Expand(inc, id, version, depth + 1, include, user, out, err)) return false;
                if (!out->text.empty() && out->text.back() != '\n') out->text += '\n';
                out->text += LineDirective(version, lineNo + 1, source);
                continue;
            }
        }
        out->text.append(ln);
    }
    return true;
}
}  // namespace

bool ParseEffect(std::string_view text, EffectDesc* out, std::string* error) {
    *out = EffectDesc{};
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.remove_prefix(3);
    Parser ps{out, error};
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string_view::npos) e = text.size();
        std::string_view ln = Trim(text.substr(p, e - p));
        p = e + 1;
        ++ps.line;
        if (ln.empty() || ln[0] == ';' || ln[0] == '#') continue;
        if (ln[0] == '[') {
            ln = Trim(StripComment(ln));
            if (ln.back() != ']') return ps.Fail("unterminated section header");
            if (!ps.Section(Trim(ln.substr(1, ln.size() - 2)))) return false;
            continue;
        }
        size_t eq = ln.find('=');
        if (eq == std::string_view::npos) return ps.Fail("expected key=value");
        if (!ps.Key(Trim(ln.substr(0, eq)), Trim(StripComment(ln.substr(eq + 1))))) return false;
    }
    return ps.Finish();
}

std::string SamplerName(const Input& in) {
    switch (in.kind) {
    case InputKind::Scene: return "mg_scene";
    case InputKind::Depth: return "mg_depth";
    case InputKind::Prev: return "mg_prev";
    case InputKind::Pass: return "mg_pass_" + in.name;
    case InputKind::Texture: return "t_" + in.name;
    }
    return {};
}

const char* FormatName(Format f) {
    switch (f) {
    case Format::Rgba8: return "rgba8";
    case Format::Rgba16f: return "rgba16f";
    case Format::R8: return "r8";
    case Format::Rg8: return "rg8";
    }
    return "?";
}

const char* StageName(Stage s) {
    switch (s) {
    case Stage::World: return "World";
    case Stage::WorldLate: return "WorldLate";
    case Stage::PostWorld: return "PostWorld";
    case Stage::Hud: return "Hud";
    case Stage::Final: return "Final";
    default: return "?";
    }
}

bool BuildFragment(std::string_view text, const std::string& fileName, const std::vector<std::string>& defines,
                   IncludeFn include, void* user, Source* out, std::string* error) {
    *out = Source{};
    std::string dummy;
    std::string* err = error ? error : &dummy;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF) text.remove_prefix(3);
    std::string body(text);
    size_t tok = FirstToken(body, 0);
    if (tok < body.size() && body[tok] == '#') {
        size_t d = tok + 1;
        while (d < body.size() && (body[d] == ' ' || body[d] == '\t')) ++d;
        if (body.compare(d, 7, "version") == 0) {
            size_t e = body.find('\n', d);
            if (e == std::string::npos) e = body.size();
            std::string_view args = Trim(std::string_view(body).substr(d + 7, e - d - 7));
            auto words = Split(args, ' ');
            if (words.empty() || !ToInt(words[0], &out->version) || out->version < 110 || out->version > 460) {
                *err = fileName + ": bad #version line";
                return false;
            }
            if (words.size() > 1) {
                out->profile = Lower(words[1]);
                if (out->profile != "core" && out->profile != "compatibility") {
                    *err = fileName + ": only desktop GLSL profiles (core, compatibility) are supported";
                    return false;
                }
            }
            for (size_t i = tok; i < e; ++i) body[i] = ' ';
        }
    }
    out->files.push_back(fileName);
    out->text = "#version " + std::to_string(out->version) + (out->profile.empty() ? "" : " " + out->profile) + "\n";
    for (const std::string& d : defines) out->text += "#define " + d + "\n";
    out->text += LineDirective(out->version, 1, 0);
    return Expand(body, 0, out->version, 0, include, user, out, err);
}

std::string BuildVertex(int version, const std::string& profile) {
    std::string s = "#version " + std::to_string(version) + (profile.empty() ? "" : " " + profile) + "\n";
    if (version >= 130)
        s += "in vec2 mg_pos;\nout vec2 mg_uv;\n";
    else
        s += "attribute vec2 mg_pos;\nvarying vec2 mg_uv;\n";
    s += "void main() {\n    mg_uv = mg_pos * 0.5 + 0.5;\n    gl_Position = vec4(mg_pos, 0.0, 1.0);\n}\n";
    return s;
}

int ParseGlslVersion(const char* s) {
    if (!s) return 0;
    while (*s && !isdigit(static_cast<unsigned char>(*s))) ++s;
    int major = 0, minor = 0, digits = 0;
    while (isdigit(static_cast<unsigned char>(*s))) major = major * 10 + (*s++ - '0');
    if (*s != '.') return 0;
    ++s;
    while (isdigit(static_cast<unsigned char>(*s)) && digits < 2) {
        minor = minor * 10 + (*s++ - '0');
        ++digits;
    }
    if (digits == 0) return 0;
    if (digits == 1) minor *= 10;
    return major * 100 + minor;
}

// Effect ids are "<owner>/<folder>", and both come from user-chosen Windows folder names that may contain ',' or
// ';' (both valid in a folder name, but ',' is our field separator and "; " starts a comment in ReadStack; ':' is
// NOT escaped, since ParseStack already tolerates it in an id by taking only the last two colons as separators,
// and Windows folder names cannot contain one anyway). Escape them (and '%' itself) so an id with either round-
// trips through the Stack= value unchanged.
std::string EscapeStackId(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (unsigned char c : id) {
        if (c == '%' || c == ',' || c == ';') {
            char b[4];
            snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

std::string UnescapeStackId(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<StackEntry> ParseStack(std::string_view s) {
    std::vector<StackEntry> out;
    for (std::string_view item : Split(s, ',')) {
        size_t b = item.rfind(':');
        if (b == std::string_view::npos || b == 0) continue;
        size_t a = item.rfind(':', b - 1);
        if (a == std::string_view::npos || a == 0) continue;
        StackEntry e;
        std::string_view en = Trim(item.substr(b + 1));
        if (!ToInt(item.substr(a + 1, b - a - 1), &e.order) || (en != "0" && en != "1")) continue;
        e.enabled = en == "1";
        e.id = UnescapeStackId(Trim(item.substr(0, a)));
        bool dup = false;
        for (StackEntry& x : out)
            if (x.id == e.id) {
                x = e;
                dup = true;
            }
        if (!dup) out.push_back(e);
    }
    return out;
}

std::string FormatStack(const std::vector<StackEntry>& v) {
    std::string s;
    for (const StackEntry& e : v) {
        if (!s.empty()) s += ',';
        s += EscapeStackId(e.id) + ":" + std::to_string(e.order) + ":" + (e.enabled ? "1" : "0");
    }
    return s;
}

int ParseFloats(std::string_view s, float* v, int max) {
    int n = 0;
    for (std::string_view item : Split(s, ',')) {
        if (n >= max || !ToFloat(item, &v[n])) break;
        ++n;
    }
    return n;
}

std::string FormatFloats(const float* v, int n) {
    std::string s;
    for (int i = 0; i < n; ++i) {
        char b[32];
        auto r = std::to_chars(b, b + sizeof b, v[i]);
        if (i) s += ',';
        s.append(b, r.ptr);
    }
    return s;
}

bool Invert4(const float* m, float* out) {
    double inv[16], a[16];
    for (int i = 0; i < 16; ++i) a[i] = m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (std::fabs(det) < 1e-30) return false;
    for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(inv[i] / det);
    return true;
}
}  // namespace melange::mirage::postfx
