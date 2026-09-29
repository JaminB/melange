#include "erg/patch.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "erg/jsonio.h"
#include "erg/names.h"

namespace melange::erg {
namespace {
using namespace jsonio;

bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

std::string OpPath(size_t i) { return "ops[" + std::to_string(i) + "]"; }

bool ValidStemShape(std::string_view s) {
    if (s.empty() || s.size() > names::kMaxStem || s.find('_') == std::string_view::npos) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

bool ParseFields(const Json& o, const std::string& p, bool add, DetailFields* f, std::string* err) {
    if (!OnlyKeys(o, {"name", "resource", "pos", "rot", "scale", "voxelPos"}, p, err)) return false;
    for (auto [key, dst] : {std::pair{"name", &f->name}, std::pair{"resource", &f->resource}}) {
        const Json* v = o.find(key);
        if (!v) {
            if (add) return Fail(err, p + "." + key + ": is required");
            continue;
        }
        if (v->kind != Json::Kind::String || !PrintableAscii(v->str, 1, 63))
            return Fail(err, p + "." + key + ": must be 1-63 printable ASCII characters");
        *dst = v->str;
    }
    for (auto [key, dst, limit] : {std::tuple{"pos", &f->pos, 1e6}, std::tuple{"rot", &f->rot, 1e3},
                                   std::tuple{"scale", &f->scale, 1e6}, std::tuple{"voxelPos", &f->voxelPos, 1e6}}) {
        if (!o.find(key)) {
            if (add && std::string_view(key) == "pos") return Fail(err, p + ".pos: is required");
            continue;
        }
        Vec3 v{};
        if (!GetVec(o, key, p, &v, err, true, limit)) return false;
        *dst = v;
    }
    return true;
}

Json FieldsJson(const DetailFields& f) {
    Json o = Json::Obj();
    if (f.name) o.set("name", Str(*f.name));
    if (f.resource) o.set("resource", Str(*f.resource));
    if (f.pos) o.set("pos", Vec(*f.pos));
    if (f.rot) o.set("rot", Vec(*f.rot));
    if (f.scale) o.set("scale", Vec(*f.scale));
    if (f.voxelPos) o.set("voxelPos", Vec(*f.voxelPos));
    return o;
}

void Assign(Detail& d, const DetailFields& f) {
    if (f.name) d.name = *f.name;
    if (f.resource) d.resource = *f.resource;
    if (f.pos) d.pos = *f.pos;
    if (f.rot) d.rot = *f.rot;
    if (f.scale) d.scale = *f.scale;
    if (f.voxelPos) d.voxelPos = *f.voxelPos;
    d.role = DeriveRole(d.name, d.resource);
}

bool IsFolder(const Frame& f) {
    return f.size == std::array<int, 3>{1, 1, 1} && f.rot == Vec3{0, 0, 0} && f.scale == Vec3{1, 1, 1};
}
}  // namespace

const char* OpName(Op::Kind k) {
    switch (k) {
        case Op::Kind::Set: return "set";
        case Op::Kind::Add: return "add";
        case Op::Kind::Remove: return "remove";
        case Op::Kind::Voxels: return "voxels";
    }
    return "?";
}

bool ValidRunValue(uint32_t v) { return (v & 0xff000000u) == 0 && ((v & 3u) == 0 || (v & 3u) == 3); }

bool ParsePatch(std::string_view json, Patch* out, std::string* err) {
    if (json.size() > kMaxPatchBytes) return Fail(err, "the patch is larger than 4 MB");
    Json root;
    std::string perr;
    if (!xom::ParseJson(json, root, &perr)) return Fail(err, "not JSON: " + perr);
    if (!Obj(&root, "patch", err) ||
        !OnlyKeys(root, {"format", "stem", "title", "base", "databank", "water", "spawns", "hmp", "ops"}, "patch", err))
        return false;
    Patch p;
    std::string format, mode;
    if (!GetString(root, "format", "patch", &format, err)) return false;
    if (format != kPatchFormat) return Fail(err, "patch.format: must be \"erg-patch/1\"");
    if (!GetString(root, "stem", "patch", &p.stem, err) || !GetString(root, "title", "patch", &p.title, err)) return false;
    if (!ValidStemShape(p.stem)) return Fail(err, "patch.stem: must be <prefix>_<slug> (a-z, 0-9, '_', at most 48)");
    if (names::CollidesWithVanilla(p.stem)) return Fail(err, "patch.stem: '" + p.stem + "' is a vanilla level stem");
    if (!PrintableAscii(p.title, 1, 40)) return Fail(err, "patch.title: must be 1-40 printable ASCII characters");

    const Json* base = root.find("base");
    if (!Obj(base, "base", err) || !OnlyKeys(*base, {"key", "source", "sha256"}, "base", err)) return false;
    {
        const Json& wrapped = *base;
        if (!GetString(wrapped, "key", "base", &p.base.key, err) || !GetString(wrapped, "source", "base", &p.base.source, err))
            return false;
        if (!PrintableAscii(p.base.key, 1, 79)) return Fail(err, "base.key: must be 1-79 printable characters");
        if (p.base.source != "game" && p.base.source != "pack") return Fail(err, "base.source: must be \"game\" or \"pack\"");
        const Json* sha = wrapped.find("sha256");
        if (!Obj(sha, "base.sha256", err) || !OnlyKeys(*sha, {"xan", "xom", "hmp"}, "base.sha256", err) ||
            !GetString(*sha, "xan", "base.sha256", &p.base.sha256.xan, err) || !GetString(*sha, "xom", "base.sha256", &p.base.sha256.xom, err))
            return false;
        if (const Json* h = sha->find("hmp"); h && h->kind != Json::Kind::Null &&
                                              !GetString(*sha, "hmp", "base.sha256", &p.base.sha256.hmp, err))
            return false;
        if (!IsHex64(p.base.sha256.xan) || !IsHex64(p.base.sha256.xom) || (!p.base.sha256.hmp.empty() && !IsHex64(p.base.sha256.hmp)))
            return Fail(err, "base.sha256: each hash must be 64 lower-case hex digits");
    }

    if (const Json* db = root.find("databank")) {
        if (!Obj(db, "databank", err) ||
            !OnlyKeys(*db, {"theme", "timeOfDay", "materialFile", "heightmapBase", "heightmapSecond"}, "databank", err))
            return false;
        DatabankInfo shape;
        for (auto [key, dst, into] :
             {std::tuple{"theme", &p.databank.theme, &shape.theme}, std::tuple{"timeOfDay", &p.databank.timeOfDay, &shape.timeOfDay},
              std::tuple{"materialFile", &p.databank.materialFile, &shape.materialFile},
              std::tuple{"heightmapBase", &p.databank.heightmapBase, &shape.heightmapBase},
              std::tuple{"heightmapSecond", &p.databank.heightmapSecond, &shape.heightmapSecond}}) {
            const Json* v = db->find(key);
            if (!v) continue;
            if (v->kind != Json::Kind::String || v->str.empty()) return Fail(err, std::string("databank.") + key + ": must be a non-empty string");
            *dst = v->str;
            *into = v->str;
        }
        if (!DatabankShape(shape, true, err)) return false;
    }
    if (const Json* water = root.find("water")) {
        if (!Obj(water, "water", err) || !OnlyKeys(*water, {"level"}, "water", err)) return false;
        if (const Json* lv = water->find("level"); lv && lv->kind != Json::Kind::Null) {
            double w = 0;
            if (!GetNumber(*lv, "water.level", -1000, 1000, &w, err)) return false;
            p.water = w;
        }
    }
    if (const Json* sp = root.find("spawns")) {
        if (!Obj(sp, "spawns", err) || !OnlyKeys(*sp, {"mode"}, "spawns", err) || !GetString(*sp, "mode", "spawns", &mode, err))
            return false;
        if (!ParseSpawnMode(mode, &p.spawns)) return Fail(err, "spawns.mode: must be \"random\" or \"knots\"");
    }
    if (const Json* hmp = root.find("hmp")) {
        if (!Obj(hmp, "hmp", err) || !OnlyKeys(*hmp, {"mode"}, "hmp", err) || !GetString(*hmp, "mode", "hmp", &mode, err))
            return false;
        if (!ParseHmpMode(mode, &p.hmp)) return Fail(err, "hmp.mode: must be \"copy\", \"none\" or \"flat\"");
    }

    const Json* ops = root.find("ops");
    if (!ops || ops->kind != Json::Kind::Array) return Fail(err, "ops: must be an array");
    if (ops->arr.size() > kMaxOps) return Fail(err, "ops: at most 20000 ops");
    std::map<int64_t, size_t> runsPerFrame;
    for (size_t i = 0; i < ops->arr.size(); ++i) {
        const std::string path = OpPath(i);
        const Json& o = ops->arr[i];
        if (!Obj(&o, path, err)) return false;
        std::string kind;
        if (!GetString(o, "op", path, &kind, err)) return false;
        Op op;
        if (kind == "set") {
            op.kind = Op::Kind::Set;
            if (!OnlyKeys(o, {"op", "src", "name", "resource", "pos", "rot", "scale", "voxelPos"}, path, err) ||
                !GetInt(o, "src", path, 1, 1 << 24, &op.src, err))
                return false;
            Json fields = Json::Obj();
            for (auto& [k, v] : o.obj)
                if (k != "op" && k != "src") fields.obj.emplace_back(k, v);
            if (!ParseFields(fields, path, false, &op.fields, err)) return false;
            if (op.fields.Empty()) return Fail(err, path + ": a set changes at least one field");
        } else if (kind == "add") {
            op.kind = Op::Kind::Add;
            if (!OnlyKeys(o, {"op", "frame", "detail"}, path, err) || !GetInt(o, "frame", path, 1, 1 << 24, &op.frame, err))
                return false;
            const Json* d = o.find("detail");
            if (!Obj(d, path + ".detail", err) || !ParseFields(*d, path + ".detail", true, &op.fields, err)) return false;
        } else if (kind == "remove") {
            op.kind = Op::Kind::Remove;
            if (!OnlyKeys(o, {"op", "src"}, path, err) || !GetInt(o, "src", path, 1, 1 << 24, &op.src, err)) return false;
        } else if (kind == "voxels") {
            op.kind = Op::Kind::Voxels;
            if (!OnlyKeys(o, {"op", "frame", "runs"}, path, err) || !GetInt(o, "frame", path, 1, 1 << 24, &op.frame, err))
                return false;
            const Json* runs = o.find("runs");
            if (!runs || runs->kind != Json::Kind::Array || runs->arr.empty())
                return Fail(err, path + ".runs: must be a non-empty array");
            size_t& total = runsPerFrame[op.frame];
            total += runs->arr.size();
            if (total > kMaxRunsPerFrame) return Fail(err, path + ".runs: more than 2000 runs for frame " + std::to_string(op.frame));
            for (size_t r = 0; r < runs->arr.size(); ++r) {
                const Json& run = runs->arr[r];
                const std::string rp = path + ".runs[" + std::to_string(r) + "]";
                if (run.kind != Json::Kind::Array || run.arr.size() != 3) return Fail(err, rp + ": must be [start, count, value]");
                uint64_t v[3] = {};
                for (int k = 0; k < 3; ++k) {
                    bool ok = false;
                    v[k] = run.arr[k].kind == Json::Kind::Number ? run.arr[k].asUInt64(&ok) : 0;
                    if (!ok || v[k] > 0xffffffffu) return Fail(err, rp + ": must hold three unsigned 32-bit integers");
                }
                if (v[1] == 0 || v[0] + v[1] > kMaxFrameVoxels) return Fail(err, rp + ": the run is empty or past 262144 voxels");
                if (!ValidRunValue(static_cast<uint32_t>(v[2])))
                    return Fail(err, rp + ": the value must keep bits 24-31 at 0 and the solid bits at 0 or 3");
                op.runs.push_back({static_cast<uint32_t>(v[0]), static_cast<uint32_t>(v[1]), static_cast<uint32_t>(v[2])});
            }
        } else {
            return Fail(err, path + ".op: unknown op '" + kind + "'");
        }
        p.ops.push_back(std::move(op));
    }
    *out = std::move(p);
    return true;
}

std::string WritePatch(const Patch& p) {
    Json root = Json::Obj();
    root.set("format", Str(kPatchFormat));
    root.set("stem", Str(p.stem));
    root.set("title", Str(p.title));
    Json base = Json::Obj();
    base.set("key", Str(p.base.key));
    base.set("source", Str(p.base.source));
    Json sha = Json::Obj();
    sha.set("xan", Str(p.base.sha256.xan));
    sha.set("xom", Str(p.base.sha256.xom));
    sha.set("hmp", p.base.sha256.hmp.empty() ? Json::Null_() : Str(p.base.sha256.hmp));
    base.set("sha256", std::move(sha));
    root.set("base", std::move(base));
    Json db = Json::Obj();
    if (p.databank.theme) db.set("theme", Str(*p.databank.theme));
    if (p.databank.timeOfDay) db.set("timeOfDay", Str(*p.databank.timeOfDay));
    if (p.databank.materialFile) db.set("materialFile", Str(*p.databank.materialFile));
    if (p.databank.heightmapBase) db.set("heightmapBase", Str(*p.databank.heightmapBase));
    if (p.databank.heightmapSecond) db.set("heightmapSecond", Str(*p.databank.heightmapSecond));
    root.set("databank", std::move(db));
    Json water = Json::Obj();
    water.set("level", p.water ? Num(*p.water) : Json::Null_());
    root.set("water", std::move(water));
    Json sp = Json::Obj();
    sp.set("mode", Str(SpawnModeName(p.spawns)));
    root.set("spawns", std::move(sp));
    Json hmp = Json::Obj();
    hmp.set("mode", Str(HmpModeName(p.hmp)));
    root.set("hmp", std::move(hmp));
    Json ops = Json::Arr();
    for (auto& op : p.ops) {
        Json o = Json::Obj();
        o.set("op", Str(OpName(op.kind)));
        switch (op.kind) {
            case Op::Kind::Set: {
                o.set("src", Int(op.src));
                Json f = FieldsJson(op.fields);
                for (auto& kv : f.obj) o.obj.push_back(kv);
                break;
            }
            case Op::Kind::Add:
                o.set("frame", Int(op.frame));
                o.set("detail", FieldsJson(op.fields));
                break;
            case Op::Kind::Remove: o.set("src", Int(op.src)); break;
            case Op::Kind::Voxels: {
                o.set("frame", Int(op.frame));
                Json runs = Json::Arr();
                for (auto& r : op.runs) {
                    Json run = Json::Arr();
                    run.arr = {Json::UInt(r.start), Json::UInt(r.count), Json::UInt(r.value)};
                    runs.arr.push_back(std::move(run));
                }
                o.set("runs", std::move(runs));
                break;
            }
        }
        ops.arr.push_back(std::move(o));
    }
    root.set("ops", std::move(ops));
    return Compact(root);
}

bool ValidatePatch(const Patch& p, const Scene& base, const PatchRules& rules, std::string* err) {
    if (p.base.key != base.base.key || p.base.source != base.base.source) return Fail(err, "base: the patch is for another level");
    if (p.base.sha256.xan != base.base.sha256.xan || p.base.sha256.xom != base.base.sha256.xom ||
        p.base.sha256.hmp != base.base.sha256.hmp)
        return Fail(err, "base.sha256: the base files changed since the patch was made");
    if (p.databank.theme && !ValidTheme(*p.databank.theme)) return Fail(err, "databank.theme: not one of the eleven themes");
    std::unordered_map<int64_t, bool> live;           // src -> still present
    for (auto& d : base.details)
        if (d.src) live[*d.src] = true;
    for (size_t i = 0; i < p.ops.size(); ++i) {
        const Op& op = p.ops[i];
        const std::string path = OpPath(i);
        switch (op.kind) {
            case Op::Kind::Set:
            case Op::Kind::Remove: {
                auto it = live.find(op.src);
                if (it == live.end()) return Fail(err, path + ".src: " + std::to_string(op.src) + " is not a detail of the base");
                if (!it->second) return Fail(err, path + ".src: " + std::to_string(op.src) + " was removed by an earlier op");
                if (op.kind == Op::Kind::Remove) it->second = false;
                break;
            }
            case Op::Kind::Add: {
                const Frame* f = base.FindFrame(op.frame);
                if (!f) return Fail(err, path + ".frame: " + std::to_string(op.frame) + " is not a frame of the base");
                if (!rules.anyFrame && !IsFolder(*f))
                    return Fail(err, path + ".frame: details may only be added to folder frames (1x1x1, no rotation or scale)");
                break;
            }
            case Op::Kind::Voxels: {
                if (!rules.voxels) return Fail(err, path + ": voxels ops are not accepted by this version");
                const Frame* f = base.FindFrame(op.frame);
                if (!f) return Fail(err, path + ".frame: " + std::to_string(op.frame) + " is not a frame of the base");
                const uint64_t cells = static_cast<uint64_t>(f->size[0]) * f->size[1] * f->size[2];
                for (size_t r = 0; r < op.runs.size(); ++r)
                    if (static_cast<uint64_t>(op.runs[r].start) + op.runs[r].count > cells)
                        return Fail(err, path + ".runs[" + std::to_string(r) + "]: past the frame's " + std::to_string(cells) + " voxels");
                break;
            }
        }
    }
    return true;
}

bool ApplyRuns(std::vector<uint32_t>& voxels, const std::vector<VoxelRun>& runs, std::string* err) {
    for (size_t r = 0; r < runs.size(); ++r) {
        const VoxelRun& run = runs[r];
        if (!ValidRunValue(run.value)) return Fail(err, "runs[" + std::to_string(r) + "]: invalid value");
        if (static_cast<uint64_t>(run.start) + run.count > voxels.size())
            return Fail(err, "runs[" + std::to_string(r) + "]: past the frame's voxels");
        std::fill_n(voxels.begin() + run.start, run.count, run.value);
    }
    return true;
}

bool ApplyPatch(Scene& s, const Patch& p, const PatchRules& rules, std::string* err) {
    if (!ValidatePatch(p, s, rules, err)) return false;
    Scene out = s;
    out.stem = p.stem;
    out.title = p.title;
    if (p.databank.theme) out.databank.theme = *p.databank.theme;
    if (p.databank.timeOfDay) out.databank.timeOfDay = *p.databank.timeOfDay;
    if (p.databank.materialFile) out.databank.materialFile = *p.databank.materialFile;
    if (p.databank.heightmapBase) out.databank.heightmapBase = *p.databank.heightmapBase;
    if (p.databank.heightmapSecond) out.databank.heightmapSecond = *p.databank.heightmapSecond;
    out.water = p.water;
    out.spawns = p.spawns;
    out.hmp = p.hmp;
    int64_t nextId = 1;
    for (auto& d : out.details) nextId = std::max(nextId, d.id + 1);
    for (size_t i = 0; i < p.ops.size(); ++i) {
        const Op& op = p.ops[i];
        switch (op.kind) {
            case Op::Kind::Set:
                for (auto& d : out.details)
                    if (d.src && *d.src == op.src) Assign(d, op.fields);
                break;
            case Op::Kind::Remove:
                std::erase_if(out.details, [&](const Detail& d) { return d.src && *d.src == op.src; });
                break;
            case Op::Kind::Add: {
                Detail d;
                d.id = nextId++;
                d.frame = op.frame;
                Assign(d, op.fields);
                out.details.push_back(std::move(d));
                break;
            }
            case Op::Kind::Voxels: break;
        }
    }
    s = std::move(out);
    return true;
}
}  // namespace melange::erg
