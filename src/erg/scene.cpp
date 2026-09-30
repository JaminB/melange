#include "erg/scene.h"

#include <cctype>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "erg/jsonio.h"
#include "erg/names.h"

namespace melange::erg {
namespace {
using namespace jsonio;

constexpr const char* kRoles[] = {"scenery", "spawn", "object", "camera", "light",
                                  "emitter", "sound", "collision", "marker", "other"};
constexpr const char* kThemes[] = {"ARABIAN", "WILDWEST", "CAMELOT", "PREHISTORIC", "BUILDING", "ARCTIC",
                                   "ENGLAND", "HORROR", "LUNAR", "PIRATE", "WAR"};
constexpr const char* kTimes[] = {"DAY", "EVENING", "NIGHT"};
constexpr size_t kMaxSceneBytes = 16u << 20;

std::string Upper(std::string_view s) {
    std::string u(s);
    for (auto& c : u) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return u;
}

bool Contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool ParseBase(const Json& o, BaseRef* b, bool withFile, std::string* err) {
    if (!OnlyKeys(o, {"key", "source", "file", "sha256"}, "base", err)) return false;
    if (!GetString(o, "key", "base", &b->key, err) || !GetString(o, "source", "base", &b->source, err)) return false;
    if (!PrintableAscii(b->key, 1, 79)) return Fail(err, "base.key: must be 1-79 printable characters");
    if (b->source != "game" && b->source != "pack") return Fail(err, "base.source: must be \"game\" or \"pack\"");
    if (withFile && !GetString(o, "file", "base", &b->file, err)) return false;
    if (!withFile && o.find("file")) return Fail(err, "base.file: not part of a patch");
    if (!b->file.empty() && !PrintableAscii(b->file, 1, 63)) return Fail(err, "base.file: must be 1-63 printable characters");
    const Json* sha = o.find("sha256");
    if (!Obj(sha, "base.sha256", err) || !OnlyKeys(*sha, {"xan", "xom", "hmp"}, "base.sha256", err)) return false;
    if (!GetString(*sha, "xan", "base.sha256", &b->sha256.xan, err) ||
        !GetString(*sha, "xom", "base.sha256", &b->sha256.xom, err))
        return false;
    if (const Json* h = sha->find("hmp"); h && h->kind != Json::Kind::Null && !GetString(*sha, "hmp", "base.sha256", &b->sha256.hmp, err))
        return false;
    if (!IsHex64(b->sha256.xan) || !IsHex64(b->sha256.xom) || (!b->sha256.hmp.empty() && !IsHex64(b->sha256.hmp)))
        return Fail(err, "base.sha256: each hash must be 64 lower-case hex digits");
    return true;
}

Json BaseJson(const BaseRef& b, bool withFile) {
    Json o = Json::Obj();
    o.set("key", Str(b.key));
    o.set("source", Str(b.source));
    if (withFile) o.set("file", Str(b.file));
    Json sha = Json::Obj();
    sha.set("xan", Str(b.sha256.xan));
    sha.set("xom", Str(b.sha256.xom));
    sha.set("hmp", b.sha256.hmp.empty() ? Json::Null_() : Str(b.sha256.hmp));
    o.set("sha256", std::move(sha));
    return o;
}

bool ParseDatabankField(const Json& o, const char* key, std::string* out, std::string* err) {
    if (!GetString(o, key, "databank", out, err, false)) return false;
    return true;
}

constexpr const char* kObjectTypes[] = {"crate", "telepad", "trigger", "minefactory"};
constexpr const char* kCrateKinds[] = {"weapon", "health", "utility"};

// "<prefix><n>" with n 0-255 written without leading zeros.
bool IndexAfter(std::string_view s, std::string_view prefix) {
    if (s.size() <= prefix.size() || s.size() > prefix.size() + 3 || s.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string_view d = s.substr(prefix.size());
    if (d.size() > 1 && d[0] == '0') return false;
    int v = 0;
    for (char c : d) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    return v <= 255;
}

bool IntField(const Json& o, const char* key, const std::string& p, int64_t lo, int64_t hi, int* out, std::string* err,
              bool required) {
    if (!required && !o.find(key)) return true;
    int64_t v = 0;
    if (!GetInt(o, key, p, lo, hi, &v, err)) return false;
    *out = static_cast<int>(v);
    return true;
}
}  // namespace

bool IsObjectKnot(std::string_view n) {
    if (n == "minefactory" || IndexAfter(n, "CRATE_") || IndexAfter(n, "TRIG_")) return true;
    return n.size() > 5 && n[3] >= '1' && n[3] <= '8' && IndexAfter(n, std::string("TP_") + n[3] + "_");
}

const char* ObjectTypeName(ObjectType t) { return kObjectTypes[static_cast<int>(t)]; }
const char* CrateKindName(CrateKind k) { return kCrateKinds[static_cast<int>(k)]; }

bool ParseObjectType(std::string_view s, ObjectType* out) {
    for (int i = 0; i < 4; ++i)
        if (s == kObjectTypes[i]) {
            *out = static_cast<ObjectType>(i);
            return true;
        }
    return false;
}

bool ParseCrateKind(std::string_view s, CrateKind* out) {
    for (int i = 0; i < 3; ++i)
        if (s == kCrateKinds[i]) {
            *out = static_cast<CrateKind>(i);
            return true;
        }
    return false;
}

bool ValidKnot(std::string_view knot, ObjectType type, int group) {
    switch (type) {
        case ObjectType::Crate: return IndexAfter(knot, "CRATE_");
        case ObjectType::Trigger: return IndexAfter(knot, "TRIG_");
        case ObjectType::MineFactory: return knot == "minefactory";
        case ObjectType::Telepad:
            if (group < 1 || group > static_cast<int>(kMaxTelepadGroups)) return false;
            return IndexAfter(knot, "TP_" + std::to_string(group) + "_");
    }
    return false;
}

bool ValidContentsName(std::string_view s) {
    if (s.empty() || s.size() > 63 || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

bool ValidateObject(const ObjectSpec& o, const std::string& p, std::string* err) {
    if (!ValidKnot(o.knot, o.type, o.group))
        return Fail(err, p + ".knot: '" + o.knot + "' is not a " + ObjectTypeName(o.type) + " knot name");
    switch (o.type) {
        case ObjectType::Crate: {
            const CrateSpec& c = o.crate;
            if (c.kind == CrateKind::Health) {
                if (c.amount < 1 || c.amount > 500) return Fail(err, p + ".crate.amount: must be 1-500");
            } else {
                if (!ValidContentsName(c.contents)) return Fail(err, p + ".crate.contents: must be a weapon or utility name");
                if (c.count < 1 || c.count > 99) return Fail(err, p + ".crate.count: must be 1-99");
            }
            if (c.hitpoints < 1 || c.hitpoints > 1000) return Fail(err, p + ".crate.hitpoints: must be 1-1000");
            break;
        }
        case ObjectType::Trigger: {
            const TriggerSpec& t = o.trigger;
            if (t.index < 0 || t.index > 255) return Fail(err, p + ".trigger.index: must be 0-255");
            if (!std::isfinite(t.radius) || t.radius < 1 || t.radius > 1000) return Fail(err, p + ".trigger.radius: must be 1-1000");
            if (t.teamCollect < 0 || t.teamCollect > 8 || t.teamDestroy < 0 || t.teamDestroy > 8)
                return Fail(err, p + ".trigger: teams must be 0-8");
            if (t.hitpoints < 0 || t.hitpoints > 1000) return Fail(err, p + ".trigger.hitpoints: must be 0-1000");
            break;
        }
        case ObjectType::Telepad:
        case ObjectType::MineFactory: break;
    }
    return true;
}

namespace jsonio {
bool GetBool(const Json& o, const char* key, const std::string& path, bool* out, std::string* err, bool required) {
    const Json* v = o.find(key);
    if (!v) return !required || Fail(err, path + "." + key + ": is required");
    if (v->kind != Json::Kind::Bool) return Fail(err, path + "." + key + ": must be a boolean");
    *out = v->boolean;
    return true;
}

bool ParseObjects(const Json* arr, std::vector<ObjectSpec>* out, std::string* err) {
    if (!arr || arr->kind != Json::Kind::Array || arr->arr.size() > kMaxObjects)
        return Fail(err, "objects: must be an array of at most 256 objects");
    for (size_t i = 0; i < arr->arr.size(); ++i) {
        const std::string p = "objects[" + std::to_string(i) + "]";
        const Json& o = arr->arr[i];
        if (!Obj(&o, p, err)) return false;
        ObjectSpec ob;
        std::string type;
        if (!GetString(o, "type", p, &type, err) || !GetString(o, "knot", p, &ob.knot, err)) return false;
        if (!ParseObjectType(type, &ob.type)) return Fail(err, p + ".type: must be crate, telepad, trigger or minefactory");
        switch (ob.type) {
            case ObjectType::Crate: {
                if (!OnlyKeys(o, {"knot", "type", "crate"}, p, err)) return false;
                const Json* c = o.find("crate");
                const std::string cp = p + ".crate";
                std::string kind;
                if (!Obj(c, cp, err) || !GetString(*c, "kind", cp, &kind, err)) return false;
                if (!ParseCrateKind(kind, &ob.crate.kind)) return Fail(err, cp + ".kind: must be weapon, health or utility");
                if (ob.crate.kind == CrateKind::Health) {
                    if (!OnlyKeys(*c, {"kind", "amount", "hitpoints", "parachute"}, cp, err) ||
                        !IntField(*c, "amount", cp, 1, 500, &ob.crate.amount, err, true))
                        return false;
                } else {
                    if (!OnlyKeys(*c, {"kind", "contents", "count", "hitpoints", "parachute"}, cp, err) ||
                        !GetString(*c, "contents", cp, &ob.crate.contents, err) ||
                        !IntField(*c, "count", cp, 1, 99, &ob.crate.count, err, false))
                        return false;
                }
                if (!IntField(*c, "hitpoints", cp, 1, 1000, &ob.crate.hitpoints, err, false) ||
                    !GetBool(*c, "parachute", cp, &ob.crate.parachute, err, false))
                    return false;
                break;
            }
            case ObjectType::Telepad:
                if (!OnlyKeys(o, {"knot", "type", "group"}, p, err) ||
                    !IntField(o, "group", p, 1, kMaxTelepadGroups, &ob.group, err, true))
                    return false;
                break;
            case ObjectType::Trigger: {
                if (!OnlyKeys(o, {"knot", "type", "trigger"}, p, err)) return false;
                const Json* t = o.find("trigger");
                const std::string tp = p + ".trigger";
                if (!Obj(t, tp, err) ||
                    !OnlyKeys(*t, {"index", "radius", "teamCollect", "teamDestroy", "hitpoints", "wormCollect"}, tp, err) ||
                    !IntField(*t, "index", tp, 0, 255, &ob.trigger.index, err, true) ||
                    !IntField(*t, "teamCollect", tp, 0, 8, &ob.trigger.teamCollect, err, false) ||
                    !IntField(*t, "teamDestroy", tp, 0, 8, &ob.trigger.teamDestroy, err, false) ||
                    !IntField(*t, "hitpoints", tp, 0, 1000, &ob.trigger.hitpoints, err, false) ||
                    !GetBool(*t, "wormCollect", tp, &ob.trigger.wormCollect, err, false))
                    return false;
                if (const Json* r = t->find("radius"); r && !GetNumber(*r, tp + ".radius", 1, 1000, &ob.trigger.radius, err))
                    return false;
                break;
            }
            case ObjectType::MineFactory:
                if (!OnlyKeys(o, {"knot", "type"}, p, err)) return false;
                break;
        }
        if (!ValidateObject(ob, p, err)) return false;
        out->push_back(std::move(ob));
    }
    return true;
}

Json ObjectsJson(const std::vector<ObjectSpec>& objects) {
    Json arr = Json::Arr();
    for (const auto& ob : objects) {
        Json o = Json::Obj();
        o.set("knot", Str(ob.knot));
        o.set("type", Str(ObjectTypeName(ob.type)));
        switch (ob.type) {
            case ObjectType::Crate: {
                Json c = Json::Obj();
                c.set("kind", Str(CrateKindName(ob.crate.kind)));
                if (ob.crate.kind == CrateKind::Health) {
                    c.set("amount", Int(ob.crate.amount));
                } else {
                    c.set("contents", Str(ob.crate.contents));
                    c.set("count", Int(ob.crate.count));
                }
                c.set("hitpoints", Int(ob.crate.hitpoints));
                c.set("parachute", Json::Bool(ob.crate.parachute));
                o.set("crate", std::move(c));
                break;
            }
            case ObjectType::Telepad: o.set("group", Int(ob.group)); break;
            case ObjectType::Trigger: {
                Json t = Json::Obj();
                t.set("index", Int(ob.trigger.index));
                t.set("radius", Num(ob.trigger.radius));
                t.set("teamCollect", Int(ob.trigger.teamCollect));
                t.set("teamDestroy", Int(ob.trigger.teamDestroy));
                t.set("hitpoints", Int(ob.trigger.hitpoints));
                t.set("wormCollect", Json::Bool(ob.trigger.wormCollect));
                o.set("trigger", std::move(t));
                break;
            }
            case ObjectType::MineFactory: break;
        }
        arr.arr.push_back(std::move(o));
    }
    return arr;
}

bool ParseKind(const Json* o, bool* survivor, std::string* err) {
    if (!Obj(o, "kind", err) || !OnlyKeys(*o, {"survivor"}, "kind", err)) return false;
    return GetBool(*o, "survivor", "kind", survivor, err, false);
}

Json KindJson(bool survivor) {
    Json o = Json::Obj();
    o.set("survivor", Json::Bool(survivor));
    return o;
}

bool ParseScript(const Json* o, ScriptMeta* out, std::string* err) {
    if (!Obj(o, "script", err) || !OnlyKeys(*o, {"present", "sha256"}, "script", err) ||
        !GetBool(*o, "present", "script", &out->present, err))
        return false;
    if (const Json* h = o->find("sha256"); h && h->kind != Json::Kind::Null) {
        if (!GetString(*o, "sha256", "script", &out->sha256, err)) return false;
        if (!IsHex64(out->sha256)) return Fail(err, "script.sha256: must be 64 lower-case hex digits");
    }
    if (out->present == out->sha256.empty()) return Fail(err, "script: sha256 is set exactly when present is true");
    return true;
}

Json ScriptJson(const ScriptMeta& m) {
    Json o = Json::Obj();
    o.set("present", Json::Bool(m.present));
    o.set("sha256", m.sha256.empty() ? Json::Null_() : Str(m.sha256));
    return o;
}
}  // namespace jsonio

const char* RoleName(Role r) { return kRoles[static_cast<int>(r)]; }

bool ParseRole(std::string_view s, Role* out) {
    for (int i = 0; i < 10; ++i)
        if (s == kRoles[i]) {
            *out = static_cast<Role>(i);
            return true;
        }
    return false;
}

// Name tokens and ResourceNames as the shipped levels use them; the case is folded (both spellings ship).
Role DeriveRole(std::string_view name, std::string_view resource) {
    const std::string n = Upper(name), r = Upper(resource);
    if (IsObjectKnot(name)) return Role::Object;
    if (n.size() >= 5 && n.compare(0, 4, "WORM") == 0 && std::isdigit(static_cast<unsigned char>(n[4]))) return Role::Spawn;
    if (r == "CHEESYGRINWORM") return Role::Spawn;
    if (n == "MINE" || n == "OILDRUM" || n == "MINEFACTORY" || n == "TELEPAD" || Contains(r, "MINE") || Contains(r, "OILDRUM") ||
        Contains(r, "OIL DRUM") || r == "TELEPAD" || Contains(r, "CRATE") || r == "TARGET")
        return Role::Object;
    if (r == "CAMERA" || r == "LOOKAT POINT") return Role::Camera;
    if (r == "LIGHT" || Contains(n, "PNTLGHT")) return Role::Light;
    if (Contains(r, "PARTICLE EMITTER") || Contains(n, "EMITTER")) return Role::Emitter;
    if (r == "SOUND EFFECT") return Role::Sound;
    if (Contains(r, "COLLISION SPHERE") || Contains(n, "RAND SPAWN VOLUME")) return Role::Collision;
    if (r == "AXIS" || r == "UNIT" || r == "WATER" || Contains(n, "STATUE SPAWN")) return Role::Marker;
    if (Contains(n, "VISIBLE") || Contains(n, "PROP") || Contains(n, "STANDIN")) return Role::Scenery;
    return Role::Other;
}

const char* SpawnModeName(SpawnMode m) { return m == SpawnMode::Knots ? "knots" : "random"; }
const char* HmpModeName(HmpMode m) {
    return m == HmpMode::None ? "none" : m == HmpMode::Flat ? "flat" : m == HmpMode::Paint ? "paint" : "copy";
}

bool ParseSpawnMode(std::string_view s, SpawnMode* out) {
    if (s == "random") *out = SpawnMode::Random;
    else if (s == "knots") *out = SpawnMode::Knots;
    else return false;
    return true;
}

bool ParseHmpMode(std::string_view s, HmpMode* out) {
    if (s == "copy") *out = HmpMode::Copy;
    else if (s == "none") *out = HmpMode::None;
    else if (s == "flat") *out = HmpMode::Flat;
    else if (s == "paint") *out = HmpMode::Paint;
    else return false;
    return true;
}

bool ValidTheme(std::string_view theme) {
    for (auto t : kThemes)
        if (theme == t) return true;
    return false;
}

bool ValidTimeOfDay(std::string_view tod) {
    for (auto t : kTimes)
        if (tod == t) return true;
    return false;
}

bool PrintableAscii(std::string_view s, size_t minLen, size_t maxLen) {
    if (s.size() < minLen || s.size() > maxLen) return false;
    for (unsigned char c : s)
        if (c < 0x20 || c > 0x7e) return false;
    return true;
}

bool DatabankShape(const DatabankInfo& d, bool partial, std::string* err) {
    if ((!partial || !d.theme.empty()) && !ValidTheme(d.theme)) return Fail(err, "databank.theme: not one of the eleven themes");
    if ((!partial || !d.timeOfDay.empty()) && !ValidTimeOfDay(d.timeOfDay))
        return Fail(err, "databank.timeOfDay: must be DAY, EVENING or NIGHT");
    if (!d.materialFile.empty()) {
        const std::string& m = d.materialFile;
        if (!PrintableAscii(m, 1, 120) || m.find("..") != std::string::npos || m.find(':') != std::string::npos ||
            m[0] == '/' || m[0] == '\\' || Upper(m).size() < 4 || Upper(m).compare(m.size() - 4, 4, ".TXT") != 0)
            return Fail(err, "databank.materialFile: must be a relative .txt path inside the install");
    }
    for (const std::string* t : {&d.heightmapBase, &d.heightmapSecond})
        if (!t->empty() && !PrintableAscii(*t, 1, 64)) return Fail(err, "databank.heightmap*: must be 1-64 printable characters");
    return true;
}

const Frame* Scene::FindFrame(int64_t id) const {
    for (auto& f : frames)
        if (f.id == id) return &f;
    return nullptr;
}

const Detail* Scene::FindDetailBySrc(int64_t src) const {
    for (auto& d : details)
        if (d.src && *d.src == src) return &d;
    return nullptr;
}

bool Scene::UsesV2() const {
    if (survivor || !objects.empty() || script.present || hmp == HmpMode::Paint || hmpRef >= 0) return true;
    for (auto& f : frames)
        if (f.isNew) return true;
    for (auto& b : blobs)
        if (b.kind == "hmp") return true;
    return false;
}

bool UnderSceneFrame(const Scene& s, int64_t frameId) {
    int64_t cur = frameId;
    for (size_t hops = 0; cur >= 0 && hops <= s.frames.size(); ++hops) {
        const Frame* f = s.FindFrame(cur);
        if (!f) return false;
        if (f->name == "Scene") return true;
        cur = f->parent;
    }
    return false;
}

bool ParseScene(std::string_view json, Scene* out, std::string* err) {
    if (json.size() > kMaxSceneBytes) return Fail(err, "the scene is larger than 16 MB");
    Json root;
    std::string perr;
    if (!xom::ParseJson(json, root, &perr)) return Fail(err, "not JSON: " + perr);
    if (!Obj(&root, "scene", err)) return false;
    Scene s;
    std::string format;
    if (!GetString(root, "format", "scene", &format, err)) return false;
    if (format != kSceneFormat && format != kSceneFormat2)
        return Fail(err, "scene.format: must be \"erg-scene/1\" or \"erg-scene/2\" (a newer format needs a newer Melange)");
    const bool v2 = format == kSceneFormat2;
    if (v2 ? !OnlyKeys(root, {"format", "stem", "title", "base", "registry", "kind", "databank", "water", "spawns", "hmp",
                              "units", "frames", "details", "blobs", "objects", "script"},
                       "scene", err)
           : !OnlyKeys(root, {"format", "stem", "title", "base", "registry", "databank", "water", "spawns", "hmp", "units",
                              "frames", "details", "blobs"},
                       "scene", err))
        return false;
    if (!GetString(root, "stem", "scene", &s.stem, err) || !GetString(root, "title", "scene", &s.title, err)) return false;
    if (!Obj(root.find("base"), "base", err) || !ParseBase(*root.find("base"), &s.base, true, err)) return false;

    const Json* reg = root.find("registry");
    if (!Obj(reg, "registry", err) || !OnlyKeys(*reg, {"levelType", "themeType", "scripts", "previewType"}, "registry", err))
        return false;
    int64_t n = 0;
    if (!GetInt(*reg, "levelType", "registry", 0, 16, &n, err)) return false;
    s.registry.levelType = static_cast<int>(n);
    if (!GetInt(*reg, "themeType", "registry", 0, 11, &n, err)) return false;
    s.registry.themeType = static_cast<int>(n);
    if (reg->find("previewType")) {
        if (!GetInt(*reg, "previewType", "registry", 0, 16, &n, err)) return false;
        s.registry.previewType = static_cast<int>(n);
    }
    const Json* scripts = reg->find("scripts");
    if (!scripts || scripts->kind != Json::Kind::Array || scripts->arr.empty() || scripts->arr.size() > 8)
        return Fail(err, "registry.scripts: must be 1-8 names");
    s.registry.scripts.clear();
    for (auto& v : scripts->arr) {
        if (v.kind != Json::Kind::String || !PrintableAscii(v.str, 1, 63) || v.str.find(',') != std::string::npos)
            return Fail(err, "registry.scripts: each name must be 1-63 printable characters without ','");
        s.registry.scripts.push_back(v.str);
    }

    const Json* db = root.find("databank");
    if (!Obj(db, "databank", err) ||
        !OnlyKeys(*db, {"theme", "timeOfDay", "materialFile", "heightmapBase", "heightmapSecond"}, "databank", err))
        return false;
    if (!ParseDatabankField(*db, "theme", &s.databank.theme, err) || !ParseDatabankField(*db, "timeOfDay", &s.databank.timeOfDay, err) ||
        !ParseDatabankField(*db, "materialFile", &s.databank.materialFile, err) ||
        !ParseDatabankField(*db, "heightmapBase", &s.databank.heightmapBase, err) ||
        !ParseDatabankField(*db, "heightmapSecond", &s.databank.heightmapSecond, err))
        return false;

    const Json* water = root.find("water");
    if (!Obj(water, "water", err) || !OnlyKeys(*water, {"level"}, "water", err)) return false;
    if (const Json* lv = water->find("level"); lv && lv->kind != Json::Kind::Null) {
        double w = 0;
        if (!GetNumber(*lv, "water.level", -1000, 1000, &w, err)) return false;
        s.water = w;
    }
    std::string mode;
    const Json* sp = root.find("spawns");
    if (!Obj(sp, "spawns", err) || !OnlyKeys(*sp, {"mode"}, "spawns", err) || !GetString(*sp, "mode", "spawns", &mode, err))
        return false;
    if (!ParseSpawnMode(mode, &s.spawns)) return Fail(err, "spawns.mode: must be \"random\" or \"knots\"");
    const Json* hmp = root.find("hmp");
    if (!Obj(hmp, "hmp", err) || !(v2 ? OnlyKeys(*hmp, {"mode", "ref"}, "hmp", err) : OnlyKeys(*hmp, {"mode"}, "hmp", err)) ||
        !GetString(*hmp, "mode", "hmp", &mode, err))
        return false;
    if (!ParseHmpMode(mode, &s.hmp) || (!v2 && s.hmp == HmpMode::Paint))
        return Fail(err, v2 ? "hmp.mode: must be \"copy\", \"none\", \"flat\" or \"paint\""
                            : "hmp.mode: must be \"copy\", \"none\" or \"flat\"");
    if (const Json* r = hmp->find("ref"); r && r->kind != Json::Kind::Null && !GetInt(*hmp, "ref", "hmp", 0, 1 << 24, &s.hmpRef, err))
        return false;
    if (v2) {
        if (const Json* k = root.find("kind"); k && !ParseKind(k, &s.survivor, err)) return false;
        if (const Json* o = root.find("objects"); o && !ParseObjects(o, &s.objects, err)) return false;
        if (const Json* sc = root.find("script"); sc && !ParseScript(sc, &s.script, err)) return false;
    }
    const Json* units = root.find("units");
    if (!Obj(units, "units", err) || !OnlyKeys(*units, {"worldPerXan"}, "units", err) ||
        !GetInt(*units, "worldPerXan", "units", kWorldPerXan, kWorldPerXan, &n, err))
        return false;

    const Json* frames = root.find("frames");
    if (!frames || frames->kind != Json::Kind::Array || frames->arr.size() > kMaxFrames)
        return Fail(err, "frames: must be an array of at most 4096 frames");
    for (size_t i = 0; i < frames->arr.size(); ++i) {
        const std::string p = "frames[" + std::to_string(i) + "]";
        const Json& fo = frames->arr[i];
        if (!Obj(&fo, p, err) ||
            !(v2 ? OnlyKeys(fo, {"id", "new", "parent", "name", "pos", "rot", "scale", "size", "voxels", "heightMap", "folder"}, p, err)
                 : OnlyKeys(fo, {"id", "parent", "name", "pos", "rot", "scale", "size", "voxels", "heightMap", "folder"}, p, err)))
            return false;
        Frame f;
        if (v2 && !GetBool(fo, "new", p, &f.isNew, err, false)) return false;
        if (f.isNew ? !GetInt(fo, "id", p, -static_cast<int64_t>(kMaxNewFrames), -1, &f.id, err)
                    : !GetInt(fo, "id", p, 1, 1 << 24, &f.id, err))
            return false;
        const Json* par = fo.find("parent");
        if (par && par->kind != Json::Kind::Null && !GetInt(fo, "parent", p, 1, 1 << 24, &f.parent, err)) return false;
        if (!GetString(fo, "name", p, &f.name, err)) return false;
        if (f.name.size() > 127) return Fail(err, p + ".name: at most 127 bytes");
        if (!GetVec(fo, "pos", p, &f.pos, err, true) || !GetVec(fo, "rot", p, &f.rot, err, true, 1e3) ||
            !GetVec(fo, "scale", p, &f.scale, err, true))
            return false;
        const Json* size = fo.find("size");
        if (!size || size->kind != Json::Kind::Array || size->arr.size() != 3) return Fail(err, p + ".size: must be 3 integers");
        for (int k = 0; k < 3; ++k) {
            bool ok = false;
            const int64_t v = size->arr[k].kind == Json::Kind::Number ? size->arr[k].asInt64(&ok) : -1;
            if (!ok || v < 0 || v > 255) return Fail(err, p + ".size: each size must be 0..255");
            f.size[k] = static_cast<int>(v);
        }
        if (static_cast<size_t>(f.size[0]) * f.size[1] * f.size[2] > kMaxFrameVoxels)
            return Fail(err, p + ".size: more than 262144 voxels");
        for (auto [key, dst] : {std::pair{"voxels", &f.voxels}, std::pair{"heightMap", &f.heightMap}}) {
            const Json* r = fo.find(key);
            if (r && r->kind != Json::Kind::Null && !GetInt(fo, key, p, 0, 1 << 24, dst, err)) return false;
        }
        const Json* folder = fo.find("folder");
        if (!folder || folder->kind != Json::Kind::Bool) return Fail(err, p + ".folder: must be a boolean");
        f.folder = folder->boolean;
        s.frames.push_back(std::move(f));
    }

    const Json* details = root.find("details");
    if (!details || details->kind != Json::Kind::Array || details->arr.size() > kMaxDetails)
        return Fail(err, "details: must be an array of at most 65536 details");
    for (size_t i = 0; i < details->arr.size(); ++i) {
        const std::string p = "details[" + std::to_string(i) + "]";
        const Json& dob = details->arr[i];
        if (!Obj(&dob, p, err) ||
            !OnlyKeys(dob, {"id", "src", "frame", "name", "resource", "pos", "rot", "scale", "voxelPos", "role"}, p, err))
            return false;
        Detail d;
        if (!GetInt(dob, "id", p, 1, 1 << 24, &d.id, err)) return false;
        const Json* src = dob.find("src");
        if (!src) return Fail(err, p + ".src: is required (null for an added detail)");
        if (src->kind != Json::Kind::Null) {
            int64_t v = 0;
            if (!GetInt(dob, "src", p, 1, 1 << 24, &v, err)) return false;
            d.src = v;
        }
        if (!GetInt(dob, "frame", p, 1, 1 << 24, &d.frame, err) || !GetString(dob, "name", p, &d.name, err) ||
            !GetString(dob, "resource", p, &d.resource, err))
            return false;
        if (d.name.size() > 127 || d.resource.size() > 127) return Fail(err, p + ": name and resource are at most 127 bytes");
        if (!GetVec(dob, "pos", p, &d.pos, err, true) || !GetVec(dob, "rot", p, &d.rot, err, true, 1e3) ||
            !GetVec(dob, "scale", p, &d.scale, err, true) || !GetVec(dob, "voxelPos", p, &d.voxelPos, err, true))
            return false;
        std::string role;
        if (!GetString(dob, "role", p, &role, err)) return false;
        if (!ParseRole(role, &d.role)) return Fail(err, p + ".role: unknown role '" + role + "'");
        s.details.push_back(std::move(d));
    }

    const Json* blobs = root.find("blobs");
    if (!blobs || blobs->kind != Json::Kind::Array || blobs->arr.size() > 2 * kMaxFrames)
        return Fail(err, "blobs: must be an array of at most 8192 blobs");
    for (size_t i = 0; i < blobs->arr.size(); ++i) {
        const std::string p = "blobs[" + std::to_string(i) + "]";
        const Json& bo = blobs->arr[i];
        if (!Obj(&bo, p, err) || !OnlyKeys(bo, {"ref", "kind", "frame", "bytes"}, p, err)) return false;
        Blob b;
        int64_t bytes = 0;
        if (!GetInt(bo, "ref", p, 0, 1 << 24, &b.ref, err) || !GetString(bo, "kind", p, &b.kind, err)) return false;
        const bool hmpBlob = v2 && b.kind == "hmp";
        const int64_t lo = hmpBlob ? 0 : v2 ? -static_cast<int64_t>(kMaxNewFrames) : 1, hi = hmpBlob ? 0 : 1 << 24;
        if (!GetInt(bo, "frame", p, lo, hi, &b.frame, err) || !GetInt(bo, "bytes", p, 0, 4 * int64_t(kMaxFrameVoxels), &bytes, err))
            return false;
        b.bytes = static_cast<uint64_t>(bytes);
        s.blobs.push_back(std::move(b));
    }
    if (!ValidateScene(s, err)) return false;
    *out = std::move(s);
    return true;
}

bool ValidateScene(const Scene& s, std::string* err) {
    if (!PrintableAscii(s.title, 1, 40)) return Fail(err, "scene.title: must be 1-40 printable ASCII characters");
    if (s.stem.empty() || s.stem.size() > names::kMaxStem || s.stem.find('.') != std::string::npos ||
        !PrintableAscii(s.stem, 1, names::kMaxStem))
        return Fail(err, "scene.stem: must be 1-48 printable characters without '.'");
    if (!DatabankShape(s.databank, true, err)) return false;
    if (s.water && (!std::isfinite(*s.water) || *s.water < -1000 || *s.water > 1000))
        return Fail(err, "water.level: must be -1000..1000 world units");
    std::unordered_map<int64_t, const Frame*> frames;
    for (auto& f : s.frames)
        if (!frames.emplace(f.id, &f).second) return Fail(err, "frames: duplicate id " + std::to_string(f.id));
    std::unordered_map<int64_t, const Blob*> blobs;
    for (auto& b : s.blobs) {
        if (!blobs.emplace(b.ref, &b).second) return Fail(err, "blobs: duplicate ref " + std::to_string(b.ref));
        if (b.kind == "hmp") {
            if (b.frame != 0 || b.bytes != kHmpBytes || s.hmpRef != b.ref)
                return Fail(err, "blobs: the hmp blob must be the painted surround (frame 0, 50000 bytes, hmp.ref)");
            continue;
        }
        if (b.kind != "voxels" && b.kind != "heightMap") return Fail(err, "blobs: kind must be voxels, heightMap or hmp");
        if (!frames.count(b.frame)) return Fail(err, "blobs: ref " + std::to_string(b.ref) + " names a missing frame");
    }
    if (s.hmpRef >= 0 && (s.hmp != HmpMode::Paint || !blobs.count(s.hmpRef)))
        return Fail(err, "hmp.ref: names the painted surround's blob and needs hmp.mode paint");
    if (s.hmp == HmpMode::Paint && s.hmpRef < 0) return Fail(err, "hmp.ref: hmp.mode paint needs the painted surround's blob");
    size_t newFrames = 0;
    for (auto& f : s.frames) {
        if (!f.isNew) continue;
        const std::string p = "frame " + std::to_string(f.id);
        if (++newFrames > kMaxNewFrames) return Fail(err, "frames: at most 64 new frames");
        if (f.id >= 0 || f.parent < 1) return Fail(err, p + ": a new frame has id -1..-64 and a frame of the base as parent");
        if (f.rot != Vec3{0, 0, 0} || f.scale != Vec3{1, 1, 1} || f.folder || f.heightMap >= 0)
            return Fail(err, p + ": a new frame has identity rotation and scale, no heightMap, and is not a folder");
        for (int k = 0; k < 3; ++k)
            if (f.size[k] < 1 || f.size[k] > static_cast<int>(kMaxNewFrameSide)) return Fail(err, p + ".size: each size must be 1..32");
    }
    int roots = 0;
    for (auto& f : s.frames) {
        const std::string p = "frame " + std::to_string(f.id);
        if (f.parent < 0) ++roots;
        else if (!frames.count(f.parent) || f.parent == f.id) return Fail(err, p + ": parent is not a frame");
        const uint64_t cells = static_cast<uint64_t>(f.size[0]) * f.size[1] * f.size[2];
        if (f.voxels >= 0) {
            auto it = blobs.find(f.voxels);
            if (it == blobs.end() || it->second->kind != "voxels" || it->second->frame != f.id || it->second->bytes != cells * 4)
                return Fail(err, p + ": the voxels blob does not match the frame size");
        }
        if (f.heightMap >= 0) {
            const uint64_t corners = cells ? static_cast<uint64_t>(f.size[0] + 1) * (f.size[2] + 1) : 0;
            auto it = blobs.find(f.heightMap);
            if (it == blobs.end() || it->second->kind != "heightMap" || it->second->frame != f.id || it->second->bytes != corners * 4)
                return Fail(err, p + ": the heightMap blob does not match the frame size");
        }
    }
    if (!s.frames.empty() && roots != 1) return Fail(err, "frames: exactly one root frame (parent null) is required");
    for (auto& f : s.frames) {
        int64_t cur = f.id;
        for (size_t hops = 0; cur >= 0; ++hops) {
            if (hops > s.frames.size()) return Fail(err, "frame " + std::to_string(f.id) + ": the parent chain has a cycle");
            cur = frames[cur]->parent;
        }
    }
    for (auto& f : s.frames)
        if (f.isNew && (!frames.count(f.parent) || frames[f.parent]->isNew || !UnderSceneFrame(s, f.parent)))
            return Fail(err, "frame " + std::to_string(f.id) + ": a new frame's parent must be the Scene frame or under it");
    std::unordered_set<int64_t> ids, srcs;
    for (auto& d : s.details) {
        const std::string p = "detail " + std::to_string(d.id);
        if (!ids.insert(d.id).second) return Fail(err, "details: duplicate id " + std::to_string(d.id));
        if (d.src && !srcs.insert(*d.src).second) return Fail(err, p + ": duplicate src");
        if (!frames.count(d.frame)) return Fail(err, p + ": frame " + std::to_string(d.frame) + " is not in the scene");
        if (!d.src && d.name == "telepad") return Fail(err, p + ": an added detail may not be named 'telepad' (place a telepad pair)");
    }
    return ValidateObjects(s, err);
}

bool ValidateObjects(const Scene& s, std::string* err) {
    if (s.objects.size() > kMaxObjects) return Fail(err, "objects: at most 256 objects");
    std::unordered_map<std::string, int> added;
    std::unordered_set<std::string> based;
    for (auto& d : s.details) {
        if (d.src) based.insert(d.name);
        else ++added[d.name];
    }
    std::unordered_set<std::string> knots;
    std::unordered_set<int> groups;
    int factories = 0;
    for (size_t i = 0; i < s.objects.size(); ++i) {
        const ObjectSpec& o = s.objects[i];
        const std::string p = "objects[" + std::to_string(i) + "]";
        if (!ValidateObject(o, p, err)) return false;
        if (!knots.insert(o.knot).second) return Fail(err, p + ".knot: '" + o.knot + "' is used by another object");
        if (based.count(o.knot)) return Fail(err, p + ".knot: the base level already has a detail named '" + o.knot + "'");
        auto it = added.find(o.knot);
        if (it == added.end() || it->second != 1) return Fail(err, p + ".knot: needs exactly one added detail named '" + o.knot + "'");
        if (o.type == ObjectType::Telepad) groups.insert(o.group);
        if (o.type == ObjectType::MineFactory && ++factories > 1) return Fail(err, p + ": at most one mine factory");
    }
    if (groups.size() > kMaxTelepadGroups) return Fail(err, "objects: at most 8 telepad groups");
    return true;
}

std::string WriteScene(const Scene& s) {
    const bool v2 = s.UsesV2();
    Json root = Json::Obj();
    root.set("format", Str(v2 ? kSceneFormat2 : kSceneFormat));
    root.set("stem", Str(s.stem));
    root.set("title", Str(s.title));
    root.set("base", BaseJson(s.base, true));
    Json reg = Json::Obj();
    reg.set("levelType", Int(s.registry.levelType));
    reg.set("themeType", Int(s.registry.themeType));
    Json scripts = Json::Arr();
    for (auto& n : s.registry.scripts) scripts.arr.push_back(Str(n));
    reg.set("scripts", std::move(scripts));
    reg.set("previewType", Int(s.registry.previewType));
    root.set("registry", std::move(reg));
    if (v2) root.set("kind", KindJson(s.survivor));
    Json db = Json::Obj();
    db.set("theme", Str(s.databank.theme));
    db.set("timeOfDay", Str(s.databank.timeOfDay));
    db.set("materialFile", Str(s.databank.materialFile));
    db.set("heightmapBase", Str(s.databank.heightmapBase));
    db.set("heightmapSecond", Str(s.databank.heightmapSecond));
    root.set("databank", std::move(db));
    Json water = Json::Obj();
    water.set("level", s.water ? Num(*s.water) : Json::Null_());
    root.set("water", std::move(water));
    Json sp = Json::Obj();
    sp.set("mode", Str(SpawnModeName(s.spawns)));
    root.set("spawns", std::move(sp));
    Json hmp = Json::Obj();
    hmp.set("mode", Str(HmpModeName(s.hmp)));
    if (s.hmpRef >= 0) hmp.set("ref", Int(s.hmpRef));
    root.set("hmp", std::move(hmp));
    Json units = Json::Obj();
    units.set("worldPerXan", Int(s.worldPerXan));
    root.set("units", std::move(units));
    Json frames = Json::Arr();
    for (auto& f : s.frames) {
        Json o = Json::Obj();
        o.set("id", Int(f.id));
        if (f.isNew) o.set("new", Json::Bool(true));
        o.set("parent", f.parent < 0 ? Json::Null_() : Int(f.parent));
        o.set("name", Str(f.name));
        o.set("pos", Vec(f.pos));
        o.set("rot", Vec(f.rot));
        o.set("scale", Vec(f.scale));
        Json size = Json::Arr();
        for (int v : f.size) size.arr.push_back(Int(v));
        o.set("size", std::move(size));
        o.set("voxels", f.voxels < 0 ? Json::Null_() : Int(f.voxels));
        o.set("heightMap", f.heightMap < 0 ? Json::Null_() : Int(f.heightMap));
        o.set("folder", Json::Bool(f.folder));
        frames.arr.push_back(std::move(o));
    }
    root.set("frames", std::move(frames));
    Json details = Json::Arr();
    for (auto& d : s.details) {
        Json o = Json::Obj();
        o.set("id", Int(d.id));
        o.set("src", d.src ? Int(*d.src) : Json::Null_());
        o.set("frame", Int(d.frame));
        o.set("name", Str(d.name));
        o.set("resource", Str(d.resource));
        o.set("pos", Vec(d.pos));
        o.set("rot", Vec(d.rot));
        o.set("scale", Vec(d.scale));
        o.set("voxelPos", Vec(d.voxelPos));
        o.set("role", Str(RoleName(d.role)));
        details.arr.push_back(std::move(o));
    }
    root.set("details", std::move(details));
    Json blobs = Json::Arr();
    for (auto& b : s.blobs) {
        Json o = Json::Obj();
        o.set("ref", Int(b.ref));
        o.set("kind", Str(b.kind));
        o.set("frame", Int(b.frame));
        o.set("bytes", Json::UInt(b.bytes));
        blobs.arr.push_back(std::move(o));
    }
    root.set("blobs", std::move(blobs));
    if (v2) {
        root.set("objects", ObjectsJson(s.objects));
        root.set("script", ScriptJson(s.script));
    }
    return Compact(root);
}

// T(pos) * Rz(rot.z) * Ry(rot.y) * Rx(rot.x) * S(scale), the composition the engine's detail placement matches.
Mat3x4 FrameLocal(const Frame& f) {
    const double cx = std::cos(f.rot[0]), sx = std::sin(f.rot[0]), cy = std::cos(f.rot[1]), sy = std::sin(f.rot[1]),
                 cz = std::cos(f.rot[2]), sz = std::sin(f.rot[2]);
    const double r[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                            {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                            {-sy, cy * sx, cy * cx}};
    Mat3x4 out{};
    for (int j = 0; j < 3; ++j) {
        for (int k = 0; k < 3; ++k) out.m[j][k] = r[j][k] * f.scale[k];
        out.m[j][3] = f.pos[j];
    }
    return out;
}

Mat3x4 Multiply(const Mat3x4& a, const Mat3x4& b) {
    Mat3x4 r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = v;
        }
    }
    return r;
}

Vec3 Apply(const Mat3x4& m, const Vec3& p) {
    Vec3 r{};
    for (int i = 0; i < 3; ++i) r[i] = m.m[i][0] * p[0] + m.m[i][1] * p[1] + m.m[i][2] * p[2] + m.m[i][3];
    return r;
}

bool FrameWorld(const Scene& s, int64_t frameId, Mat3x4* out) {
    Mat3x4 acc{};
    for (int i = 0; i < 3; ++i) acc.m[i][i] = 1;
    int64_t cur = frameId;
    for (size_t hops = 0; cur >= 0; ++hops) {
        const Frame* f = s.FindFrame(cur);
        if (!f || hops > s.frames.size()) return false;
        if (f->parent < 0) break;  // the root store is empty (scale 0) and places nothing
        acc = Multiply(FrameLocal(*f), acc);
        cur = f->parent;
    }
    *out = acc;
    return true;
}

bool DetailWorld(const Scene& s, const Detail& d, Vec3* out) {
    Mat3x4 m;
    if (!FrameWorld(s, d.frame, &m)) return false;
    *out = Apply(m, d.pos);
    return true;
}
}  // namespace melange::erg
