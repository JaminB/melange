#include "import/generate.h"

#include <algorithm>
#include <cctype>

#include "erg/xomutil.h"
#include "tools/hash.h"
#include "tools/json_mini.h"
#include "tools/json_read.h"
#include "xom/xom.h"

namespace melange::import {
namespace {
bool Fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
}

bool Resource(const xom::Document& d, const char* cls, const char* name, const xom::Value* value, uint32_t flags,
              xom::Object* out, std::string* err) {
    std::string e;
    if (!erg::xomutil::NewObject(d, cls, out, &e)) return Fail(err, e);
    xom::Value* v = out->field("Value");
    xom::Value* f = out->field("Flags");
    if (!v || !f || !erg::xomutil::SetStr(*out, "Name", name)) return Fail(err, std::string(cls) + ": unexpected schema");
    f->setInt(flags);
    if (v->type == xom::Type::String) v->str = value->str;
    else v->setInt(static_cast<int64_t>(value->bits));
    return true;
}

std::string Q(const std::string& s) { return "\"" + jsonmini::Escape(s) + "\""; }
}  // namespace

bool BuildDescriptor(const DescriptorOut& in, std::vector<uint8_t>* out, std::string* err) {
    xom::Document d;
    d.version = {0, 0, 0, 2};
    d.schmRec = {1, 0, 0};
    d.strings = {""};
    // XContainer has no GUID in the schema; level descriptors carry this one.
    xom::TypeEntry base;
    base.name = "XContainer";
    static const uint8_t kGuid[16] = {0x46, 0xd4, 0x1c, 0x5e, 0xa3, 0x48, 0xfe, 0x44, 0xa5, 0x5a, 0xe2, 0x47, 0xb8, 0xf5, 0xe7, 0x13};
    std::copy(std::begin(kGuid), std::end(kGuid), base.guid.begin());
    std::copy(base.name.begin(), base.name.end(), base.rawName.begin());
    d.types.push_back(base);
    for (const char* cls : {"XResourceDetails", "XUintResourceDetails", "XStringResourceDetails", "XDataBank"})
        if (!erg::xomutil::EnsureType(d, cls, "")) return Fail(err, std::string("descriptor: no schema for ") + cls);
    struct U { const char* name; uint32_t v; } uints[] = {{"Databank.CustomDetailBank", in.customDetailBank},
                                                          {"Databank.CustomTextureBank", in.customTextureBank}};
    std::vector<std::pair<const char*, std::string>> strs = {
        {"Databank.MaterialFile", in.materialFile}, {"Databank.Theme", in.theme}, {"Databank.TimeOfDay", in.timeOfDay}};
    if (in.heightmapBase) strs.emplace_back("Heightmap.BaseTexture", *in.heightmapBase);
    if (in.heightmapSecond) strs.emplace_back("Heightmap.SecondTexture", *in.heightmapSecond);
    xom::Object bank;
    std::string e;
    if (!erg::xomutil::NewObject(d, "XDataBank", &bank, &e)) return Fail(err, e);
    xom::Value* ul = bank.field("UintResources");
    xom::Value* sl = bank.field("StringResources");
    if (!ul || !sl) return Fail(err, "descriptor: unexpected XDataBank schema");
    for (const U& u : uints) {
        xom::Value v;
        v.type = xom::Type::U32;
        v.bits = u.v;
        xom::Object o;
        if (!Resource(d, "XUintResourceDetails", u.name, &v, u.v ? 72 : 64, &o, err)) return false;
        d.objects.push_back(std::move(o));
        ul->items.push_back(erg::xomutil::RefValue(static_cast<uint32_t>(d.objects.size())));
    }
    for (const auto& [name, val] : strs) {
        xom::Value v;
        v.type = xom::Type::String;
        v.str = val;
        xom::Object o;
        if (!Resource(d, "XStringResourceDetails", name, &v, 64, &o, err)) return false;
        d.objects.push_back(std::move(o));
        sl->items.push_back(erg::xomutil::RefValue(static_cast<uint32_t>(d.objects.size())));
    }
    d.objects.push_back(std::move(bank));
    d.root = static_cast<uint32_t>(d.objects.size());
    if (!xom::serialize(d, *out, &e)) return Fail(err, "descriptor: " + e);
    return true;
}

std::string NormalTimeOfDay(const Recipe& r, const std::string& tod) {
    std::string u = tod;
    for (char& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& a : r.transform.timeOfDay)
        if (a == u) return a;
    return r.transform.timeOfDayFallback;
}

std::string CleanTitle(const std::string& raw, size_t max) {
    std::string s;
    for (unsigned char c : raw)
        if (c >= 0x20 && c <= 0x7e) s.push_back(static_cast<char>(c));
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    s = s.substr(i);
    if (s.size() > max) s.resize(max);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::string SpiceJson(const PackSpec& p) {
    std::string s = "{\n";
    s += "  \"spiceVersion\": 1,\n";
    s += "  \"id\": " + Q(p.id) + ",\n";
    s += "  \"version\": " + Q(p.version) + ",\n";
    s += "  \"name\": " + Q(p.name) + ",\n";
    s += "  \"authors\": [" + Q(p.author) + "],\n";
    s += "  \"description\": " + Q(p.description) + ",\n";
    s += "  \"kind\": \"content\",\n";
    s += "  \"melange\": { \"range\": " + Q(p.melangeRange) + " },\n";
    s += "  \"generated\": { \"by\": " + Q(p.generatedBy) + ", \"recipe\": " + Q(p.recipe) + ", \"format\": " +
         std::to_string(kFormat) + " },\n";
    s += "  \"levels\": [\n";
    for (size_t i = 0; i < p.levels.size(); ++i) {
        const PackLevel& l = p.levels[i];
        s += "    { \"slug\": " + Q(l.slug) + ", \"title\": " + Q(l.title) + ", \"survivor\": " + (l.survivor ? "true" : "false") + " }";
        s += i + 1 < p.levels.size() ? ",\n" : "\n";
    }
    s += "  ]\n}\n";
    return s;
}

std::string CatalogueJson(const std::vector<MapInfo>& maps) {
    jsonmini::Arr a;
    for (const MapInfo& m : maps) {
        jsonmini::Obj o;
        o.Str("file", m.file).Str("stem", m.stem).Str("pack", m.pack).Str("title", m.title).Str("author", m.author);
        o.Str("group", m.group).Str("groupLabel", m.groupLabel).Str("category", m.category).Str("categoryLabel", m.categoryLabel);
        o.Str("mode", m.mode).Str("theme", m.theme).Str("timeOfDay", m.timeOfDay).Bool("survivor", m.survivor).Bool("preview", m.preview);
        a.Raw(o.End());
    }
    return jsonmini::Obj().Int("catalogue", 1).Raw("maps", a.End()).End() + "\n";
}

bool ParseCatalogue(const std::string& text, std::vector<MapInfo>* out) {
    out->clear();
    json::Value v;
    json::Error e;
    if (!json::Parse(text, &v, &e, 4u << 20) || !v.IsObject()) return false;
    const json::Value* maps = v.Get("maps");
    if (!maps || !maps->IsArray()) return false;
    for (const json::Value& m : maps->items) {
        if (!m.IsObject()) continue;
        MapInfo i;
        auto s = [&](const char* k, std::string* o) {
            if (const json::Value* x = m.Get(k); x && x->IsString()) *o = x->string;
        };
        auto b = [&](const char* k, bool* o) {
            if (const json::Value* x = m.Get(k); x && x->IsBool()) *o = x->boolean;
        };
        s("file", &i.file), s("stem", &i.stem), s("pack", &i.pack), s("title", &i.title), s("author", &i.author);
        s("group", &i.group), s("groupLabel", &i.groupLabel), s("category", &i.category), s("categoryLabel", &i.categoryLabel);
        s("mode", &i.mode), s("theme", &i.theme), s("timeOfDay", &i.timeOfDay);
        b("survivor", &i.survivor), b("preview", &i.preview);
        if (!i.file.empty() && !i.stem.empty()) out->push_back(std::move(i));
    }
    return true;
}

std::string Fingerprint(std::vector<std::pair<std::string, std::string>> fh) {
    std::sort(fh.begin(), fh.end());
    std::string text;
    for (const auto& [path, sha] : fh) text += path + "\t" + sha + "\n";
    return hashutil::Sha256Hex(text.data(), text.size());
}
}  // namespace melange::import
