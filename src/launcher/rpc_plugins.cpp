// plugins.*, defaults.*, recommended.*: plugin settings in Melange.ini, the user's defaults in launcher.json, and the
// recommended set first run suggests (installed through the Store engine).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "launcher/app.h"
#include "launcher/plugin_settings.h"
#include "launcher/recommended.h"
#include "launcher/rpc.h"
#include "launcher/util.h"
#include "oasis/standalone/mods_provider.h"
#include "store/store.h"
#include "tools/json_mini.h"

namespace melange::launcher::rpc {
namespace {
using oasis::Call;
using oasis::Result;
std::atomic<bool> g_applying{false};

bool ValidId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char ch : id)
        if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_')) return false;
    return true;
}

// The declaration from the installed plugin, else a built-in one (a recommended plugin not installed yet).
bool Decl(const std::wstring& game, const std::string& id, std::vector<plugins::Setting>* decl, bool* installed) {
    std::string err;
    *installed = !game.empty() && !plugins::ModFolder(game, id).empty();
    if (*installed) return plugins::LoadDecl(game, id, decl, &err);
    return BuiltinDecl(id, decl);
}

// Declared defaults with the user's own defaults on top.
plugins::Values UserDefaults(const std::string& id, const std::vector<plugins::Setting>& decl) {
    plugins::Values v = plugins::Defaults(decl);
    for (const auto& d : app::GetSettings().defaults)
        if (d.id == id)
            for (const auto& [k, val] : d.settings) {
                plugins::Values one{{k, val}};
                std::string bk, why;
                if (plugins::Validate(decl, one, &bk, &why)) v[k] = val;
            }
    return v;
}

bool ReadValuesParam(const json::Value& p, plugins::Values* out) {
    const json::Value* vals = p.Get("values");
    if (!vals || !vals->IsObject()) return false;
    for (const auto& [k, v] : vals->members) {
        plugins::Val x;
        if (!plugins::ValFromJson(v, &x)) return false;
        (*out)[k] = x;
    }
    return true;
}

void Settings_(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string id = Str(p, "id");
    if (!ValidId(id)) return Fail(r, -32602, "expected {id}");
    const std::wstring game = app::GameDir();
    std::vector<plugins::Setting> decl;
    bool installed = false;
    if (!Decl(game, id, &decl, &installed)) return Fail(r, -32602, "no such plugin");
    const plugins::Values values = installed ? plugins::ReadValues(game, id, decl) : UserDefaults(id, decl);
    r.json = jsonmini::Obj()
                 .Raw("decl", plugins::DeclJson(decl))
                 .Raw("values", plugins::ValuesJson(values))
                 .Raw("defaults", plugins::ValuesJson(UserDefaults(id, decl)))
                 .End();
}

void Write(Result& r, const std::string& id, const plugins::Values& values) {
    const std::wstring game = app::GameDir();
    if (game.empty()) return Fail(r, -32000, "Choose your game folder first.");
    if (const std::string gate = app::WriteGate(); !gate.empty()) return Fail(r, -32000, gate);
    // Shares setup's transaction lock: a plugin write must not race setup.apply/restore touching the same
    // Mods\ folder or Melange.ini.
    std::unique_lock lk(app::Tx(), std::try_to_lock);
    if (!lk.owns_lock()) return Fail(r, -32002, app::BusyMessage());
    std::vector<plugins::Setting> decl;
    std::string err;
    if (plugins::ModFolder(game, id).empty() || !plugins::LoadDecl(game, id, &decl, &err)) return Fail(r, -32602, "no such plugin");
    std::string key, why;
    if (!plugins::Validate(decl, values, &key, &why))
        return Fail(r, -32602, key + " " + why, jsonmini::Obj().Str("key", key).Str("why", why).End());
    if (!plugins::WriteValues(game, id, decl, values, &err)) return Fail(r, -32000, err);
    r.json = jsonmini::Obj().Raw("values", plugins::ValuesJson(plugins::ReadValues(game, id, decl))).End();
}

void SetSettings(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string id = Str(p, "id");
    plugins::Values values;
    if (!ValidId(id) || !ReadValuesParam(p, &values)) return Fail(r, -32602, "expected {id, values}");
    Write(r, id, values);
}

void ResetSettings(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const std::string id = Str(p, "id");
    if (!ValidId(id)) return Fail(r, -32602, "expected {id}");
    std::vector<plugins::Setting> decl;
    bool installed = false;
    if (!Decl(app::GameDir(), id, &decl, &installed) || !installed) return Fail(r, -32602, "no such plugin");
    Write(r, id, UserDefaults(id, decl));
}

void DefaultsGet(const Call&, Result& r, void*) {
    const Settings s = app::GetSettings();
    r.json = DefaultsJson(s.defaults, s.defaultsSeeded);
}

void DefaultsSet(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    std::vector<DefaultPlugin> d;
    bool seeded = true;
    std::string err;
    if (!DefaultsFromJson(p, &d, &seeded, &err)) return Fail(r, -32602, err);
    for (auto& plugin : d) {
        if (!ValidId(plugin.id)) return Fail(r, -32602, "bad plugin id", jsonmini::Obj().Str("key", plugin.id).Str("why", "is not a plugin id").End());
        std::vector<plugins::Setting> decl;
        bool installed = false;
        if (!Decl(app::GameDir(), plugin.id, &decl, &installed)) continue;   // unknown here: kept as given
        std::string key, why;
        if (!plugins::Validate(decl, plugin.settings, &key, &why))
            return Fail(r, -32602, plugin.id + " " + key + " " + why, jsonmini::Obj().Str("id", plugin.id).Str("key", key).Str("why", why).End());
    }
    app::UpdateSettings([&](Settings& s) {
        s.defaults = d;
        s.defaultsSeeded = seeded;
    });
    r.json = DefaultsJson(d, seeded);
}

// The index's list once the Store has one (fetching it first, briefly), else the built-in list.
std::vector<Recommended> Current(std::string* source) {
    store::Status st = store::GetStatus();
    if (store::Active() && !st.haveIndex) {
        store::EnsureFetched();
        for (int i = 0; i < 40; ++i) {
            st = store::GetStatus();
            if (st.haveIndex || (!st.fetching && st.offline)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    const std::wstring game = app::GameDir();
    std::vector<Recommended> items;
    *source = "builtin";
    const std::string text = store::IndexText();
    if (!text.empty()) {
        store::Index idx;
        std::string err;
        DeclLookup lookup = [&](const std::string& id, std::vector<plugins::Setting>* out) {
            bool installed = false;
            return Decl(game, id, out, &installed);
        };
        if (store::ParseIndex(text, &idx, &err) && ParseRecommended(text, idx, &items, lookup)) *source = "index";
    }
    if (*source == "builtin") items = BuiltinRecommended();
    for (auto& it : items) {
        it.installed = !game.empty() && !plugins::ModFolder(game, it.id).empty();
        if (it.decl.empty()) {
            bool installed = false;
            Decl(game, it.id, &it.decl, &installed);
        }
        store::Details d;
        if (store::GetDetails(it.id, &d, false)) {
            it.compatible = !d.item.compatible.empty() || it.installed;
            it.reason = it.compatible ? std::string() : d.item.reason;
            if (it.name.empty()) it.name = d.item.name;
            if (it.description.empty()) it.description = d.item.description;
        } else if (!it.installed) {
            it.compatible = false;
            it.reason = st.offline ? "The plugin store can't be reached right now." : "Not in the plugin store's list.";
        }
    }
    return items;
}

void RecommendedGet(const Call&, Result& r, void*) {
    std::string source;
    jsonmini::Arr arr;
    for (const auto& it : Current(&source)) arr.Raw(RecommendedJson(it));
    r.json = jsonmini::Obj().Str("source", source).Raw("items", arr.End()).End();
}

struct ApplyItem {
    std::string id, name;
    plugins::Values settings;
};

void ApplyOne(const ApplyItem& it) {
    const std::wstring game = app::GameDir();
    if (game.empty()) return;
    if (plugins::ModFolder(game, it.id).empty()) {
        for (int i = 0; i < 1200 && store::GetStatus().busy; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (!store::GetStatus().haveIndex) {
            store::Refresh();
            for (int i = 0; i < 160 && !store::GetStatus().haveIndex; ++i) {
                if (!store::GetStatus().fetching && store::GetStatus().offline) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
        const store::Outcome o = store::Install(it.id, "", true, false);
        if (o.code != 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (int i = 0; i < 2400 && store::GetStatus().busy; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (plugins::ModFolder(game, it.id).empty()) return;
    }
    std::vector<plugins::Setting> decl;
    std::string err;
    if (plugins::LoadDecl(game, it.id, &decl, &err)) {
        plugins::Values valid;
        for (const auto& [k, v] : it.settings) {
            plugins::Values one{{k, v}};
            std::string bk, why;
            if (plugins::Validate(decl, one, &bk, &why)) valid[k] = v;
        }
        if (!valid.empty() && app::WriteGate().empty()) plugins::WriteValues(game, it.id, decl, valid, &err);
    }
    if (app::WriteGate().empty()) oasis::standalone::modsprov::SetEnabled(game, it.id, true);
    store::MarkDirty();
}

void RecommendedApply(const Call& c, Result& r, void*) {
    json::Value p;
    if (!Params(c, r, &p)) return;
    const json::Value* items = p.Get("items");
    if (!items || !items->IsArray() || items->items.size() > 32) return Fail(r, -32602, "expected {items, saveAsDefaults}");
    std::vector<ApplyItem> list;
    for (const auto& it : items->items) {
        ApplyItem a;
        a.id = it.IsObject() ? Str(it, "id") : "";
        if (!ValidId(a.id)) return Fail(r, -32602, "each item needs an id");
        a.name = it.IsObject() ? Str(it, "name") : "";
        if (a.name.empty()) a.name = a.id;
        if (const json::Value* s = it.Get("settings"); s && s->IsObject())
            for (const auto& [k, v] : s->members) {
                plugins::Val x;
                if (plugins::ValFromJson(v, &x)) a.settings[k] = x;
            }
        list.push_back(std::move(a));
    }
    if (Bool(p, "saveAsDefaults"))
        app::UpdateSettings([&](Settings& s) {
            s.defaults.clear();
            for (const auto& a : list) s.defaults.push_back(DefaultPlugin{a.id, true, a.settings});
            s.defaultsSeeded = true;
        });
    if (list.empty()) {
        r.json = "{\"queued\":[]}";
        return;
    }
    if (app::GameDir().empty()) return Fail(r, -32000, "Choose your game folder first.");
    if (const std::string gate = app::WriteGate(); !gate.empty()) return Fail(r, -32000, gate);
    if (g_applying.exchange(true)) return Fail(r, -32002, "Plugins are already being installed.");
    // Report busy right away if a setup.* transaction already holds app::Tx(). The worker below takes its own
    // lock for the actual duration instead of inheriting this one: a std::mutex must be unlocked by the same
    // thread that locked it, so a lock can't cross the thread boundary into the detached worker.
    {
        std::unique_lock<std::mutex> probe(app::Tx(), std::try_to_lock);
        if (!probe.owns_lock()) {
            g_applying = false;
            return Fail(r, -32002, app::BusyMessage());
        }
    }
    jsonmini::Arr queued;
    for (const auto& a : list) queued.Str(a.id);
    const int total = static_cast<int>(list.size());
    app::SetBatchBusy(true, "recommended", 0, total, "Installing plugins…");
    std::thread([list, total] {
        // Held for the whole batch so setup.apply/restore can't run against the same Mods\ folder or
        // Melange.ini while plugins are being installed. A setup.* call that sneaks in between the probe
        // above and this lock just waits here rather than racing the writes below.
        std::lock_guard<std::mutex> lk(app::Tx());
        int step = 0;
        for (const auto& a : list) {
            ++step;
            app::SetBatchBusy(true, "recommended", step, total, "Installing " + a.name + "…");
            ApplyOne(a);
        }
        g_applying = false;
        app::SetBatchBusy(false, "", 0, 0, "");
    }).detach();
    r.json = jsonmini::Obj().Raw("queued", queued.End()).End();
}
}  // namespace

void InstallPlugins() {
    using oasis::kRpcMutating;
    using oasis::kRpcServerThread;
    oasis::AddMethod("plugins.settings", &Settings_, nullptr, kRpcServerThread);
    oasis::AddMethod("plugins.setSettings", &SetSettings, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("plugins.resetSettings", &ResetSettings, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("defaults.get", &DefaultsGet, nullptr, kRpcServerThread);
    oasis::AddMethod("defaults.set", &DefaultsSet, nullptr, kRpcServerThread | kRpcMutating);
    oasis::AddMethod("recommended.get", &RecommendedGet, nullptr, kRpcServerThread);
    oasis::AddMethod("recommended.apply", &RecommendedApply, nullptr, kRpcServerThread | kRpcMutating);
}
}  // namespace melange::launcher::rpc
