#include "launcher/settings.h"

#include "launcher/util.h"
#include "tools/json_mini.h"

namespace melange::launcher {
std::wstring SettingsPath() { return AppDataDir() + L"\\launcher.json"; }

std::string DefaultsJson(const std::vector<DefaultPlugin>& d, bool seeded) {
    jsonmini::Arr arr;
    for (const auto& p : d) arr.Raw(jsonmini::Obj().Str("id", p.id).Bool("enabled", p.enabled).Raw("settings", plugins::ValuesJson(p.settings)).End());
    return jsonmini::Obj().Raw("plugins", arr.End()).Bool("seeded", seeded).End();
}

bool DefaultsFromJson(const json::Value& v, std::vector<DefaultPlugin>* out, bool* seeded, std::string* err) {
    out->clear();
    if (!v.IsObject()) return *err = "defaults must be an object", false;
    if (const json::Value* s = v.Get("seeded"); s && s->IsBool()) *seeded = s->boolean;
    const json::Value* p = v.Get("plugins");
    if (!p) return true;
    if (!p->IsArray() || p->items.size() > 200) return *err = "plugins must be an array", false;
    for (const auto& it : p->items) {
        const json::Value* id = it.Get("id");
        if (!it.IsObject() || !id || !id->IsString() || id->string.empty() || id->string.size() > 64) return *err = "each plugin needs an id", false;
        DefaultPlugin d;
        d.id = id->string;
        if (const json::Value* e = it.Get("enabled"); e && e->IsBool()) d.enabled = e->boolean;
        if (const json::Value* s = it.Get("settings"); s && s->IsObject())
            for (const auto& [k, val] : s->members) {
                plugins::Val x;
                if (plugins::ValFromJson(val, &x)) d.settings[k] = x;
            }
        bool dup = false;
        for (const auto& o : *out) dup |= o.id == d.id;
        if (!dup) out->push_back(std::move(d));
    }
    return true;
}

bool LoadSettings(const std::wstring& path, Settings* out) {
    *out = Settings{};
    json::Value v;
    json::Error e;
    if (!json::ParseFile(path, &v, &e) || !v.IsObject()) return false;
    if (const json::Value* g = v.Get("gameDir"); g && g->IsString()) out->gameDir = Widen(g->string);
    if (const json::Value* f = v.Get("firstRunDone"); f && f->IsBool()) out->firstRunDone = f->boolean;
    if (const json::Value* t = v.Get("theme"); t && t->IsString() && (t->string == "light" || t->string == "dark" || t->string == "system"))
        out->theme = t->string;
    if (const json::Value* w = v.Get("window"); w && w->IsObject()) {
        auto num = [&](const char* k, int* dst) {
            const json::Value* x = w->Get(k);
            if (x && x->IsNumber()) *dst = static_cast<int>(x->number);
            return x && x->IsNumber();
        };
        out->window.saved = num("left", &out->window.left) & num("top", &out->window.top) & num("right", &out->window.right) &
                            num("bottom", &out->window.bottom);
        if (const json::Value* m = w->Get("maximized"); m && m->IsBool()) out->window.maximized = m->boolean;
    }
    if (const json::Value* u = v.Get("lastUpdateCheck"); u && u->IsString()) out->lastUpdateCheck = u->string;
    if (const json::Value* a = v.Get("autoUpdate"); a && a->IsBool()) out->autoUpdate = a->boolean;
    if (const json::Value* d = v.Get("defaults")) {
        std::string err;
        if (!DefaultsFromJson(*d, &out->defaults, &out->defaultsSeeded, &err)) out->defaults.clear();
    }
    return true;
}

bool SaveSettings(const std::wstring& path, const Settings& s) {
    jsonmini::Obj o;
    o.Int("version", 1);
    if (!s.gameDir.empty()) o.Str("gameDir", Narrow(s.gameDir));
    o.Bool("firstRunDone", s.firstRunDone).Str("theme", s.theme);
    if (s.window.saved)
        o.Raw("window", jsonmini::Obj().Int("left", s.window.left).Int("top", s.window.top).Int("right", s.window.right)
                            .Int("bottom", s.window.bottom).Bool("maximized", s.window.maximized).End());
    o.Raw("defaults", DefaultsJson(s.defaults, s.defaultsSeeded));
    if (!s.lastUpdateCheck.empty()) o.Str("lastUpdateCheck", s.lastUpdateCheck);
    o.Bool("autoUpdate", s.autoUpdate);
    MakeDirs(Parent(path));
    return WriteAtomic(path, o.End()) == 0;
}
}  // namespace melange::launcher
