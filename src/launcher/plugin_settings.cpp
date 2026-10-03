#include "launcher/plugin_settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "launcher/util.h"
#include "oasis/rpc/ini_edit.h"
#include "tools/json_mini.h"

namespace melange::launcher::plugins {
namespace {
std::string Section(const std::string& id) { return "Mod." + id; }

std::string NumText(double v, bool integer) {
    char b[64];
    if (integer) snprintf(b, sizeof b, "%lld", static_cast<long long>(v));
    else snprintf(b, sizeof b, "%.9g", v);
    return b;
}

bool FromIni(const Setting& s, const std::string& text, Val* out) {
    if (s.type == "bool") {
        if (text == "1" || IEquals(text, "true")) *out = Val::B(true);
        else if (text == "0" || IEquals(text, "false")) *out = Val::B(false);
        else return false;
        return true;
    }
    if (s.type == "int" || s.type == "float") {
        char* end = nullptr;
        const double v = strtod(text.c_str(), &end);
        if (text.empty() || !end || *end || !std::isfinite(v)) return false;
        *out = Val::N(s.type == "int" ? std::trunc(v) : v);
        return true;
    }
    if (s.type == "enum" && std::find(s.options.begin(), s.options.end(), text) == s.options.end()) return false;
    *out = Val::S(text);
    return true;
}

bool CheckOne(const Setting& s, const Val& v, std::string* why) {
    if (s.type == "bool") {
        if (v.t != Val::T::Bool) return *why = "must be true or false", false;
        return true;
    }
    if (s.type == "int" || s.type == "float") {
        if (v.t != Val::T::Num || !std::isfinite(v.num)) return *why = "must be a number", false;
        if (s.type == "int" && v.num != std::trunc(v.num)) return *why = "must be a whole number", false;
        if (s.hasMin && v.num < s.min) return *why = "must be at least " + NumText(s.min, s.type == "int"), false;
        if (s.hasMax && v.num > s.max) return *why = "must be at most " + NumText(s.max, s.type == "int"), false;
        return true;
    }
    if (v.t != Val::T::Str) return *why = "must be text", false;
    if (v.str.find_first_of("\r\n;") != std::string::npos) return *why = "cannot contain a line break or ';'", false;
    if (v.str.size() > 1024) return *why = "is too long", false;
    if (s.type == "enum" && std::find(s.options.begin(), s.options.end(), v.str) == s.options.end())
        return *why = "is not one of the options", false;
    return true;
}

std::string ReadIni(const std::wstring& gameDir, oasis::ini::Encoding* enc) {
    std::string bytes;
    *enc = oasis::ini::Encoding::Ansi;
    if (!ReadAll(gameDir + L"\\Melange.ini", &bytes, 4u << 20)) return {};
    return oasis::ini::Decode(bytes, enc);
}
}  // namespace

bool ParseDecl(const json::Value& arr, std::vector<Setting>* out, std::string* err) {
    out->clear();
    if (!arr.IsArray()) return *err = "settings must be an array", false;
    for (const auto& item : arr.items) {
        if (!item.IsObject()) continue;
        Setting s;
        const json::Value* k = item.Get("key");
        const json::Value* t = item.Get("type");
        if (!k || !k->IsString() || k->string.empty() || k->string.find_first_of("\r\n=[];") != std::string::npos) continue;
        if (!t || !t->IsString()) continue;
        s.key = k->string;
        s.type = t->string;
        if (s.type != "bool" && s.type != "int" && s.type != "float" && s.type != "string" && s.type != "enum") continue;
        const json::Value* label = item.Get("label");
        s.label = label && label->IsString() ? label->string : s.key;
        if (const json::Value* h = item.Get("help"); h && h->IsString()) s.help = h->string;
        if (const json::Value* o = item.Get("options"); o && o->IsArray())
            for (const auto& x : o->items)
                if (x.IsString() && x.string.find_first_of("\r\n;") == std::string::npos) s.options.push_back(x.string);
        if (s.type == "enum" && s.options.empty()) continue;
        if (const json::Value* m = item.Get("min"); m && m->IsNumber()) s.hasMin = true, s.min = m->number;
        if (const json::Value* m = item.Get("max"); m && m->IsNumber()) s.hasMax = true, s.max = m->number;
        if (const json::Value* ol = item.Get("optionLabels"); ol && ol->IsObject())
            for (const auto& [opt, v] : ol->members) {
                if (!v.IsObject() || std::find(s.options.begin(), s.options.end(), opt) == s.options.end()) continue;
                OptionLabel l{opt, "", ""};
                if (const json::Value* x = v.Get("label"); x && x->IsString()) l.label = x->string;
                if (const json::Value* x = v.Get("help"); x && x->IsString()) l.help = x->string;
                s.optionLabels.push_back(l);
            }
        // The declared default, coerced to the type; a bad one falls back to the type's zero value.
        Val def;
        const json::Value* d = item.Get("default");
        std::string why;
        if (d && ValFromJson(*d, &def) && CheckOne(s, def, &why)) {
            s.def = def;
        } else if (s.type == "bool") {
            s.def = Val::B(false);
        } else if (s.type == "int" || s.type == "float") {
            s.def = Val::N(s.hasMin ? s.min : 0);
        } else {
            s.def = Val::S(s.type == "enum" ? s.options.front() : "");
        }
        out->push_back(std::move(s));
    }
    return true;
}

std::wstring ModFolder(const std::wstring& gameDir, const std::string& id) {
    if (id.empty() || id.find_first_of("\\/:.") != std::string::npos) return {};
    const std::wstring mods = gameDir + L"\\Mods";
    if (FileExists(mods + L"\\" + Widen(id) + L"\\spice.json")) return mods + L"\\" + Widen(id);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((mods + L"\\*").c_str(), &fd);
    std::wstring found;
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        json::Value v;
        json::Error e;
        const std::wstring dir = mods + L"\\" + fd.cFileName;
        if (!json::ParseFile(dir + L"\\spice.json", &v, &e) || !v.IsObject()) continue;
        const json::Value* x = v.Get("id");
        if (x && x->IsString() && x->string == id) {
            found = dir;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

bool LoadDecl(const std::wstring& gameDir, const std::string& id, std::vector<Setting>* out, std::string* err) {
    out->clear();
    const std::wstring dir = ModFolder(gameDir, id);
    if (dir.empty()) return *err = "no such plugin", false;
    json::Value v;
    json::Error e;
    if (!json::ParseFile(dir + L"\\spice.json", &v, &e) || !v.IsObject()) return *err = "spice.json could not be read", false;
    const json::Value* s = v.Get("settings");
    if (!s) return true;
    return ParseDecl(*s, out, err);
}

Values Defaults(const std::vector<Setting>& decl) {
    Values v;
    for (const auto& s : decl) v[s.key] = s.def;
    return v;
}

Values ReadValues(const std::wstring& gameDir, const std::string& id, const std::vector<Setting>& decl) {
    oasis::ini::Encoding enc{};
    const std::string text = ReadIni(gameDir, &enc);
    const auto entries = oasis::ini::Parse(text);
    Values out;
    for (const auto& s : decl) {
        Val v = s.def;
        if (const auto* e = oasis::ini::Find(entries, Section(id), s.key)) {
            Val parsed;
            std::string why;
            if (FromIni(s, e->value, &parsed) && CheckOne(s, parsed, &why)) v = parsed;
        }
        out[s.key] = v;
    }
    return out;
}

bool Validate(const std::vector<Setting>& decl, const Values& values, std::string* badKey, std::string* why) {
    for (const auto& [k, v] : values) {
        const auto it = std::find_if(decl.begin(), decl.end(), [&](const Setting& s) { return s.key == k; });
        if (it == decl.end()) {
            *badKey = k;
            *why = "is not a setting of this plugin";
            return false;
        }
        if (!CheckOne(*it, v, why)) {
            *badKey = k;
            return false;
        }
    }
    return true;
}

std::string IniText(const Setting& s, const Val& v) {
    if (s.type == "bool") return v.b ? "true" : "false";
    if (s.type == "int" || s.type == "float") return NumText(v.num, s.type == "int");
    return v.str;
}

bool WriteValues(const std::wstring& gameDir, const std::string& id, const std::vector<Setting>& decl, const Values& values, std::string* err) {
    std::string key, why;
    if (!Validate(decl, values, &key, &why)) return *err = key + " " + why, false;
    oasis::ini::Encoding enc{};
    std::string text = ReadIni(gameDir, &enc);
    for (const auto& [k, v] : values) {
        const auto it = std::find_if(decl.begin(), decl.end(), [&](const Setting& s) { return s.key == k; });
        text = oasis::ini::Set(text, Section(id), k, IniText(*it, v));
    }
    std::string bytes;
    if (!oasis::ini::Encode(text, enc, &bytes)) return *err = "a value cannot be written in Melange.ini's encoding", false;
    if (const unsigned long e = WriteAtomic(gameDir + L"\\Melange.ini", bytes)) return *err = "could not write Melange.ini: " + Win32Message(e), false;
    return true;
}

bool ValFromJson(const json::Value& v, Val* out) {
    if (v.IsBool()) return *out = Val::B(v.boolean), true;
    if (v.IsNumber()) return *out = Val::N(v.number), true;
    if (v.IsString()) return *out = Val::S(v.string), true;
    return false;
}

std::string ValJson(const Val& v) {
    if (v.t == Val::T::Bool) return v.b ? "true" : "false";
    if (v.t == Val::T::Num) {
        char b[48];
        snprintf(b, sizeof b, "%.17g", std::isfinite(v.num) ? v.num : 0.0);
        return b;
    }
    return "\"" + jsonmini::Escape(v.str) + "\"";
}

std::string ValuesJson(const Values& vals) {
    jsonmini::Obj o;
    for (const auto& [k, v] : vals) o.Raw(k, ValJson(v));
    return o.End();
}

std::string DeclJson(const std::vector<Setting>& decl) {
    jsonmini::Arr a;
    for (const auto& s : decl) {
        jsonmini::Obj o;
        o.Str("key", s.key).Str("type", s.type).Str("label", s.label).Raw("default", ValJson(s.def));
        if (s.hasMin) o.Float("min", s.min);
        if (s.hasMax) o.Float("max", s.max);
        if (!s.options.empty()) {
            jsonmini::Arr opts;
            for (const auto& x : s.options) opts.Str(x);
            o.Raw("options", opts.End());
        }
        if (!s.help.empty()) o.Str("help", s.help);
        if (!s.optionLabels.empty()) {
            jsonmini::Obj ol;
            for (const auto& l : s.optionLabels) {
                jsonmini::Obj x;
                x.Str("label", l.label.empty() ? l.option : l.label);
                if (!l.help.empty()) x.Str("help", l.help);
                ol.Raw(l.option, x.End());
            }
            o.Raw("optionLabels", ol.End());
        }
        a.Raw(o.End());
    }
    return a.End();
}
}  // namespace melange::launcher::plugins
