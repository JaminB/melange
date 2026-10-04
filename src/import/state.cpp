#include "import/state.h"

#include <windows.h>

#include "store/install.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::import {
namespace {
std::wstring W(const std::string& s) {
    std::wstring w;
    for (char c : s) w.push_back(static_cast<unsigned char>(c));
    return w;
}

bool MakeDir(const std::wstring& p) {
    if (CreateDirectoryW(p.c_str(), nullptr)) return true;
    const DWORD a = GetFileAttributesW(p.c_str());
    return GetLastError() == ERROR_ALREADY_EXISTS && a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) &&
           !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}
}  // namespace

Paths MakePaths(const std::wstring& gameDir, const std::string& plugin) { return Paths{gameDir, gameDir + L"\\Mods", W(plugin)}; }

bool EnsureWork(const Paths& p) {
    const std::wstring dot = p.mods + L"\\.import";
    const bool ok = MakeDir(p.mods) && MakeDir(dot) && MakeDir(p.Work()) && MakeDir(p.Dl()) && MakeDir(p.Stage());
    if (ok) SetFileAttributesW(dot.c_str(), FILE_ATTRIBUTE_HIDDEN);
    return ok;
}

std::string StateJson(const State& s) {
    jsonmini::Arr packs;
    for (const PackRecord& r : s.packs) {
        jsonmini::Arr cats;
        for (const auto& c : r.categories) cats.Str(c);
        packs.Raw(jsonmini::Obj().Str("id", r.id).Int("levels", r.levels).Raw("categories", cats.End()).End());
    }
    jsonmini::Obj counts;
    for (const auto& [k, v] : s.counts) counts.Int(k, v);
    jsonmini::Arr hidden;
    for (const auto& h : s.hiddenByFile) hidden.Str(h);
    jsonmini::Obj o;
    o.Str("recipe", s.recipe).Str("recipeVersion", s.recipeVersion).Int("format", s.format).Str("sourceSha256", s.sourceSha256);
    o.Str("fingerprint", s.fingerprint).Int("maps", s.maps).Raw("packs", packs.End()).Raw("counts", counts.End());
    o.Int("skipped", s.skipped).UInt("bytes", s.bytes).Str("importedAt", s.importedAt).Bool("zipKept", s.zipKept);
    o.Raw("hiddenByFile", hidden.End());
    return o.End() + "\n";
}

bool LoadState(const Paths& p, State* out) {
    *out = State{};
    json::Value v;
    json::Error e;
    if (!json::ParseFile(p.StateFile(), &v, &e) || !v.IsObject()) return false;
    auto str = [&](const json::Value& o, const char* k, std::string* d) {
        if (const json::Value* x = o.Get(k); x && x->IsString()) *d = x->string;
    };
    auto num = [&](const json::Value& o, const char* k, int* d) {
        if (const json::Value* x = o.Get(k); x && x->IsInteger()) *d = static_cast<int>(x->number);
    };
    str(v, "recipe", &out->recipe);
    str(v, "recipeVersion", &out->recipeVersion);
    str(v, "sourceSha256", &out->sourceSha256);
    str(v, "fingerprint", &out->fingerprint);
    str(v, "importedAt", &out->importedAt);
    num(v, "format", &out->format);
    num(v, "maps", &out->maps);
    num(v, "skipped", &out->skipped);
    if (const json::Value* x = v.Get("bytes"); x && x->IsInteger()) out->bytes = static_cast<uint64_t>(x->number);
    if (const json::Value* x = v.Get("zipKept"); x && x->IsBool()) out->zipKept = x->boolean;
    if (const json::Value* x = v.Get("packs"); x && x->IsArray())
        for (const auto& it : x->items) {
            if (!it.IsObject()) continue;
            PackRecord r;
            str(it, "id", &r.id);
            num(it, "levels", &r.levels);
            if (const json::Value* c = it.Get("categories"); c && c->IsArray())
                for (const auto& ci : c->items)
                    if (ci.IsString()) r.categories.push_back(ci.string);
            if (!r.id.empty()) out->packs.push_back(std::move(r));
        }
    if (const json::Value* x = v.Get("counts"); x && x->IsObject())
        for (const auto& [k, n] : x->members)
            if (n.IsInteger()) out->counts[k] = static_cast<int>(n.number);
    if (const json::Value* x = v.Get("hiddenByFile"); x && x->IsArray())
        for (const auto& it : x->items)
            if (it.IsString() && it.string.size() <= 128) out->hiddenByFile.push_back(it.string);
    return !out->recipe.empty();
}

bool SaveState(const Paths& p, const State& s) { return store::install::WriteFileAtomic(p.StateFile(), StateJson(s)); }
}  // namespace melange::import
