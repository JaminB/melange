#include "erg/build.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>

#include "erg/luagen.h"
#include "erg/voxels.h"
#include "erg/xomutil.h"

namespace melange::erg::build {
namespace {
using xom::Type;
using xom::Value;

bool Fail(std::string* err, std::string why) {
    if (err) *err = std::move(why);
    return false;
}

bool SafeStem(const std::string& s) {
    if (s.empty() || s.size() > 48) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

// Objects are grouped in TYPE-table order, so a new object of `cls` goes right after the last object whose type comes
// no later than cls in the table.
uint32_t InsertAt(const xom::Document& doc, std::string_view cls) {
    std::unordered_map<std::string, size_t> order;
    for (size_t i = 0; i < doc.types.size(); ++i) order[doc.types[i].className()] = i;
    const size_t mine = order.count(std::string(cls)) ? order[std::string(cls)] : doc.types.size();
    uint32_t n = 0;
    for (const auto& o : doc.objects)
        if (order[o.type] <= mine) ++n;
    return n + 1;
}

const xom::Object* FirstOf(const xom::Document& doc, std::string_view cls) {
    for (const auto& o : doc.objects)
        if (o.type == cls) return &o;
    return nullptr;
}

bool Serialize(const xom::Document& doc, const char* what, std::vector<uint8_t>* out, std::string* err) {
    std::string e;
    if (!xom::serialize(doc, *out, &e)) return Fail(err, std::string(what) + ": " + e);
    xom::Document again;
    xom::ParseOptions opt;
    opt.strict = true;
    std::vector<uint8_t> round;
    if (!xom::parse(out->data(), out->size(), again, &e, opt) || !xom::serialize(again, round, &e) || round != *out)
        return Fail(err, std::string(what) + ": the output does not re-parse identically" + (e.empty() ? "" : ": " + e));
    return true;
}

std::vector<uint8_t> Words(const std::vector<uint32_t>& v) {
    std::vector<uint8_t> b(v.size() * 4);
    for (size_t i = 0; i < v.size(); ++i)
        for (int k = 0; k < 4; ++k) b[i * 4 + k] = static_cast<uint8_t>(v[i] >> (8 * k));
    return b;
}

std::vector<uint32_t> FromBytes(const std::vector<uint8_t>& b) {
    std::vector<uint32_t> v(b.size() / 4);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = b[i * 4] | b[i * 4 + 1] << 8 | b[i * 4 + 2] << 16 | static_cast<uint32_t>(b[i * 4 + 3]) << 24;
    return v;
}

bool BuildXan(const load::Loaded& base, const Scene& edited, const VoxelEdits& voxels, std::vector<uint8_t>* out,
              std::string* err) {
    xom::Document x = base.xan;
    const Scene& bs = base.scene;
    std::vector<uint32_t> cur(x.objects.size() + 1);
    for (uint32_t i = 0; i < cur.size(); ++i) cur[i] = i;
    auto onInsert = [&](uint32_t at) {
        for (auto& c : cur)
            if (c >= at) ++c;
    };
    auto onRemove = [&](uint32_t at) {
        for (auto& c : cur) c = c == at ? 0 : c > at ? c - 1 : c;
    };

    std::unordered_map<int64_t, const Detail*> bySrc;
    for (const auto& d : edited.details)
        if (d.src) {
            if (!bs.FindDetailBySrc(*d.src)) return Fail(err, "detail " + std::to_string(d.id) + ": src " + std::to_string(*d.src) + " is not in the base");
            bySrc[*d.src] = &d;
        }
    std::vector<const Detail*> removed;
    for (const auto& b : bs.details) {
        auto it = bySrc.find(*b.src);
        if (it == bySrc.end()) {
            removed.push_back(&b);
            continue;
        }
        const Detail& e = *it->second;
        if (e.frame != b.frame) return Fail(err, "detail " + std::to_string(e.id) + ": moving a detail to another frame is not supported");
        xom::Object& o = x.objects[static_cast<size_t>(*b.src) - 1];
        bool ok = true;
        if (e.name != b.name) ok &= xomutil::SetStr(o, "Name", e.name);
        if (e.resource != b.resource) ok &= xomutil::SetStr(o, "ResourceName", e.resource);
        if (e.pos != b.pos) ok &= xomutil::SetVec(o, "Position", e.pos);
        if (e.rot != b.rot) ok &= xomutil::SetVec(o, "Orientation", e.rot);
        if (e.scale != b.scale) ok &= xomutil::SetVec(o, "Scale", e.scale);
        if (e.voxelPos != b.voxelPos) ok &= xomutil::SetVec(o, "VoxelPos", e.voxelPos);
        if (!ok) return Fail(err, "detail " + std::to_string(e.id) + ": the base object has an unexpected shape");
    }

    std::sort(removed.begin(), removed.end(), [](const Detail* a, const Detail* b) { return *a->src > *b->src; });
    for (const Detail* d : removed) {
        const uint32_t at = cur[static_cast<size_t>(*d->src)], fr = cur[static_cast<size_t>(d->frame)];
        Value* list = x.objects[fr - 1].field("Details");
        if (!list) return Fail(err, "frame " + std::to_string(d->frame) + " has no Details");
        std::erase_if(list->items, [at](const Value& v) { return v.asRef() == at; });
        if (!xomutil::RemoveObject(x, at)) return Fail(err, "could not remove detail " + std::to_string(*d->src));
        onRemove(at);
    }

    for (const auto& d : edited.details) {
        if (d.src) continue;
        if (!bs.FindFrame(d.frame)) return Fail(err, "detail " + std::to_string(d.id) + ": frame " + std::to_string(d.frame) + " is not in the base");
        if (!xomutil::EnsureType(x, "DetailEntityStore", "LandFrameStore")) return Fail(err, "no DetailEntityStore type");
        xom::Object obj;
        if (const xom::Object* like = FirstOf(x, "DetailEntityStore")) {
            obj = *like;
        } else {
            std::string e;
            if (!xomutil::NewObject(x, "DetailEntityStore", &obj, &e)) return Fail(err, e);
            if (Value* b = obj.field("Bounds")) b->setComponents({0, 0, 0, -1});
            if (Value* m = obj.field("BoundMode")) m->setInt(1);
        }
        if (!xomutil::SetStr(obj, "Name", d.name) || !xomutil::SetStr(obj, "ResourceName", d.resource) ||
            !xomutil::SetVec(obj, "Position", d.pos) || !xomutil::SetVec(obj, "Orientation", d.rot) ||
            !xomutil::SetVec(obj, "Scale", d.scale) || !xomutil::SetVec(obj, "VoxelPos", d.voxelPos))
            return Fail(err, "a new detail could not be built");
        const uint32_t at = InsertAt(x, "DetailEntityStore");
        if (!xomutil::InsertObject(x, at, std::move(obj))) return Fail(err, "could not insert a detail");
        onInsert(at);
        Value* list = x.objects[cur[static_cast<size_t>(d.frame)] - 1].field("Details");
        if (!list) return Fail(err, "frame " + std::to_string(d.frame) + " has no Details");
        list->items.push_back(xomutil::RefValue(at));
    }

    for (const auto& [fid, words] : voxels) {
        const Frame* f = bs.FindFrame(fid);
        if (!f) return Fail(err, "voxels: frame " + std::to_string(fid) + " is not in the base");
        const size_t cells = static_cast<size_t>(f->size[0]) * f->size[1] * f->size[2];
        if (words.size() != cells || f->voxels < 0) return Fail(err, "voxels: frame " + std::to_string(fid) + " has another size");
        for (uint32_t w : words)
            if (!ValidRunValue(w)) return Fail(err, "voxels: frame " + std::to_string(fid) + " holds an invalid voxel");
        Value* v = x.objects[cur[static_cast<size_t>(fid)] - 1].field("Voxels");
        if (!v || v->raw.size() != cells * 4) return Fail(err, "voxels: frame " + std::to_string(fid) + " has no voxel array");
        v->raw = Words(words);
    }
    return Serialize(x, ".xan", out, err);
}

bool BuildXom(const load::Loaded& base, const DatabankInfo& want, std::vector<uint8_t>* out, std::string* err) {
    xom::Document d = base.xom;
    const DatabankInfo& have = base.scene.databank;
    const std::string* w[5] = {&want.theme, &want.timeOfDay, &want.materialFile, &want.heightmapBase, &want.heightmapSecond};
    const std::string* h[5] = {&have.theme, &have.timeOfDay, &have.materialFile, &have.heightmapBase, &have.heightmapSecond};
    for (int k = 0; k < 5; ++k) {
        if (*w[k] == *h[k]) continue;
        const char* key = load::kDatabankKeys[k];
        bool done = false;
        for (auto& o : d.objects)
            if (o.type == "XStringResourceDetails" && xomutil::Str(o, "Name") == key) {
                done = xomutil::SetStr(o, "Value", *w[k]);
                break;
            }
        if (done) continue;
        if (!xomutil::EnsureType(d, "XStringResourceDetails", "XDataBank")) return Fail(err, "level .XOM: no string resource type");
        xom::Object obj;
        if (const xom::Object* like = FirstOf(d, "XStringResourceDetails")) {
            obj = *like;
        } else {
            std::string e;
            if (!xomutil::NewObject(d, "XStringResourceDetails", &obj, &e)) return Fail(err, e);
            if (Value* f = obj.field("Flags")) f->setInt(64);
        }
        if (!xomutil::SetStr(obj, "Name", key) || !xomutil::SetStr(obj, "Value", *w[k])) return Fail(err, "level .XOM: bad string resource");
        const uint32_t at = InsertAt(d, "XStringResourceDetails");
        if (!xomutil::InsertObject(d, at, std::move(obj))) return Fail(err, "level .XOM: could not insert " + std::string(key));
        xom::Object* bank = nullptr;
        for (auto& o : d.objects)
            if (o.type == "XDataBank") {
                bank = &o;
                break;
            }
        Value* list = bank ? bank->field("StringResources") : nullptr;
        if (!list) return Fail(err, "level .XOM: no XDataBank");
        list->items.push_back(xomutil::RefValue(at));
    }
    return Serialize(d, "level .XOM", out, err);
}
}  // namespace

bool Build(const load::Loaded& base, const Scene& edited, const VoxelEdits& voxels, const Options& opt,
           std::vector<File>* out, std::string* err) {
    out->clear();
    std::string why;
    if (!ValidateScene(edited, &why)) return Fail(err, why);
    if (!SafeStem(edited.stem)) return Fail(err, "stem '" + edited.stem + "': only letters, digits, '_' and '-'");
    if (edited.base.key != base.scene.base.key || edited.base.sha256.xan != base.scene.base.sha256.xan ||
        edited.base.sha256.xom != base.scene.base.sha256.xom || edited.base.sha256.hmp != base.scene.base.sha256.hmp)
        return Fail(err, "the scene is for another base");
    DatabankInfo db = edited.databank;
    if (opt.materialTxt) db.materialFile = "Maps\\" + edited.stem + ".txt";
    std::string e;
    if (!DatabankShape(db, true, &e)) return Fail(err, e);

    File xan{"Maps/" + edited.stem + ".xan", {}}, lvl{edited.stem + ".XOM", {}};
    if (!BuildXan(base, edited, voxels, &xan.bytes, err) || !BuildXom(base, db, &lvl.bytes, err)) return false;
    out->push_back(std::move(xan));
    out->push_back(std::move(lvl));
    if (edited.hmp == HmpMode::Copy && base.hmp) {
        out->push_back({"Maps/" + edited.stem + ".hmp", *base.hmp});
    } else if (edited.hmp == HmpMode::Flat) {
        std::vector<uint8_t> h(load::kHmpBytes, 0);
        if (base.hmp) std::copy(base.hmp->begin() + load::kHmpSide * load::kHmpSide * 4, base.hmp->end(), h.begin() + load::kHmpSide * load::kHmpSide * 4);
        out->push_back({"Maps/" + edited.stem + ".hmp", std::move(h)});
    }
    if (opt.materialTxt) out->push_back({"Maps/" + edited.stem + ".txt", *opt.materialTxt});
    if (opt.chunk && luagen::Needed(edited)) {
        const std::string lua = luagen::Chunk(edited);
        out->push_back({edited.stem + ".lub", std::vector<uint8_t>(lua.begin(), lua.end())});
    }
    return true;
}

bool Apply(const load::Loaded& base, const Patch& p, const PatchRules& rules, Scene* scene, VoxelEdits* voxels,
           std::string* err) {
    Scene s = base.scene;
    if (!ApplyPatch(s, p, rules, err)) return false;
    VoxelEdits edits;
    for (size_t i = 0; i < p.ops.size(); ++i) {
        const Op& op = p.ops[i];
        if (op.kind != Op::Kind::Voxels) continue;
        const Frame* f = base.scene.FindFrame(op.frame);
        auto blob = f ? base.blobs.find(f->voxels) : base.blobs.end();
        if (blob == base.blobs.end()) return Fail(err, "ops[" + std::to_string(i) + "].frame: " + std::to_string(op.frame) + " has no voxels");
        auto [it, fresh] = edits.try_emplace(op.frame);
        if (fresh) it->second = FromBytes(blob->second);
        std::string why;
        if (!ApplyRuns(it->second, op.runs, &why)) return Fail(err, "ops[" + std::to_string(i) + "]." + why);
        const auto* bw = reinterpret_cast<const uint8_t*>(blob->second.data());
        for (size_t k = 0; k < op.runs.size(); ++k)
            for (uint32_t j = 0; j < op.runs[k].count; ++j) {
                const size_t at = (size_t(op.runs[k].start) + j) * 4;
                const uint32_t was = uint32_t(bw[at]) | uint32_t(bw[at + 1]) << 8 | uint32_t(bw[at + 2]) << 16 | uint32_t(bw[at + 3]) << 24;
                if (!voxels::ValidEdit(was, op.runs[k].value))
                    return Fail(err, "ops[" + std::to_string(i) + "].runs[" + std::to_string(k) +
                                         "]: a voxel may only be carved, filled or painted (second material and blend stay)");
            }
    }
    *scene = std::move(s);
    if (voxels) *voxels = std::move(edits);
    return true;
}

Patch Diff(const Scene& base, const Scene& edited, const VoxelEdits& voxels, const std::map<int64_t, std::vector<uint8_t>>* baseBlobs) {
    Patch p;
    p.stem = edited.stem;
    p.title = edited.title;
    p.base = base.base;
    p.base.file.clear();
    const std::string* w[5] = {&edited.databank.theme, &edited.databank.timeOfDay, &edited.databank.materialFile,
                               &edited.databank.heightmapBase, &edited.databank.heightmapSecond};
    const std::string* h[5] = {&base.databank.theme, &base.databank.timeOfDay, &base.databank.materialFile,
                               &base.databank.heightmapBase, &base.databank.heightmapSecond};
    std::optional<std::string>* d[5] = {&p.databank.theme, &p.databank.timeOfDay, &p.databank.materialFile,
                                        &p.databank.heightmapBase, &p.databank.heightmapSecond};
    for (int k = 0; k < 5; ++k)
        if (*w[k] != *h[k] && !w[k]->empty()) *d[k] = *w[k];
    p.water = edited.water;
    p.spawns = edited.spawns;
    p.hmp = edited.hmp;
    for (const auto& b : base.details) {
        if (!b.src) continue;
        const Detail* e = edited.FindDetailBySrc(*b.src);
        Op op;
        op.src = *b.src;
        if (!e) {
            op.kind = Op::Kind::Remove;
            p.ops.push_back(std::move(op));
            continue;
        }
        op.kind = Op::Kind::Set;
        if (e->name != b.name) op.fields.name = e->name;
        if (e->resource != b.resource) op.fields.resource = e->resource;
        if (e->pos != b.pos) op.fields.pos = e->pos;
        if (e->rot != b.rot) op.fields.rot = e->rot;
        if (e->scale != b.scale) op.fields.scale = e->scale;
        if (e->voxelPos != b.voxelPos) op.fields.voxelPos = e->voxelPos;
        if (!op.fields.Empty()) p.ops.push_back(std::move(op));
    }
    for (const auto& e : edited.details) {
        if (e.src) continue;
        Op op;
        op.kind = Op::Kind::Add;
        op.frame = e.frame;
        op.fields.name = e.name;
        op.fields.resource = e.resource;
        op.fields.pos = e.pos;
        if (e.rot != Vec3{0, 0, 0}) op.fields.rot = e.rot;
        if (e.scale != Vec3{1, 1, 1}) op.fields.scale = e.scale;
        if (e.voxelPos != Vec3{0, 0, 0}) op.fields.voxelPos = e.voxelPos;
        p.ops.push_back(std::move(op));
    }
    if (baseBlobs) {
        for (const auto& f : base.frames) {
            auto ed = voxels.find(f.id);
            auto bb = baseBlobs->find(f.voxels);
            if (f.voxels < 0 || ed == voxels.end() || bb == baseBlobs->end()) continue;
            const std::vector<uint32_t> before = FromBytes(bb->second);
            const std::vector<uint32_t>& after = ed->second;
            Op op;
            op.kind = Op::Kind::Voxels;
            op.frame = f.id;
            for (size_t i = 0; i < std::min(before.size(), after.size()); ++i) {
                if (before[i] == after[i]) continue;
                if (!op.runs.empty() && op.runs.back().start + op.runs.back().count == i && op.runs.back().value == after[i]) ++op.runs.back().count;
                else op.runs.push_back({static_cast<uint32_t>(i), 1, after[i]});
            }
            if (!op.runs.empty()) p.ops.push_back(std::move(op));
        }
    }
    return p;
}
}  // namespace melange::erg::build
