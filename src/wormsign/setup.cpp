#include "wormsign/setup.h"

#include <cstdio>

#include "tools/json_mini.h"
#include "tools/json_read.h"

namespace melange::wormsign::setup {
namespace {
std::string Hex(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", v);
    return b;
}

struct Field {
    const char* key;
    const char* label;
};
constexpr Field kFields[] = {
    {"level", "level"},         {"landFile", "land file"},     {"landTheme", "land theme"},
    {"dataBank", "data bank"},  {"timeOfDay", "time of day"},  {"levelDetails", "level details"},
    {"lastScheme", "scheme choice"}, {"schemeName", "scheme name"}, {"scheme", "scheme settings"},
    {"init", "team setup"}, {"levelSim", "level script"},
};

std::string Show(const json::Value* v) {
    if (!v) return "(none)";
    switch (v->type) {
        case json::Type::String: return "\"" + v->string + "\"";
        case json::Type::Number: {
            char b[32];
            snprintf(b, sizeof b, "%.0f", v->number);
            return b;
        }
        case json::Type::Bool: return v->boolean ? "true" : "false";
        case json::Type::Null: return "null";
        default: return "(value)";
    }
}

bool Same(const json::Value* a, const json::Value* b) {
    if (!a || !b || a->type != b->type) return false;
    switch (a->type) {
        case json::Type::String: return a->string == b->string;
        case json::Type::Number: return a->number == b->number;
        case json::Type::Bool: return a->boolean == b->boolean;
        case json::Type::Null: return true;
        default: return false;
    }
}
}  // namespace

std::string ToJson(const Data& d) {
    jsonmini::Obj o;
    o.Int("v", kVersion)
        .Str("level", d.level)
        .Str("landFile", d.landFile)
        .Str("landTheme", d.landTheme)
        .Str("dataBank", d.dataBank)
        .Str("timeOfDay", d.timeOfDay)
        .Str("levelDetails", d.levelDetails)
        .Str("lastScheme", d.lastScheme)
        .Str("schemeName", d.schemeName)
        .Str("levelSim", d.levelSim);
    if (d.haveScheme) o.Str("scheme", Hex(d.scheme));
    if (!d.schemeRaw.empty()) o.Str("schemeRaw", d.schemeRaw);
    if (d.haveInit) o.Str("init", Hex(d.init));
    jsonmini::Arr teams;
    for (const Team& t : d.teams) teams.Raw(jsonmini::Obj().Str("name", t.name).UInt("worms", t.worms).End());
    o.Raw("teams", teams.End());
    return o.End();
}

bool Compare(std::string_view recorded, std::string_view live, std::string* why) {
    json::Value r, l;
    json::Error e;
    if (!json::Parse(recorded, &r, &e) || !r.IsObject()) {
        *why = "the recorded setup is unreadable";
        return false;
    }
    if (!json::Parse(live, &l, &e) || !l.IsObject()) {
        *why = "the live setup is unreadable";
        return false;
    }
    const json::Value* v = r.Get("v");
    if (v && v->IsNumber() && v->number > kVersion) {
        *why = "the recording's setup fingerprint is newer than this build: not compared";
        return true;
    }
    for (const Field& f : kFields) {
        const json::Value* a = r.Get(f.key);
        if (!a) continue;
        const json::Value* b = l.Get(f.key);
        if (!Same(a, b)) {
            if (std::string_view(f.key) == "levelSim") *why = "level script differs";
            else *why = std::string(f.label) + ": recorded " + Show(a) + ", now " + Show(b);
            return false;
        }
    }
    const json::Value* rt = r.Get("teams");
    if (rt && rt->IsArray()) {
        const json::Value* lt = l.Get("teams");
        const size_t ln = lt && lt->IsArray() ? lt->items.size() : 0;
        if (ln != rt->items.size()) {
            *why = "teams: recorded " + std::to_string(rt->items.size()) + ", now " + std::to_string(ln);
            return false;
        }
        for (size_t i = 0; i < ln; ++i) {
            for (const char* k : {"name", "worms"}) {
                const json::Value* a = rt->items[i].Get(k);
                if (!a) continue;
                const json::Value* b = lt->items[i].Get(k);
                if (!Same(a, b)) {
                    *why = "team " + std::to_string(i + 1) + " " + k + ": recorded " + Show(a) + ", now " + Show(b);
                    return false;
                }
            }
        }
    }
    why->clear();
    return true;
}
}  // namespace melange::wormsign::setup
