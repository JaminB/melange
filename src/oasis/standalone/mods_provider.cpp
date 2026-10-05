#include "oasis/standalone/mods_provider.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "mods/spice.h"
#include "oasis/standalone/json_write.h"
#include "store/compat.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::oasis::standalone::modsprov {
namespace {
std::wstring ModsDir(const std::wstring& gameDir) { return gameDir + L"\\Mods"; }
std::wstring StatePath(const std::wstring& gameDir) { return gameDir + L"\\Mods\\thumper-state.json"; }

// True whether or not a file existed: an empty object is a fine starting point for a fresh install.
void LoadState(const std::wstring& gameDir, json::Value* root) {
    json::Error e;
    if (json::ParseFile(StatePath(gameDir), root, &e) && root->IsObject()) return;
    *root = json::Value{};
    root->type = json::Type::Object;
}

bool SaveState(const std::wstring& gameDir, const json::Value& root) {
    const std::wstring path = StatePath(gameDir);
    CreateDirectoryW((gameDir + L"\\Mods").c_str(), nullptr);
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const std::string text = WriteJson(root);
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// The object member `key` of `obj`, created as `type` when missing.
json::Value* Member(json::Value& obj, const char* key, json::Type type) {
    for (auto& [k, v] : obj.members)
        if (k == key) return &v;
    obj.members.emplace_back(key, json::Value{});
    json::Value* v = &obj.members.back().second;
    v->type = type;
    return v;
}

std::map<std::string, bool> EnabledMap(const json::Value& root) {
    std::map<std::string, bool> out;
    if (const json::Value* en = root.Get("enabled"); en && en->IsObject())
        for (const auto& [k, v] : en->members)
            if (v.IsBool()) out[k] = v.boolean;
    return out;
}

std::set<std::string> DeepDesertGranted(const json::Value& root) {
    std::set<std::string> out;
    if (const json::Value* dd = root.Get("deepDesert"); dd && dd->IsObject())
        for (const auto& [id, rec] : dd->members) {
            const json::Value* g = rec.Get("granted");
            if (g && g->IsBool() && g->boolean) out.insert(id);
        }
    return out;
}

// A folder whose spice.json failed to parse: listed (incompatible, never enabled), as Thumper does.
struct Broken {
    std::string id, reason;
};

std::vector<spice::Manifest> ScanManifests(const std::wstring& gameDir, std::vector<Broken>* broken = nullptr) {
    std::vector<spice::Manifest> out;
    const std::wstring modsDir = ModsDir(gameDir);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((modsDir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        const std::wstring name = fd.cFileName;
        spice::Manifest m;
        std::vector<spice::Error> errs;
        if (spice::Parse(modsDir + L"\\" + name, &m, &errs)) {
            out.push_back(std::move(m));
        } else if (broken) {
            Broken b;
            for (wchar_t c : name) b.id.push_back(static_cast<char>(c < 128 ? std::tolower(static_cast<int>(c)) : '?'));
            for (const spice::Error& er : errs) {
                if (!b.reason.empty()) b.reason += "; ";
                if (er.line) b.reason += std::to_string(er.line) + ":" + std::to_string(er.col) + " ";
                b.reason += er.text;
            }
            if (b.reason.empty()) b.reason = "invalid spice.json";
            broken->push_back(std::move(b));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

// The same names as the game's mods.list (oasis/rpc/mods.cpp), so one web page reads both.
const char* StateName(mods::State s) {
    switch (s) {
        case mods::State::Enabled: return "enabled";
        case mods::State::Disabled: return "disabled";
        case mods::State::Blocked: return "blocked";
        case mods::State::PendingConsent: return "pending-consent";
        case mods::State::Incompatible: return "incompatible";
        case mods::State::RestartRequired: return "restart-required";
    }
    return "disabled";
}
}  // namespace

std::string ListJson(const std::wstring& gameDir, const std::string& melangeVersion) {
    json::Value state;
    LoadState(gameDir, &state);
    const auto enabledMap = EnabledMap(state);
    const auto granted = DeepDesertGranted(state);
    std::vector<Broken> broken;
    const std::vector<spice::Manifest> manifests = ScanManifests(gameDir, &broken);
    const std::set<std::string> storeIds = compat::StoreIds(ModsDir(gameDir));

    std::set<std::string> userEnabled;
    for (const auto& m : manifests) {
        const auto it = enabledMap.find(m.id);
        if (it != enabledMap.end() ? it->second : m.defaultEnabled) userEnabled.insert(m.id);
    }
    const auto resolved = spice::Resolve(manifests, userEnabled, melangeVersion, {});
    std::map<std::string, const spice::Manifest*> byId;
    for (const auto& m : manifests) byId[m.id] = &m;

    jsonmini::Arr arr;
    for (const auto& r : resolved) {
        const auto it = byId.find(r.id);
        if (it == byId.end()) continue;
        const spice::Manifest& m = *it->second;
        mods::State st = r.state;
        if (st == mods::State::PendingConsent && granted.count(r.id)) st = mods::State::Enabled;
        std::string authors;
        for (size_t i = 0; i < m.authors.size(); ++i) authors += (i ? ", " : "") + m.authors[i];
        arr.Raw(jsonmini::Obj()
                    .Str("id", m.id)
                    .Str("name", m.name)
                    .Str("version", m.version)
                    .Str("authors", authors)
                    .Str("kind", m.content ? "content" : "client-only")
                    .Str("state", StateName(st))
                    .Str("reason", r.reason)
                    .Bool("on", userEnabled.count(m.id) > 0)
                    .Int("order", r.order)
                    .Str("generatedBy", m.generatedBy)
                    .Str("source", compat::IsStore(storeIds, m.id, m.generatedBy) ? "store" : "local")
                    .End());
    }
    for (const Broken& b : broken) {
        const auto it = enabledMap.find(b.id);
        arr.Raw(jsonmini::Obj()
                    .Str("id", b.id)
                    .Str("name", b.id)
                    .Str("version", "")
                    .Str("authors", "")
                    .Str("kind", "client-only")
                    .Str("state", "incompatible")
                    .Str("reason", b.reason)
                    .Bool("on", it != enabledMap.end() && it->second)
                    .Int("order", -1)
                    .Str("generatedBy", "")
                    .Str("source", storeIds.count(b.id) ? "store" : "local")
                    .End());
    }
    return arr.End();
}

std::vector<spice::Manifest> Enabled(const std::wstring& gameDir, const std::string& melangeVersion) {
    json::Value state;
    LoadState(gameDir, &state);
    const auto enabledMap = EnabledMap(state);
    const auto granted = DeepDesertGranted(state);
    std::vector<spice::Manifest> manifests = ScanManifests(gameDir);
    std::set<std::string> userEnabled;
    for (const auto& m : manifests) {
        const auto it = enabledMap.find(m.id);
        if (it != enabledMap.end() ? it->second : m.defaultEnabled) userEnabled.insert(m.id);
    }
    auto resolved = spice::Resolve(manifests, userEnabled, melangeVersion, {});
    std::sort(resolved.begin(), resolved.end(), [](const spice::Resolved& a, const spice::Resolved& b) { return a.order < b.order; });
    std::vector<spice::Manifest> out;
    for (const auto& r : resolved) {
        if (r.state != mods::State::Enabled && !(r.state == mods::State::PendingConsent && granted.count(r.id))) continue;
        for (auto& m : manifests)
            if (m.id == r.id) out.push_back(m);
    }
    return out;
}

int SetEnabled(const std::wstring& gameDir, const std::string& id, bool on) {
    bool exists = false;
    for (const auto& m : ScanManifests(gameDir)) exists |= m.id == id;
    if (!exists) return 0;

    json::Value state;
    LoadState(gameDir, &state);
    json::Value* en = Member(state, "enabled", json::Type::Object);
    if (!en->IsObject()) {
        *en = json::Value{};
        en->type = json::Type::Object;
    }
    json::Value* v = Member(*en, id.c_str(), json::Type::Bool);
    v->type = json::Type::Bool;
    v->boolean = on;
    return SaveState(gameDir, state) ? 1 : -1;
}

bool Forget(const std::wstring& gameDir, const std::string& id) {
    json::Value state;
    LoadState(gameDir, &state);
    bool changed = false;
    for (auto& [k, v] : state.members) {
        if ((k == "enabled" || k == "deepDesert") && v.IsObject())
            changed |= std::erase_if(v.members, [&](const auto& m) { return m.first == id; }) > 0;
        if (k == "pins" && v.IsArray())
            changed |= std::erase_if(v.items, [&](const json::Value& p) {
                for (const char* f : {"id", "before", "after"})
                    if (const json::Value* x = p.Get(f); x && x->IsString() && x->string == id) return true;
                return false;
            }) > 0;
    }
    return !changed || SaveState(gameDir, state);
}

bool ShowLocal(const std::wstring& gameDir) {
    json::Value state;
    LoadState(gameDir, &state);
    const json::Value* v = state.Get("showLocal");
    return v && v->IsBool() && v->boolean;
}

bool SetShowLocal(const std::wstring& gameDir, bool on) {
    json::Value state;
    LoadState(gameDir, &state);
    json::Value* v = Member(state, "showLocal", json::Type::Bool);
    v->type = json::Type::Bool;
    v->boolean = on;
    return SaveState(gameDir, state);
}

std::string ViewJson(const std::wstring& gameDir) {
    return jsonmini::Obj()
        .Bool("showLocal", ShowLocal(gameDir))
        .Raw("notices", compat::NoticesJson(compat::LoadNotices(ModsDir(gameDir))))
        .End();
}
}  // namespace melange::oasis::standalone::modsprov
