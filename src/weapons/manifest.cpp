#include "weapons/manifest.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "weapons/fields.h"

namespace melange::weapons::manifest {
namespace {
struct Base {
    const char* name;
    int id;
};
constexpr Base kBases[] = {
    {"kWeaponBazooka", 1}, {"kWeaponGrenade", 2}, {"kWeaponHolyHandGrenade", 6}, {"kWeaponBananaBomb", 7},
    {"kWeaponGasCanister", 16},
};

std::vector<CloneDecl> g_frozen;
bool g_isFrozen = false;
std::vector<TextDecl> g_frozenText;
bool g_textFrozen = false;

bool SafeRel(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find_first_of("/\\", i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        i = j + 1;
    }
    return true;
}

bool EndsWithI(const std::string& s, const char* suffix) {
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[s.size() - n + i])) != suffix[i]) return false;
    return true;
}

bool IsFreeCell(int c) {
    for (int f : kFreeCells)
        if (f == c) return true;
    return false;
}

bool CheckValue(const spice::WeaponSet& s, FieldType t, SetValue* out, std::string* why) {
    out->field = s.field;
    out->type = t;
    switch (t) {
        case FieldType::F32:
            if (s.kind != spice::WeaponSet::Number) break;
            if (!std::isfinite(s.number) || std::fabs(s.number) > 1e9) {
                *why = "out of range";
                return false;
            }
            out->number = s.number;
            return true;
        case FieldType::I32:
        case FieldType::U32:
        case FieldType::U16:
        case FieldType::U8: {
            if (s.kind != spice::WeaponSet::Number) break;
            const double lo = t == FieldType::I32 ? -2147483648.0 : 0.0;
            const double hi = t == FieldType::I32   ? 2147483647.0
                              : t == FieldType::U32 ? 4294967295.0
                              : t == FieldType::U16 ? 65535.0
                                                    : 255.0;
            if (s.number != std::floor(s.number) || s.number < lo || s.number > hi) {
                *why = "must be an integer in range";
                return false;
            }
            out->number = s.number;
            return true;
        }
        case FieldType::Bool:
            if (s.kind != spice::WeaponSet::Boolean) break;
            out->boolean = s.boolean;
            return true;
        case FieldType::String:
            if (s.kind != spice::WeaponSet::String) break;
            out->string = s.string;
            return true;
        default:
            *why = "cannot be set";
            return false;
    }
    *why = "has the wrong type";
    return false;
}
}  // namespace

int BaseId(const std::string& base) {
    for (auto& b : kBases)
        if (base == b.name) return b.id;
    return -1;
}

const char* BaseName(int id) {
    for (auto& b : kBases)
        if (b.id == id) return b.name;
    return nullptr;
}

bool ValidName(const std::string& n) {
    if (n.size() < 10 || n.size() > 48 || n.compare(0, 7, "kWeapon") != 0) return false;
    if (!(n[7] >= 'A' && n[7] <= 'Z')) return false;
    for (size_t i = 8; i < n.size(); ++i)
        if (!std::isalnum(static_cast<unsigned char>(n[i]))) return false;
    if (n.size() - 8 > 40) return false;
    return n.compare(0, 14, "kWeaponCluster") != 0 && n.compare(0, 14, "kWeaponFactory") != 0;
}

std::vector<CloneDecl> Parse(const spice::Manifest& m, std::vector<Error>* errs) {
    std::vector<CloneDecl> out;
    bool ok = true;
    auto fail = [&](const std::string& what, const std::string& text) {
        if (errs) errs->push_back({m.id, what + ": " + text});
        ok = false;
    };
    if (!m.weapons.empty() && !m.content) fail("weapons", "requires kind: \"content\"");
    if (m.weapons.size() > static_cast<size_t>(kMaxClones)) fail("weapons", "at most 3 weapons");
    for (const auto& w : m.weapons) {
        CloneDecl d;
        d.mod = m.id;
        d.name = w.name;
        d.base = w.base;
        const std::string tag = w.name.empty() ? "weapons" : w.name;
        if (!ValidName(w.name))
            fail(tag, "name must match kWeapon[A-Z][A-Za-z0-9]{2,40} and not start with kWeaponCluster or kWeaponFactory");
        for (auto& o : out)
            if (o.name == w.name) fail(tag, "declared twice");
        d.baseId = BaseId(w.base);
        if (d.baseId < 0) fail(tag, "base '" + w.base + "' is not clonable in this version");
        if (w.cell >= 0 && !IsFreeCell(w.cell)) fail(tag, "cell must be 29, 39 or 40");
        for (auto& o : out)
            if (w.cell >= 0 && o.cell == w.cell) fail(tag, "cell " + std::to_string(w.cell) + " used twice");
        d.cell = w.cell;
        if (!w.bank.empty() && (!SafeRel(w.bank) || !EndsWithI(w.bank, ".xom")))
            fail(tag, "bank must be a relative .xom path under assets/data/");
        d.bank = w.bank;
        if (!w.panelIcon.empty() && (!SafeRel(w.panelIcon) || !EndsWithI(w.panelIcon, ".png")))
            fail(tag, "panelIcon must be a relative .png path under assets/");
        d.panelIcon = w.panelIcon;
        if (!w.hudIcon.empty() &&
            (w.hudIcon.find_first_of("/\\:") != std::string::npos || w.hudIcon.compare(0, m.id.size() + 1, m.id + ".") != 0 ||
             !EndsWithI(w.hudIcon, ".tga")))
            fail(tag, "hudIcon must be a .tga file name in assets/loose/ named '" + m.id + ".*'");
        d.hudIcon = w.hudIcon;
        d.text = {w.textName, w.textHelp};
        for (const auto& s : w.set) {
            const FieldType t = fields::SchemaType(kContainerClass, s.field.c_str());
            if (t == FieldType::None) {
                fail(tag, "set." + s.field + " is not a settable field of " + kContainerClass);
                continue;
            }
            SetValue v;
            std::string why;
            if (!CheckValue(s, t, &v, &why)) {
                fail(tag, "set." + s.field + " " + why);
                continue;
            }
            d.set.push_back(std::move(v));
        }
        out.push_back(std::move(d));
    }
    if (!ok) out.clear();
    return out;
}

std::vector<CloneDecl> Assign(const std::vector<std::vector<CloneDecl>>& perMod, std::vector<Error>* refused) {
    std::vector<CloneDecl> acc;
    for (const auto& mod : perMod) {
        if (mod.empty()) continue;
        std::string why;
        if (acc.size() + mod.size() > static_cast<size_t>(kMaxClones)) why = "no free panel cell left (3 clones in total)";
        for (const auto& d : mod)
            for (const auto& a : acc) {
                if (why.empty() && a.name == d.name) why = d.name + " is already declared by " + a.mod;
                if (why.empty() && d.cell >= 0 && a.cell == d.cell)
                    why = "cell " + std::to_string(d.cell) + " is already taken by " + a.mod;
            }
        if (!why.empty()) {
            if (refused) refused->push_back({mod.front().mod, why});
            continue;
        }
        for (const auto& d : mod) acc.push_back(d);
    }
    for (size_t i = 0; i < acc.size(); ++i) {
        acc[i].k = static_cast<uint16_t>(i);
        if (acc[i].cell >= 0) continue;
        for (int c : kFreeCells) {
            bool used = false;
            for (const auto& o : acc) used |= o.cell == c;
            if (!used) {
                acc[i].cell = c;
                break;
            }
        }
    }
    return acc;
}

bool ValidTextKey(const std::string& k) {
    size_t p = 0;
    if (k.compare(0, 7, "kWeapon") == 0) p = 7;
    else if (k.compare(0, 8, "kUtility") == 0) p = 8;
    else return false;
    if (p >= k.size() || !(k[p] >= 'A' && k[p] <= 'Z')) return false;
    const size_t tail = k.size() - p - 1;
    if (tail < 2 || tail > 40) return false;
    for (size_t i = p + 1; i < k.size(); ++i)
        if (!std::isalnum(static_cast<unsigned char>(k[i]))) return false;
    return true;
}

std::vector<TextDecl> ParseText(const spice::Manifest& m, std::vector<Error>* errs) {
    std::vector<TextDecl> out;
    bool ok = true;
    auto fail = [&](const std::string& what, const std::string& text) {
        if (errs) errs->push_back({m.id, what + ": " + text});
        ok = false;
    };
    auto printable = [](const std::string& s) {
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x20 && c < 0x7f; });
    };
    if (m.weaponText.empty()) return out;
    if (!m.content) fail("weaponText", "requires kind: \"content\"");
    if (m.weaponText.size() > kMaxTextPerMod) fail("weaponText", "at most 64 entries");
    for (const auto& w : m.weaponText) {
        const std::string tag = w.weapon.empty() ? "weaponText" : w.weapon;
        if (!ValidTextKey(w.weapon)) fail(tag, "key must match ^k(Weapon|Utility)[A-Z][A-Za-z0-9]{2,40}$");
        for (const auto& o : out)
            if (o.weapon == w.weapon) fail(tag, "listed twice");
        for (const auto& c : m.weapons)
            if (c.name == w.weapon) fail(tag, "is a clone of this mod; clones have their own \"text\"");
        if (w.name.empty() && w.help.empty()) fail(tag, "needs a name or a help");
        if (w.name.size() > kMaxTextName || !printable(w.name)) fail(tag, "name must be 1-24 printable ASCII characters");
        if (w.help.size() > kMaxTextHelp || !printable(w.help)) fail(tag, "help must be at most 160 printable ASCII characters");
        out.push_back({m.id, w.weapon, w.name, w.help});
    }
    if (!ok) out.clear();
    return out;
}

std::vector<TextDecl> AssignText(const std::vector<std::vector<TextDecl>>& perMod, const std::vector<std::string>& cloneNames,
                                 std::vector<Error>* refused) {
    std::vector<TextDecl> acc;
    for (const auto& mod : perMod) {
        if (mod.empty()) continue;
        std::string why;
        for (const auto& d : mod) {
            if (why.empty() && std::find(cloneNames.begin(), cloneNames.end(), d.weapon) != cloneNames.end())
                why = "weaponText." + d.weapon + " is a clone name; clones have their own \"text\"";
            for (const auto& a : acc)
                if (why.empty() && a.weapon == d.weapon) why = d.weapon + " is already renamed by " + a.mod;
        }
        if (!why.empty()) {
            if (refused) refused->push_back({mod.front().mod, why});
            continue;
        }
        for (const auto& d : mod) acc.push_back(d);
    }
    return acc;
}

std::vector<IconDecl> ParseIcons(const spice::Manifest& m, std::vector<Error>* errs) {
    std::vector<IconDecl> out;
    bool ok = true;
    auto fail = [&](const std::string& what, const std::string& text) {
        if (errs) errs->push_back({m.id, what + ": " + text});
        ok = false;
    };
    if (m.weaponIcons.empty()) return out;
    if (!m.content) fail("weaponIcons", "requires kind: \"content\"");
    if (m.weaponIcons.size() > kMaxIconsPerMod) fail("weaponIcons", "at most 64 entries");
    for (const auto& w : m.weaponIcons) {
        const std::string tag = w.weapon.empty() ? "weaponIcons" : w.weapon;
        if (!ValidTextKey(w.weapon)) fail(tag, "key must match ^k(Weapon|Utility)[A-Z][A-Za-z0-9]{2,40}$");
        for (const auto& o : out)
            if (o.weapon == w.weapon) fail(tag, "listed twice");
        for (const auto& c : m.weapons)
            if (c.name == w.weapon) fail(tag, "is a clone of this mod; clones have their own panelIcon and hudIcon");
        if (w.panelIcon.empty() && w.hudIcon.empty()) fail(tag, "needs a panelIcon or a hudIcon");
        if (!w.panelIcon.empty() && (!SafeRel(w.panelIcon) || !EndsWithI(w.panelIcon, ".png")))
            fail(tag, "panelIcon must be a relative .png path under assets/");
        if (!w.hudIcon.empty() &&
            (w.hudIcon.find_first_of("/\\:") != std::string::npos || w.hudIcon.compare(0, m.id.size() + 1, m.id + ".") != 0 ||
             !EndsWithI(w.hudIcon, ".tga")))
            fail(tag, "hudIcon must be a .tga file name in assets/loose/ named '" + m.id + ".*'");
        out.push_back({m.id, w.weapon, w.panelIcon, w.hudIcon});
    }
    if (!ok) out.clear();
    return out;
}

std::vector<IconDecl> AssignIcons(const std::vector<std::vector<IconDecl>>& perMod, const std::vector<std::string>& cloneNames,
                                  std::vector<Error>* refused) {
    std::vector<IconDecl> acc;
    for (const auto& mod : perMod) {
        if (mod.empty()) continue;
        std::string why;
        for (const auto& d : mod) {
            if (why.empty() && std::find(cloneNames.begin(), cloneNames.end(), d.weapon) != cloneNames.end())
                why = "weaponIcons." + d.weapon + " is a clone name; clones have their own panelIcon and hudIcon";
            for (const auto& a : acc)
                if (why.empty() && a.weapon == d.weapon) why = d.weapon + " already has icons from " + a.mod;
        }
        if (!why.empty()) {
            if (refused) refused->push_back({mod.front().mod, why});
            continue;
        }
        for (const auto& d : mod) acc.push_back(d);
    }
    return acc;
}

Resolved Resolve(const std::vector<std::vector<CloneDecl>>& clones, const std::vector<std::vector<TextDecl>>& texts,
                 const std::vector<std::vector<IconDecl>>& icons) {
    Resolved r;
    const size_t n = std::max({clones.size(), texts.size(), icons.size()});
    std::vector<std::string> ids(n);
    for (size_t i = 0; i < n; ++i) {
        if (i < clones.size() && !clones[i].empty()) ids[i] = clones[i].front().mod;
        else if (i < texts.size() && !texts[i].empty()) ids[i] = texts[i].front().mod;
        else if (i < icons.size() && !icons[i].empty()) ids[i] = icons[i].front().mod;
    }
    auto named = [&](const std::vector<Error>& list, size_t i) {
        return !ids[i].empty() && std::any_of(list.begin(), list.end(), [&](const Error& er) { return er.mod == ids[i]; });
    };
    // Clone cells first, then renames and icons against the clones that survived. A rename or icon refusal removes
    // that mod's clones, which can free a cell or a name, so the clone pass runs again. Clone refusals are recomputed
    // every round (a mod refused only because of a mod that is gone is let back in); rename and icon refusals are
    // kept, so the set of removed mods only grows and the loop ends.
    std::vector<bool> textOut(n, false);
    std::vector<Error> textRefusals;
    for (;;) {
        std::vector<std::vector<CloneDecl>> cl(n);
        for (size_t i = 0; i < n && i < clones.size(); ++i)
            if (!textOut[i]) cl[i] = clones[i];
        std::vector<Error> cloneErrs;
        r.clones = Assign(cl, &cloneErrs);
        // Only clones that will exist block a rename or an icon rule: those of a mod refused for any reason do not count.
        std::vector<std::vector<TextDecl>> tx(n);
        std::vector<std::vector<IconDecl>> ic(n);
        std::vector<std::string> cloneNames;
        for (size_t i = 0; i < n; ++i) {
            if (textOut[i] || named(cloneErrs, i)) continue;
            if (i < clones.size())
                for (const auto& d : clones[i]) cloneNames.push_back(d.name);
            if (i < texts.size()) tx[i] = texts[i];
            if (i < icons.size()) ic[i] = icons[i];
        }
        std::vector<Error> textErrs, iconErrs;
        r.texts = AssignText(tx, cloneNames, &textErrs);
        r.icons = AssignIcons(ic, cloneNames, &iconErrs);
        textErrs.insert(textErrs.end(), iconErrs.begin(), iconErrs.end());
        bool again = false;
        for (size_t i = 0; i < n; ++i)
            if (!textOut[i] && named(textErrs, i)) textOut[i] = again = true;
        textRefusals.insert(textRefusals.end(), textErrs.begin(), textErrs.end());
        if (!again) {
            r.refused = cloneErrs;
            break;
        }
    }
    r.refused.insert(r.refused.end(), textRefusals.begin(), textRefusals.end());
    return r;
}

std::vector<IconDecl> g_frozenIcons;
bool g_iconsFrozen = false;

void FreezeIcons(std::vector<IconDecl> decls) {
    if (g_iconsFrozen) return;
    g_frozenIcons = std::move(decls);
    g_iconsFrozen = true;
}

bool IsIconsFrozen() { return g_iconsFrozen; }

const std::vector<IconDecl>& FrozenIcons() { return g_frozenIcons; }

void FreezeText(std::vector<TextDecl> decls) {
    if (g_textFrozen) return;
    g_frozenText = std::move(decls);
    g_textFrozen = true;
}

bool IsTextFrozen() { return g_textFrozen; }

const std::vector<TextDecl>& FrozenText() { return g_frozenText; }

void Freeze(std::vector<CloneDecl> decls) {
    if (g_isFrozen) return;
    g_frozen = std::move(decls);
    g_isFrozen = true;
}

bool IsFrozen() { return g_isFrozen; }

const std::vector<CloneDecl>& Frozen() { return g_frozen; }
}  // namespace melange::weapons::manifest
