#include "erg/bank.h"

#include <algorithm>
#include <set>

#include "erg/scene.h"
#include "erg/xomutil.h"

namespace melange::erg::bank {
namespace {
using xom::Type;
using xom::Value;

const xom::Object* Template(const xom::Document& s) {
    const xom::Object* any = nullptr;
    for (const auto& o : s.objects) {
        if (o.type != "WXFE_LevelDetails") continue;
        if (xomutil::Str(o, "Level_FileName") == "Multi_DinerMight") return &o;
        if (!any && xomutil::Int(o, "Level_Type", -1) == 0) any = &o;
    }
    return any;
}

bool SetInt(xom::Object& o, const char* field, int64_t v) {
    Value* f = o.field(field);
    if (!f || f->array) return false;
    f->setInt(v);
    return true;
}
}  // namespace

std::vector<uint8_t> RegistryBank(const xom::Document& scripts, const std::vector<Entry>& entries, std::string* err) {
    std::vector<uint8_t> out;
    auto fail = [err](std::string why) {
        if (err) *err = std::move(why);
        return std::vector<uint8_t>{};
    };
    if (entries.empty() || entries.size() > 128) return fail("a registry bank holds 1-128 levels");
    std::set<std::string> keys;
    for (const auto& e : entries) {
        if (!PrintableAscii(e.key, 1, 79) || !PrintableAscii(e.stem, 1, 63) || e.stem.find('.') != std::string::npos ||
            !PrintableAscii(e.frontendName, 1, 79) || !PrintableAscii(e.scripts, 1, 127))
            return fail("registry entry '" + e.key + "': key, stem, name or scripts are not printable (or the stem has a '.')");
        if (!keys.insert(e.key).second) return fail("registry entry '" + e.key + "' is listed twice");
    }
    const xom::Object* tmpl = Template(scripts);
    const xom::Object *crd = nullptr, *bank = nullptr;
    for (const auto& o : scripts.objects) {
        if (!crd && o.type == "XContainerResourceDetails") crd = &o;
        if (!bank && o.type == "XDataBank") bank = &o;
    }
    if (!tmpl || !crd || !bank) return fail("SCRIPTS.XOM has no level entry, resource or data bank to copy");

    xom::Document d;
    d.version = scripts.version;
    d.reserved08 = scripts.reserved08;
    d.reserved24 = scripts.reserved24;
    d.guidRec = scripts.guidRec;
    d.schmRec = scripts.schmRec;
    static const char* const kTypes[] = {"XContainer", "XResourceDetails", "XContainerResourceDetails", "XDataBank",
                                         "WXFE_LevelDetails"};
    for (const auto& t : scripts.types)
        if (std::find(std::begin(kTypes), std::end(kTypes), t.name) != std::end(kTypes)) {
            d.types.push_back(t);
            d.types.back().count = 0;
        }

    std::set<std::string> strings;
    for (const auto& [k, v] : tmpl->fields)
        if (v.type == Type::String && !v.array) strings.insert(v.str);
    const uint32_t n = static_cast<uint32_t>(entries.size());
    for (uint32_t i = 0; i < n; ++i) {
        xom::Object r = *crd;
        Value* val = r.field("Value");
        if (!xomutil::SetStr(r, "Name", entries[i].key) || !val || val->type != Type::Ref)
            return fail("SCRIPTS.XOM: unexpected resource shape");
        val->bits = n + 2 + i;
        d.objects.push_back(std::move(r));
    }
    xom::Object b = *bank;
    for (auto& [k, v] : b.fields)
        if (v.array) {
            v.items.clear();
            v.raw.clear();
        }
    Value* list = b.field("ContainerResources");
    if (!list || list->type != Type::Ref) return fail("SCRIPTS.XOM: unexpected data bank shape");
    for (uint32_t i = 0; i < n; ++i) list->items.push_back(xomutil::RefValue(i + 1));
    d.objects.push_back(std::move(b));
    for (const auto& e : entries) {
        xom::Object l = *tmpl;
        if (!xomutil::SetStr(l, "Level_FileName", e.stem) || !xomutil::SetStr(l, "Level_ScriptName", e.scripts) ||
            !xomutil::SetStr(l, "Frontend_Name", e.frontendName) || !xomutil::SetStr(l, "Lock", "") ||
            !SetInt(l, "Level_Type", e.levelType) || !SetInt(l, "Theme_Type", e.themeType))
            return fail("SCRIPTS.XOM: unexpected level entry shape");
        strings.insert({e.key, e.stem, e.scripts, e.frontendName});
        d.objects.push_back(std::move(l));
    }
    strings.erase("");
    d.strings.push_back("");
    d.strings.insert(d.strings.end(), strings.begin(), strings.end());
    d.root = n + 1;

    std::string e;
    if (!xom::serialize(d, out, &e)) return fail("registry bank: " + e);
    xom::Document check;
    xom::ParseOptions opt;
    opt.strict = true;
    if (!xom::parse(out.data(), out.size(), check, &e, opt) || !check.object(check.root) || check.object(check.root)->type != "XDataBank")
        return fail("registry bank: the output does not parse back");
    return out;
}
}  // namespace melange::erg::bank
