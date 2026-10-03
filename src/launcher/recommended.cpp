#include "launcher/recommended.h"

#include <cstdio>

#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::launcher {
namespace {
constexpr char kSunstoneDecl[] = R"([
  {"key":"quality","type":"enum","options":["off","low","subtle","bold","ultra"],"default":"bold","label":"Quality",
   "optionLabels":{"off":{"label":"Off"},"low":{"label":"Low","help":"Light touches for older PCs."},
                   "subtle":{"label":"Subtle","help":"Close to the original look, only cleaner."},
                   "bold":{"label":"Bold","help":"The full look. Recommended."},
                   "ultra":{"label":"Ultra","help":"Bold plus 2x2 supersampling. Needs a strong GPU."}}},
  {"key":"look","type":"enum","options":["golden","dusk"],"default":"golden","label":"Look",
   "optionLabels":{"golden":{"label":"Golden"},"dusk":{"label":"Dusk"}}},
  {"key":"water","type":"bool","default":true,"label":"Sunstone water"},
  {"key":"lighting","type":"bool","default":true,"label":"Sunstone lighting"}
])";
constexpr char kSunstoneWhy[] = "Sharper, richer graphics. Client-only: it never affects other players.";
}  // namespace

bool BuiltinDecl(const std::string& id, std::vector<plugins::Setting>* out) {
    if (id != "sunstone") return false;
    json::Value v;
    json::Error e;
    std::string err;
    return json::Parse(kSunstoneDecl, &v, &e) && plugins::ParseDecl(v, out, &err);
}

std::vector<Recommended> BuiltinRecommended() {
    Recommended r;
    r.id = "sunstone";
    r.name = "Sunstone";
    r.description = "A dramatic, client-only graphics overhaul: sharper textures, anti-aliasing, grading, atmosphere, soft shadows, "
                    "per-pixel lighting and water.";
    r.why = kSunstoneWhy;
    BuiltinDecl(r.id, &r.decl);
    r.settings["quality"] = plugins::Val::S("bold");
    return {r};
}

bool ParseRecommended(std::string_view text, const store::Index& idx, std::vector<Recommended>* out, const DeclLookup& decl) {
    out->clear();
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, store::kMaxIndexBytes) || !v.IsObject()) return false;
    const json::Value* rec = v.Get("recommended");
    if (!rec) return false;
    if (!rec->IsArray()) return true;
    for (const auto& it : rec->items) {
        const json::Value* id = it.IsObject() ? it.Get("id") : nullptr;
        if (!id || !id->IsString()) continue;
        const store::Plugin* p = store::FindPlugin(idx, id->string);
        if (!p) {
            fprintf(stderr, "recommended: %s is not in the index; dropped\n", id->string.c_str());
            continue;
        }
        Recommended r;
        r.id = p->id;
        r.name = p->name;
        r.description = p->description;
        if (const json::Value* w = it.Get("why"); w && w->IsString()) r.why = w->string;
        if (const json::Value* s = it.Get("settings"); s && s->IsObject())
            for (const auto& [k, val] : s->members) {
                plugins::Val x;
                if (plugins::ValFromJson(val, &x)) r.settings[k] = x;
            }
        if (decl && decl(r.id, &r.decl)) {
            std::string key, why;
            if (!plugins::Validate(r.decl, r.settings, &key, &why)) {
                fprintf(stderr, "recommended: %s setting %s %s; dropped\n", r.id.c_str(), key.c_str(), why.c_str());
                continue;
            }
        }
        out->push_back(std::move(r));
    }
    return true;
}

std::string RecommendedJson(const Recommended& r) {
    jsonmini::Obj o;
    o.Str("id", r.id).Str("name", r.name).Str("description", r.description).Str("why", r.why).Raw("settings", plugins::ValuesJson(r.settings))
        .Bool("installed", r.installed).Bool("compatible", r.compatible);
    if (!r.reason.empty()) o.Str("reason", r.reason);
    if (!r.decl.empty()) o.Raw("decl", plugins::DeclJson(r.decl));
    return o.End();
}
}  // namespace melange::launcher
