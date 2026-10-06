#include "schemes/builder.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "erg/scene.h"
#include "erg/xomutil.h"
#include "tools/json_read.h"

namespace melange::schemes {
namespace {
namespace fs = std::filesystem;
namespace xu = erg::xomutil;
using xom::Object;
using xom::Type;
using xom::Value;

// Visits a value and every value inside it (array items, struct members).
template <class V, class F>
void Each(V& v, F&& f) {
    f(v);
    for (auto& i : v.items) Each(i, f);
    for (auto& m : v.members) Each(m.second, f);
}

bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsAlnum(char c) { return IsAlpha(c) || (c >= '0' && c <= '9'); }

// A relative path with no drive, no leading slash and no "." or ".." segment.
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

// The mod's file, read and parsed; false with *why when the path leaves the folder or the file is not valid JSON.
bool LoadJson(const Source& s, const std::string& rel, json::Value* out, std::string* why) {
    if (!SafeRel(rel) || !rel.ends_with(".json")) {
        *why = "the path must be a relative .json path inside the mod folder";
        return false;
    }
    std::error_code ec;
    const fs::path file = fs::weakly_canonical(s.dir / fs::path(std::u8string(rel.begin(), rel.end())), ec);
    const fs::path root = fs::weakly_canonical(s.dir, ec);
    const fs::path inside = file.lexically_relative(root);
    if (ec || inside.empty() || *inside.begin() == "..") {
        *why = "the file is outside the mod folder";
        return false;
    }
    json::Error err;
    if (!json::ParseFile(file.wstring(), out, &err)) {
        *why = err.line ? "line " + std::to_string(err.line) + ": " + err.text : err.text;
        return false;
    }
    if (!out->IsObject()) {
        *why = "the file must hold a JSON object";
        return false;
    }
    return true;
}

bool Int(const json::Value& v, int64_t lo, int64_t hi, int64_t* out) {
    if (!v.IsInteger() || v.number < static_cast<double>(lo) || v.number > static_cast<double>(hi)) return false;
    *out = static_cast<int64_t>(v.number);
    return true;
}

constexpr int64_t kI32Min = INT32_MIN, kI32Max = INT32_MAX;

// The graph being built: LOCAL's resource, its reachable objects and the entries added so far. Refs are 1-based
// positions in `objs`.
struct Graph {
    std::vector<Object> objs;
    uint32_t collective = 0;

    Value* List(const char* field) { return objs[collective - 1].field(field); }

    // A deep copy of `ref` and everything it references; returns the copy's ref. Objects are only ever appended, so
    // a failed entry is undone with objs.resize().
    uint32_t Clone(uint32_t ref, std::map<uint32_t, uint32_t>& memo) {
        if (auto it = memo.find(ref); it != memo.end()) return it->second;
        objs.emplace_back();
        const uint32_t at = static_cast<uint32_t>(objs.size());
        memo[ref] = at;
        Object o = objs[ref - 1];
        for (auto& f : o.fields)
            Each(f.second, [&](Value& v) {
                if (v.type == Type::Ref && !v.array && v.bits) v.bits = Clone(static_cast<uint32_t>(v.bits), memo);
            });
        objs[at - 1] = std::move(o);
        return at;
    }
    uint32_t Clone(uint32_t ref) {
        std::map<uint32_t, uint32_t> memo;
        return Clone(ref, memo);
    }
};

// One kind of list: the resource name, its collective class and list field, the entry class and the key it is known by.
struct Kind {
    const char* wrapper;
    const char* collective;
    const char* list;
    const char* entry;
    size_t maxTotal;
    const char* what;
};
constexpr Kind kSchemes = {"DATA.LockedSchemes", "SchemeColective", "Schemes", "SchemeData", kMaxSchemes, "schemes"};
constexpr Kind kFactory = {"DATA.LockedWeapons", "WeaponFactoryCollective", "Weapons", "StoreWeaponFactory", kMaxFactoryWeapons,
                           "factory weapons"};

const Value* RefField(const Object& o, const char* name) {
    const Value* v = o.field(name);
    return v && v->type == Type::Ref && !v->array ? v : nullptr;
}

// The key an entry is known by: a scheme's Name, a preset's Weapon container Name.
std::string KeyOf(const Graph& g, const Kind& k, uint32_t ref) {
    const Object* o = ref && ref <= g.objs.size() ? &g.objs[ref - 1] : nullptr;
    if (!o || o->type != k.entry) return {};
    if (k.entry == std::string("SchemeData")) return xu::Str(*o, "Name");
    const Value* w = RefField(*o, "Weapon");
    return w && w->bits && w->bits <= g.objs.size() ? xu::Str(g.objs[w->asRef() - 1], "Name") : std::string();
}

bool Printable(const std::string& s, size_t lo, size_t hi) { return erg::PrintableAscii(s, lo, hi); }

// The members of a JSON object must all be in `allowed`.
bool OnlyKeys(const json::Value& j, std::initializer_list<const char*> allowed, std::string* why) {
    for (const auto& [k, v] : j.members)
        if (std::none_of(allowed.begin(), allowed.end(), [&](const char* a) { return k == a; })) {
            *why = "unknown key '" + k + "'";
            return false;
        }
    return true;
}

bool StringKey(const json::Value& j, const char* name, size_t lo, size_t hi, std::string* out, std::string* why) {
    const json::Value* v = j.Get(name);
    if (!v || !v->IsString() || !Printable(v->string, lo, hi)) {
        *why = std::string(name) + " must be " + std::to_string(lo) + "-" + std::to_string(hi) + " printable ASCII characters";
        return false;
    }
    *out = v->string;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Scheme: {"key", "title", "base", "lock", "fields", "weapons"}
// ---------------------------------------------------------------------------------------------------------------
bool SetScalar(Value& f, const json::Value& j, std::string* why) {
    int64_t n = 0;
    switch (f.type) {
        case Type::Bool:
            if (!j.IsBool()) {
                *why = "must be true or false";
                return false;
            }
            f.bits = j.boolean ? 1 : 0;
            return true;
        case Type::I32:
            if (!Int(j, kI32Min, kI32Max, &n)) {
                *why = "must be an integer within int32";
                return false;
            }
            f.setInt(n);
            return true;
        case Type::U32:
            if (!Int(j, 0, UINT32_MAX, &n)) {
                *why = "must be an integer from 0 to 4294967295";
                return false;
            }
            f.setInt(n);
            return true;
        case Type::Enum:
            if (!Int(j, 0, 255, &n)) {
                *why = "must be an enum id from 0 to 255";
                return false;
            }
            f.setInt(n);
            return true;
        case Type::F32:
            if (!j.IsNumber() || !std::isfinite(j.number) || std::fabs(j.number) > 3.0e38) {
                *why = "must be a finite number";
                return false;
            }
            f.setFloat(j.number);
            return true;
        case Type::String:
            if (!j.IsString() || !Printable(j.string, 0, 127)) {
                *why = "must be 0-127 printable ASCII characters";
                return false;
            }
            f.str = j.string;
            return true;
        default:
            *why = "the field type cannot be set";
            return false;
    }
}

bool ApplyScheme(Graph& g, const Kind& k, const json::Value& j, std::set<std::string>& keys, Text* text, std::string* why) {
    if (!OnlyKeys(j, {"key", "title", "base", "lock", "fields", "weapons"}, why)) return false;
    std::string key, title, base = "FE.Scheme.Standard";
    const json::Value* kv = j.Get("key");
    if (!kv || !kv->IsString() || !ValidSchemeKey(kv->string)) {
        *why = "key must match FETXT.Scheme.<Name> (2-32 letters or digits, starting with a letter)";
        return false;
    }
    key = kv->string;
    if (!StringKey(j, "title", 1, 24, &title, why)) return false;
    if (const json::Value* b = j.Get("base"))
        if (!b->IsString() || b->string.empty()) {
            *why = "base must be the Name key of a built-in scheme";
            return false;
        } else {
            base = b->string;
        }
    std::string lock;
    const json::Value* lj = j.Get("lock");
    if (lj) {
        bool ok = lj->IsString() && Printable(lj->string, 1, 64) && IsAlpha(lj->string[0]);
        for (char c : lj->IsString() ? lj->string : std::string()) ok = ok && (IsAlnum(c) || c == '.' || c == '_');
        if (!ok) {
            *why = "lock must be a key of 1-64 letters, digits, '.' or '_'";
            return false;
        }
        lock = lj->string;
    }
    if (keys.count(key)) {
        *why = "the key " + key + " is already used";
        return false;
    }
    uint32_t baseRef = 0;
    for (const Value& r : g.List(k.list)->items)
        if (KeyOf(g, k, r.asRef()) == base) baseRef = r.asRef();
    if (!baseRef) {
        *why = "base " + base + " is not a built-in scheme";
        return false;
    }
    const uint32_t ref = g.Clone(baseRef);
    Object& s = g.objs[ref - 1];
    xu::SetStr(s, "Name", key);
    if (lj) xu::SetStr(s, "Lock", lock);
    if (const json::Value* f = j.Get("fields")) {
        if (!f->IsObject()) {
            *why = "fields must be an object";
            return false;
        }
        for (const auto& [name, v] : f->members) {
            Value* fv = s.field(name);
            if (!fv || fv->array || (fv->type != Type::I32 && fv->type != Type::Bool)) {
                *why = "fields." + name + " is not an integer or boolean field of SchemeData";
                return false;
            }
            std::string e;
            if (!SetScalar(*fv, v, &e)) {
                *why = "fields." + name + " " + e;
                return false;
            }
        }
    }
    if (Value* p = s.field("Permanent")) p->bits = 1;   // a built-in style: the game neither edits nor deletes it
    if (const json::Value* w = j.Get("weapons")) {
        if (!w->IsObject()) {
            *why = "weapons must be an object";
            return false;
        }
        std::map<std::string, uint32_t> entries;
        for (auto& [name, v] : s.fields)
            if (v.type == Type::Ref && !v.array && v.bits && v.bits <= g.objs.size() && g.objs[v.asRef() - 1].type == "WeaponSettingsData")
                entries[name] = static_cast<uint32_t>(v.bits);
        // "*" first, then the named entries in document order.
        std::vector<std::pair<std::string, const json::Value*>> order;
        if (const json::Value* all = w->Get("*")) order.push_back({"*", all});
        for (const auto& [name, v] : w->members)
            if (name != "*") order.push_back({name, &v});
        for (const auto& [name, v] : order) {
            if (name != "*" && !entries.count(name)) {
                *why = "weapons." + name + " is not a weapon of SchemeData";
                return false;
            }
            if (!v->IsObject()) {
                *why = "weapons." + name + " must be an object";
                return false;
            }
            for (const auto& [fname, fv] : v->members) {
                int64_t n = 0;
                if (fname != "Ammo" && fname != "Crate" && fname != "Delay") {
                    *why = "weapons." + name + ": unknown key '" + fname + "' (Ammo, Crate or Delay)";
                    return false;
                }
                if (!Int(fv, kI32Min, kI32Max, &n)) {
                    *why = "weapons." + name + "." + fname + " must be an integer within int32";
                    return false;
                }
                for (auto& [en, er] : entries) {
                    if (name != "*" && en != name) continue;
                    Value* t = g.objs[er - 1].field(fname);
                    if (!t || t->type != Type::I32) {
                        *why = "WeaponSettingsData has no " + fname + " field";
                        return false;
                    }
                    t->setInt(n);
                }
            }
        }
    }
    g.List(k.list)->items.push_back(xu::RefValue(ref));
    keys.insert(key);
    *text = {{}, key, title};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Factory weapon: {"key", "title", "base", "stock", "weapon", "cluster"}
// ---------------------------------------------------------------------------------------------------------------
bool Overrides(Object& o, const json::Value& j, const char* part, std::string* why) {
    if (!j.IsObject()) {
        *why = std::string(part) + " must be an object";
        return false;
    }
    for (const auto& [name, v] : j.members) {
        Value* f = o.field(name);
        std::string e;
        if (!f || name == "Name") {
            *why = std::string(part) + "." + name + (f ? " is set from the key" : " is not a field of WeaponFactoryContainer");
            return false;
        }
        if (f->array) {
            if (f->type != Type::String || !v.IsArray() || v.items.size() > 32) {
                *why = std::string(part) + "." + name + " must be an array of at most 32 strings";
                return false;
            }
            std::vector<Value> items;
            for (const json::Value& s : v.items) {
                Value it;
                it.type = Type::String;
                if (!s.IsString() || !Printable(s.string, 0, 127)) {
                    *why = std::string(part) + "." + name + " must hold printable ASCII strings";
                    return false;
                }
                it.str = s.string;
                items.push_back(std::move(it));
            }
            f->items = std::move(items);
        } else if (!SetScalar(*f, v, &e)) {
            *why = std::string(part) + "." + name + " " + e;
            return false;
        }
    }
    return true;
}

bool ApplyFactory(Graph& g, const Kind& k, const json::Value& j, std::set<std::string>& keys, Text* text, std::string* why) {
    if (!OnlyKeys(j, {"key", "title", "base", "stock", "weapon", "cluster"}, why)) return false;
    std::string key, title, base;
    const json::Value* kv = j.Get("key");
    if (!kv || !kv->IsString() || !ValidFactoryKey(kv->string)) {
        *why = "key must be FETXT.<Name>[.<Name>...] (6-40 characters, letters and digits, each part starting with a letter)";
        return false;
    }
    key = kv->string;
    if (!StringKey(j, "title", 1, 24, &title, why) || !StringKey(j, "base", 1, 79, &base, why)) return false;
    const json::Value* st = j.Get("stock");
    if (st && !st->IsBool()) {
        *why = "stock must be true or false";
        return false;
    }
    if (keys.count(key)) {
        *why = "the key " + key + " is already used";
        return false;
    }
    uint32_t baseRef = 0;
    for (const Value& r : g.List(k.list)->items)
        if (KeyOf(g, k, r.asRef()) == base) baseRef = r.asRef();
    if (!baseRef) {
        *why = "base " + base + " is not a built-in factory weapon";
        return false;
    }
    const uint32_t ref = g.Clone(baseRef);
    Value* sw = g.objs[ref - 1].field("StockWeapon");
    const Value* w = RefField(g.objs[ref - 1], "Weapon");
    const Value* c = RefField(g.objs[ref - 1], "Cluster");
    if (!sw || sw->type != Type::Bool || !w || !c || !w->bits || !c->bits) {
        *why = "the base preset has an unexpected shape";
        return false;
    }
    sw->bits = !st || st->boolean ? 1 : 0;
    Object& weapon = g.objs[w->asRef() - 1];
    xu::SetStr(weapon, "Name", key);
    if (const json::Value* o = j.Get("weapon"))
        if (!Overrides(weapon, *o, "weapon", why)) return false;
    if (const json::Value* o = j.Get("cluster"))
        if (!Overrides(g.objs[c->asRef() - 1], *o, "cluster", why)) return false;
    g.List(k.list)->items.push_back(xu::RefValue(ref));
    keys.insert(key);
    *text = {{}, key, title};
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
Built Build(const xom::Document& local, const std::vector<Source>& mods, const Kind& k, bool emitEmpty) {
    Built out;
    auto fatal = [&](std::string why) {
        out.fatal = std::move(why);
        return out;
    };
    const Object *wrap = nullptr, *bank = nullptr;
    uint32_t coll = 0;
    for (const auto& o : local.objects) {
        if (!bank && o.type == "XDataBank") bank = &o;
        if (!wrap && o.type == "XContainerResourceDetails" && xu::Str(o, "Name") == k.wrapper) wrap = &o;
    }
    if (wrap) {
        const Value* v = RefField(*wrap, "Value");
        coll = v ? static_cast<uint32_t>(v->bits) : 0;
    }
    if (!wrap || !bank || !local.object(coll) || local.object(coll)->type != k.collective)
        return fatal(std::string(k.wrapper) + " was not found in LOCAL.XOM");
    const Value* list = local.object(coll)->field(k.list);
    if (!list || list->type != Type::Ref || !list->array) return fatal(std::string(k.wrapper) + " has an unexpected shape");

    // The objects reachable from the collective, in LOCAL's order.
    std::set<uint32_t> seen;
    std::vector<uint32_t> todo = {coll};
    while (!todo.empty()) {
        const uint32_t r = todo.back();
        todo.pop_back();
        if (!local.object(r) || !seen.insert(r).second) continue;
        if (local.object(r)->opaque) return fatal(std::string(k.wrapper) + " holds an object that could not be decoded");
        for (const auto& f : local.object(r)->fields)
            Each(f.second, [&](const Value& v) {
                if (v.type == Type::Ref && !v.array && v.bits) todo.push_back(static_cast<uint32_t>(v.bits));
            });
    }
    Graph g;
    std::map<uint32_t, uint32_t> renum;
    g.objs.push_back(*wrap);
    g.objs.push_back(*bank);
    for (uint32_t r : seen) {
        g.objs.push_back(*local.object(r));
        renum[r] = static_cast<uint32_t>(g.objs.size());
    }
    g.collective = renum[coll];
    for (size_t i = 2; i < g.objs.size(); ++i)
        for (auto& f : g.objs[i].fields)
            Each(f.second, [&](Value& v) {
                if (v.type == Type::Ref && !v.array && v.bits) v.bits = renum.count(static_cast<uint32_t>(v.bits)) ? renum[static_cast<uint32_t>(v.bits)] : 0;
            });
    g.objs[0].field("Value")->bits = g.collective;
    for (auto& [name, v] : g.objs[1].fields)
        if (v.array) {
            v.items.clear();
            v.raw.clear();
        }
    Value* section = g.objs[1].field("Section");
    Value* containers = g.objs[1].field("ContainerResources");
    if (!section || !containers || containers->type != Type::Ref) return fatal("LOCAL.XOM: unexpected data bank shape");
    section->setInt(0);
    containers->items.push_back(xu::RefValue(1));

    std::set<std::string> keys;
    for (const Value& r : g.List(k.list)->items) keys.insert(KeyOf(g, k, r.asRef()));
    out.builtIn = static_cast<int>(g.List(k.list)->items.size());

    for (const Source& s : mods)
        for (const std::string& file : s.files) {
            auto err = [&](const std::string& why) { out.errors.push_back({s.mod, file, why}); };
            if (out.added.size() >= k.maxTotal) {
                err(std::string("more than ") + std::to_string(k.maxTotal) + " " + k.what + " across all enabled mods");
                continue;
            }
            json::Value j;
            std::string why;
            if (!LoadJson(s, file, &j, &why)) {
                err(why);
                continue;
            }
            const size_t mark = g.objs.size();
            const size_t listed = g.List(k.list)->items.size();
            Text text;
            const bool ok = k.entry == std::string("SchemeData") ? ApplyScheme(g, k, j, keys, &text, &why)
                                                                  : ApplyFactory(g, k, j, keys, &text, &why);
            if (!ok) {
                g.objs.resize(mark);
                g.List(k.list)->items.resize(listed);
                err(why);
                continue;
            }
            text.mod = s.mod;
            out.added.push_back(std::move(text));
        }
    if (out.added.empty() && !emitEmpty) return out;

    // The document: TYPE entries for the classes in use (LOCAL's order), objects grouped by them, wrapper #1 and the
    // data bank #2 (the shape of the level registry's banks).
    xom::Document d;
    d.version = local.version;
    d.reserved08 = local.reserved08;
    d.reserved24 = local.reserved24;
    d.guidRec = local.guidRec;
    d.schmRec = local.schmRec;
    d.guidRecord = local.guidRecord;
    std::set<std::string> used = {"XContainer", "XResourceDetails"};
    for (const auto& o : g.objs) used.insert(o.type);
    for (const auto& t : local.types)
        if (used.count(t.className())) {
            d.types.push_back(t);
            d.types.back().count = 0;
        }
    auto rank = [&](const Object& o) {
        for (size_t i = 0; i < d.types.size(); ++i)
            if (d.types[i].className() == o.type) return i;
        return d.types.size();
    };
    std::vector<uint32_t> order(g.objs.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<uint32_t>(i);
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return rank(g.objs[a]) < rank(g.objs[b]); });
    std::vector<uint32_t> at(g.objs.size() + 1, 0);
    for (size_t i = 0; i < order.size(); ++i) at[order[i] + 1] = static_cast<uint32_t>(i + 1);
    std::set<std::string> strings;
    for (uint32_t i : order) {
        Object o = std::move(g.objs[i]);
        for (auto& f : o.fields)
            Each(f.second, [&](Value& v) {
                if (v.type == Type::Ref && !v.array && v.bits) v.bits = at[v.asRef()];
                if (v.type == Type::String && !v.array) strings.insert(v.str);
            });
        if (o.type == "XDataBank") d.root = static_cast<uint32_t>(d.objects.size() + 1);
        d.objects.push_back(std::move(o));
    }
    strings.erase("");
    d.strings.push_back("");
    d.strings.insert(d.strings.end(), strings.begin(), strings.end());

    std::string e;
    if (!xom::serialize(d, out.bank, &e)) {
        out.bank.clear();
        return fatal(std::string(k.wrapper) + " bank: " + e);
    }
    xom::Document check;
    xom::ParseOptions opt;
    opt.strict = true;
    if (!xom::parse(out.bank.data(), out.bank.size(), check, &e, opt) || !check.object(check.root) ||
        check.object(check.root)->type != "XDataBank") {
        out.bank.clear();
        return fatal(std::string(k.wrapper) + " bank: the output does not parse back");
    }
    return out;
}
}  // namespace

bool ValidSchemeKey(const std::string& key) {
    static const std::string kPrefix = "FETXT.Scheme.";
    if (key.size() < kPrefix.size() + 2 || key.size() > kPrefix.size() + 32 || key.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    if (!IsAlpha(key[kPrefix.size()])) return false;
    return std::all_of(key.begin() + kPrefix.size(), key.end(), IsAlnum);
}

bool ValidFactoryKey(const std::string& key) {
    if (key.size() < 6 || key.size() > 40 || key.compare(0, 6, "FETXT.") != 0) return false;
    size_t i = 6;
    while (i <= key.size()) {
        size_t j = key.find('.', i);
        if (j == std::string::npos) j = key.size();
        if (j == i || !IsAlpha(key[i])) return false;
        for (size_t p = i; p < j; ++p)
            if (!IsAlnum(key[p])) return false;
        i = j + 1;
    }
    return true;
}

Built BuildSchemes(const xom::Document& local, const std::vector<Source>& mods, bool emitEmpty) {
    return Build(local, mods, kSchemes, emitEmpty);
}
Built BuildFactoryWeapons(const xom::Document& local, const std::vector<Source>& mods, bool emitEmpty) {
    return Build(local, mods, kFactory, emitEmpty);
}
}  // namespace melange::schemes
