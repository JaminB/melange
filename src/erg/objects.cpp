#include "erg/objects.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

#include "erg/xomutil.h"
#include "xom/xom.h"

namespace melange::erg::objects {
namespace {
constexpr uintmax_t kMaxWeaponFile = 4u << 20;

bool Fail(std::string* err, std::string text) {
    if (err) *err = std::move(text);
    return false;
}

// kWeapon<Name> or kUtility<Name>: the prefix, an upper-case letter, then letters and digits.
bool CrateName(std::string_view n, std::string_view prefix) {
    if (n.size() <= prefix.size() || n.size() > 63 || n.compare(0, prefix.size(), prefix) != 0) return false;
    if (!std::isupper(static_cast<unsigned char>(n[prefix.size()]))) return false;
    return std::all_of(n.begin(), n.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

bool Contains(const std::vector<std::string>& v, const std::string& s) { return std::binary_search(v.begin(), v.end(), s); }

std::string KnotName(ObjectType type, int group, int n) {
    switch (type) {
        case ObjectType::Crate: return "CRATE_" + std::to_string(n);
        case ObjectType::Telepad: return "TP_" + std::to_string(group) + "_" + std::to_string(n);
        case ObjectType::Trigger: return "TRIG_" + std::to_string(n);
        case ObjectType::MineFactory: return "minefactory";
    }
    return {};
}
}  // namespace

bool CatalogFrom(const xom::Document& d, Catalog* out) {
    Catalog c;
    for (const auto& o : d.objects) {
        if (o.type != "XContainerResourceDetails") continue;
        const std::string name = xomutil::Str(o, "Name");
        const bool weapon = CrateName(name, "kWeapon"), utility = CrateName(name, "kUtility");
        if (!weapon && !utility) continue;
        const xom::Value* v = o.field("Value");
        const xom::Object* w = v && v->type == xom::Type::Ref && !v->array ? d.object(v->asRef()) : nullptr;
        // Sub-munitions and factory payloads share the table but have no display name of their own.
        if (!w || xomutil::Str(*w, "DisplayName") != "Text." + name) continue;
        (weapon ? c.weapons : c.utilities).push_back(name);
    }
    for (auto* v : {&c.weapons, &c.utilities}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    if (c.weapons.empty() && c.utilities.empty()) return false;
    *out = std::move(c);
    return true;
}

bool LoadCatalog(const std::filesystem::path& game, Catalog* out, std::string* err) {
    const std::filesystem::path path = game / "Data" / "Tweak" / "WEAPTWK.XOM";
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > kMaxWeaponFile) return Fail(err, "the install's weapon table (Data\\Tweak\\WEAPTWK.XOM) cannot be read");
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return Fail(err, "the install's weapon table cannot be read");
    xom::Document d;
    xom::ParseOptions opt;
    opt.strict = true;
    std::string e;
    if (!xom::parse(bytes.data(), bytes.size(), d, &e, opt)) return Fail(err, "the install's weapon table does not parse: " + e);
    if (!CatalogFrom(d, out)) return Fail(err, "the install's weapon table lists no weapons");
    return true;
}

bool Validate(const Scene& s, const Catalog& c, std::vector<std::string>* warnings, std::string* err) {
    std::map<int, std::vector<std::string>> pads;
    for (const auto& o : s.objects) {
        if (o.type == ObjectType::Telepad) pads[o.group].push_back(o.knot);
        if (o.type != ObjectType::Crate || !warnings || !s.water) continue;
        for (const auto& d : s.details) {
            Vec3 w;
            if (d.src || d.name != o.knot || !DetailWorld(s, d, &w)) continue;
            const double y = w[1] * s.worldPerXan;
            if (y < *s.water) warnings->push_back(o.knot + " is under the water (" + std::to_string(static_cast<int>(y)) + " < " +
                                                  std::to_string(static_cast<int>(*s.water)) + ")");
        }
    }
    if (warnings)
        for (const auto& [g, knots] : pads)
            if (knots.size() == 1)
                warnings->push_back("telepad group " + std::to_string(g) + " has one pad (" + knots[0] + "); it needs a partner");
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const ObjectSpec& o = s.objects[i];
        if (o.type != ObjectType::Crate || o.crate.kind == CrateKind::Health) continue;
        const bool weapon = o.crate.kind == CrateKind::Weapon;
        if (!Contains(weapon ? c.weapons : c.utilities, o.crate.contents))
            return Fail(err, "objects[" + std::to_string(i) + "].crate.contents: '" + o.crate.contents + "' is not a " +
                                 (weapon ? "weapon" : "utility") + " of this install");
    }
    return true;
}

std::string NextKnot(const Scene& s, ObjectType type, int group) {
    if (type == ObjectType::Telepad && (group < 1 || group > static_cast<int>(kMaxTelepadGroups))) return {};
    std::set<std::string> used;
    for (const auto& d : s.details) used.insert(d.name);
    for (const auto& o : s.objects) used.insert(o.knot);
    const int last = type == ObjectType::MineFactory ? 0 : 255;
    for (int n = 0; n <= last; ++n) {
        std::string k = KnotName(type, group, n);
        if (!used.count(k)) return k;
    }
    return {};
}
}  // namespace melange::erg::objects
