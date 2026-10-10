// melange::xom::mesh - see clone.h.
#include "clone.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace melange::xom::mesh {
namespace {

constexpr const char* kWorldGraphGuidHex = "6ae6dbe4fa866b45a73ff9130e12dfeb";
constexpr const char* kRootEntryGuidHex = "99cc436e6fbef54b85d2bfcdf9ae4283";

std::array<uint8_t, 16> GuidOf(const char* hex) {
    std::array<uint8_t, 16> g{};
    for (int i = 0; i < 16; ++i) {
        auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        g[size_t(i)] = uint8_t((hv(hex[2 * i]) << 4) | hv(hex[2 * i + 1]));
    }
    return g;
}

// Every Ref held by `v`, mutable (CollectRefs in mesh.cpp is the read-only form).
template <class F>
void VisitRefs(Value& v, F&& f) {
    if (v.type == Type::Ref) {
        if (!v.array) { if (v.bits) f(v); return; }
        for (auto& it : v.items) if (it.bits) f(it);
        return;
    }
    if (v.array) { if (!v.packed()) for (auto& it : v.items) VisitRefs(it, f); return; }
    if (v.type == Type::Struct) for (auto& m : v.members) VisitRefs(m.second, f);
}

std::string NameOf(const Object& o) {
    if (const Value* n = o.field("Name")) if (n->type == Type::String) return n->str;
    if (const Value* n = o.field("ResourceId")) if (n->type == Type::String) return n->str;
    return {};
}

// The field of `parent` that holds a reference to `child` ("Graphs", "Children", ...).
std::string FieldHolding(const Object& parent, uint32_t child) {
    for (auto& [k, v] : parent.fields) {
        std::vector<uint32_t> refs;
        CollectRefs(v, refs);
        if (std::find(refs.begin(), refs.end(), child) != refs.end()) return k;
    }
    return {};
}

std::vector<float> FloatsOf(const Value* v) {
    std::vector<float> out;
    if (!v || !v->packed() || v->type != Type::Math || mathDef(v->math).elem != 'f') return out;
    out.resize(v->raw.size() / 4);
    if (!out.empty()) std::memcpy(out.data(), v->raw.data(), out.size() * 4);
    return out;
}

}  // namespace

std::vector<float> ReadFloatArray(const Value& v) { return FloatsOf(&v); }
void WriteFloatArray(Value& v, const std::vector<float>& f) {
    v.raw.resize(f.size() * 4);
    if (!f.empty()) std::memcpy(v.raw.data(), f.data(), f.size() * 4);
}

uint32_t FindDescriptor(const Document& doc, const std::string& resourceId) {
    for (size_t i = 0; i < doc.objects.size(); ++i) {
        const Object& o = doc.objects[i];
        if (o.type != "XMeshDescriptor" || o.opaque || o.inTail) continue;
        const Value* rid = o.field("ResourceId");
        if (rid && rid->str == resourceId) return uint32_t(i + 1);
    }
    return 0;
}

uint32_t WorldRoot(const Document& doc, uint32_t descRef) {
    const Object* desc = doc.object(descRef);
    const Value* gsF = desc ? desc->field("GraphSet") : nullptr;
    const Object* gs = gsF ? doc.object(gsF->asRef()) : nullptr;
    const Value* graphs = gs ? gs->field("Graphs") : nullptr;
    if (!graphs || graphs->items.empty()) return 0;
    const auto want = GuidOf(kWorldGraphGuidHex);
    const Value* pick = nullptr;
    for (auto& g : graphs->items) {
        const Value* gd = g.member("Guid");
        if (gd && gd->guid == want) { pick = &g; break; }
    }
    if (!pick)
        for (auto& g : graphs->items) {
            const Value* n = g.member("Name");
            if (n && n->str == "world") { pick = &g; break; }
        }
    if (!pick) pick = &graphs->items.front();
    const Value* gr = pick->member("Graph");
    return gr ? gr->asRef() : 0;
}

std::string DescribeObject(const Document& doc, uint32_t ref) {
    const Object* o = doc.object(ref);
    std::string s = "#" + std::to_string(ref) + " " + (o ? o->type : std::string("(missing)"));
    if (o) {
        std::string n = NameOf(*o);
        if (!n.empty()) s += " \"" + n + "\"";
    }
    return s;
}

std::string ReachChain(const Document& doc, const Closure& c, uint32_t ref) {
    std::vector<uint32_t> chain;
    for (uint32_t r = ref; r;) {
        chain.push_back(r);
        auto it = c.parent.find(r);
        r = it == c.parent.end() ? 0 : it->second;
        if (chain.size() > 4096) break;
    }
    std::reverse(chain.begin(), chain.end());
    std::string s;
    // The tail of a long chain is what names the culprit; keep the first hop (the descriptor) for orientation.
    size_t from = chain.size() > 6 ? chain.size() - 5 : 0;
    if (from) s = DescribeObject(doc, chain[0]) + " -> ... ";
    for (size_t i = from; i < chain.size(); ++i) {
        if (i > from || from) s += " -> ";
        s += DescribeObject(doc, chain[i]);
        if (i + 1 < chain.size()) {
            const Object* o = doc.object(chain[i]);
            std::string f = o ? FieldHolding(*o, chain[i + 1]) : std::string();
            if (!f.empty()) s += " (" + f + ")";
        }
    }
    return s;
}

bool CollectClosure(const Document& doc, uint32_t root, Closure& out, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    out = Closure{};
    if (!doc.object(root)) return fail("closure: object #" + std::to_string(root) + " does not exist");
    std::vector<std::pair<uint32_t, uint32_t>> stack{{root, 0}};  // (object, who reached it)
    std::unordered_set<uint32_t> seen;
    while (!stack.empty()) {
        auto [r, par] = stack.back();
        stack.pop_back();
        if (!seen.insert(r).second) continue;
        const Object* o = doc.object(r);
        if (!o) {
            return fail("object #" + std::to_string(par) + " references #" + std::to_string(r) +
                        ", which does not exist (" + std::to_string(doc.objects.size()) + " objects)");
        }
        out.parent[r] = par;
        out.order.push_back(r);
        if (o->inTail || o->opaque) {
            return fail(DescribeObject(doc, r) + " is " + (o->inTail ? "in the undelimited tail of the file" : "an opaque object") +
                        " (xomtool could not decode its fields), so its references cannot be followed or rewritten and it cannot be copied; reached via " +
                        ReachChain(doc, out, r));
        }
        std::vector<uint32_t> refs;
        for (auto& [k, v] : o->fields) { (void)k; CollectRefs(v, refs); }
        for (auto it = refs.rbegin(); it != refs.rend(); ++it)
            if (!seen.count(*it)) stack.emplace_back(*it, r);
    }
    return true;
}

void InsertObject(Document& doc, uint32_t pos, Object obj) {
    auto shift = [pos](Value& ref) { if (ref.bits >= pos) ++ref.bits; };
    for (auto& o : doc.objects)
        for (auto& [k, v] : o.fields) { (void)k; VisitRefs(v, shift); }
    if (doc.root >= pos) ++doc.root;
    doc.objects.insert(doc.objects.begin() + long(pos - 1), std::move(obj));
}

uint32_t CloneMesh(Document& dst, const Document& src, const std::string& vanillaName, const std::string& newName,
                   uint16_t section, const CloneOptions& opt, CloneReport* report, std::string* error) {
    auto fail = [&](const std::string& e) -> uint32_t { if (error) *error = e; return 0; };
    if (section < kFirstModSection || section > kLastModSection)
        return fail("section " + std::to_string(section) + " is outside the mod range 476..519");
    if (!dst.objects.empty()) return fail("clone: the destination document must be empty");
    const uint32_t srcDesc = FindDescriptor(src, vanillaName);
    if (!srcDesc) return fail("no XMeshDescriptor named " + vanillaName);

    Closure c;
    if (!CollectClosure(src, srcDesc, c, error)) return 0;
    std::unordered_set<uint32_t> in(c.order.begin(), c.order.end());

    // What the closure must not drag in unannounced. Another mesh descriptor means the "mesh" is really several
    // resources; an XAnimClipLibrary that something outside the closure also reads is a shared resource that a copy
    // would silently fork. A library reached only through this descriptor's own graph set (every vanilla animated mesh
    // keeps its clips there) is the mesh's own data and is copied with it.
    std::unordered_map<uint32_t, uint32_t> outsideUser;  // closure object -> an object outside that references it
    for (size_t i = 0; i < src.objects.size(); ++i) {
        const uint32_t r = uint32_t(i + 1);
        const Object& o = src.objects[i];
        if (in.count(r) || o.inTail || o.opaque) continue;
        for (auto& [k, v] : o.fields) {
            (void)k;
            std::vector<uint32_t> refs;
            CollectRefs(v, refs);
            for (auto t : refs) if (in.count(t) && !outsideUser.count(t)) outsideUser[t] = r;
        }
    }
    std::vector<std::string> notes;
    for (auto r : c.order) {
        if (r == srcDesc) continue;
        const Object& o = src.objects[r - 1];
        if (o.type == "XMeshDescriptor" && !opt.allowShared)
            return fail("the closure of " + vanillaName + " reaches another XMeshDescriptor, " + DescribeObject(src, r) +
                        " (" + ReachChain(src, c, r) + "); pass --allow-shared to copy it too");
        if (o.type == "XAnimClipLibrary") {
            auto u = outsideUser.find(r);
            const Value* clips = o.field("Clips");
            std::string what = DescribeObject(src, r) + (clips ? ", " + std::to_string(clips->size()) + " clip(s)" : std::string());
            if (u != outsideUser.end()) {
                if (!opt.allowShared)
                    return fail("the closure of " + vanillaName + " reaches " + what + " (" + ReachChain(src, c, r) +
                                "), which " + DescribeObject(src, u->second) +
                                " outside the mesh also references; pass --allow-shared to duplicate it");
                notes.push_back("duplicated shared " + what);
            } else {
                notes.push_back("owns " + what);
            }
        }
    }

    // The engine fetches the geometry graph by GUID; a descriptor without that entry would load as an empty mesh.
    {
        const Object& d = src.objects[srcDesc - 1];
        const Value* gsF = d.field("GraphSet");
        const Object* gs = gsF ? src.object(gsF->asRef()) : nullptr;
        const Value* graphs = gs ? gs->field("Graphs") : nullptr;
        bool haveWorld = false;
        const auto want = GuidOf(kWorldGraphGuidHex);
        if (graphs)
            for (auto& g : graphs->items) {
                const Value* gd = g.member("Guid");
                if (gd && gd->guid == want) haveWorld = true;
            }
        if (!haveWorld) return fail(vanillaName + ": its XGraphSet has no entry with the geometry-graph GUID " + kWorldGraphGuidHex);
    }

    dst.version = src.version;
    dst.reserved08 = src.reserved08;
    dst.reserved24 = src.reserved24;
    dst.guidRec = src.guidRec;
    dst.schmRec = src.schmRec;
    dst.guidRecord = src.guidRecord;
    const uint32_t desc = CopySubgraph(dst, src, srcDesc, error);
    if (!desc) return 0;
    {
        Object& d = dst.objects[desc - 1];
        Value* rid = d.field("ResourceId");
        Value* sec = d.field("SectionId");
        if (!rid || !sec) return fail("internal: the copied descriptor lost ResourceId/SectionId");
        rid->str = newName;
        sec->bits = section;
    }

    // The root entry goes at the end of the XGraphSet run (every XGraphSet shares one TYPE slot), so objects stay
    // grouped; everything after it moves one place on.
    uint32_t lastGraphSet = 0;
    for (size_t i = 0; i < dst.objects.size(); ++i)
        if (dst.objects[i].type == "XGraphSet") lastGraphSet = uint32_t(i + 1);
    if (!lastGraphSet) return fail("internal: the copy has no XGraphSet");
    const uint32_t rootRef = lastGraphSet + 1;
    Object root;
    root.type = "XGraphSet";
    root.container = false;
    Value graphs;
    graphs.type = Type::Struct;
    graphs.array = true;
    graphs.items.resize(1);
    graphs.items[0].type = Type::Struct;
    { Value g; g.type = Type::Guid; g.guid = GuidOf(kRootEntryGuidHex); graphs.items[0].members.emplace_back("Guid", g); }
    { Value r; r.type = Type::Ref; r.bits = 0; graphs.items[0].members.emplace_back("Graph", r); }
    { Value n; n.type = Type::String; n.str = newName; graphs.items[0].members.emplace_back("Name", n); }
    root.fields.emplace_back("Graphs", graphs);
    InsertObject(dst, rootRef, std::move(root));
    const uint32_t descNow = desc >= rootRef ? desc + 1 : desc;
    for (auto& m : dst.objects[rootRef - 1].fields[0].second.items[0].members)
        if (m.first == "Graph") m.second.bits = descNow;
    dst.root = rootRef;

    if (report) {
        report->srcDescriptor = srcDesc;
        report->descriptor = descNow;
        report->root = rootRef;
        report->objects = dst.objects.size();
        report->notes = notes;
        report->classes.clear();
        for (auto& t : dst.types) {
            size_t n = 0;
            for (auto& o : dst.objects) if (o.type == t.className()) ++n;
            if (n) report->classes.push_back({t.className(), n});
        }
    }
    return descNow;
}

// ---------------------------------------------------------------- shapes

namespace {

// The group's own transform (its Core XTransform's Matrix, 12 floats = columns 0..3 without the bottom row), identity when
// it has none.
Mat4 LocalMatrix(const Document& doc, const Object& group) {
    Mat4 local = Identity();
    const Value* core = group.field("Core");
    const Object* xf = core ? doc.object(core->asRef()) : nullptr;
    if (xf && xf->type == "XTransform") {
        const Value* m = xf->field("Matrix");
        if (m && m->type == Type::Math) {
            auto cmp = m->components();
            if (cmp.size() == 12)
                for (size_t col = 0; col < 4; ++col)
                    for (size_t row = 0; row < 3; ++row) local[col * 4 + row] = float(cmp[col * 3 + row]);
        }
    }
    return local;
}

bool HasCore(const Document& doc, const Object& group) {
    const Value* core = group.field("Core");
    const Object* xf = core ? doc.object(core->asRef()) : nullptr;
    return xf && xf->type == "XTransform";
}

ShapeRef ShapeRefOf(const Document& doc, uint32_t ref) {
    const Object& o = doc.objects[ref - 1];
    ShapeRef s;
    s.shape = ref;
    s.skinned = o.type == "XSkinShape";
    if (const Value* n = o.field("Name")) s.name = n->str;
    if (const Value* sh = o.field("Shader")) s.shader = sh->asRef();
    if (const Value* g = o.field("Geometry")) {
        const Object* geom = doc.object(g->asRef());
        if (geom && geom->type == "XIndexedTriangleSet") s.geometry = g->asRef();
    }
    return s;
}

bool IsShape(const Document& doc, uint32_t ref) {
    const Object* o = doc.object(ref);
    return o && (o->type == "XShape" || o->type == "XSkinShape");
}

void WalkShapes(const Document& doc, uint32_t ref, const Mat4& accum, std::vector<std::string>& path,
                std::vector<ShapeRef>& out, std::unordered_set<uint32_t>& visited) {
    const Object* o = ref ? doc.object(ref) : nullptr;
    if (!o || o->opaque || o->inTail || !visited.insert(ref).second) return;
    if (o->type == "XShape" || o->type == "XSkinShape") {
        ShapeRef s = ShapeRefOf(doc, ref);
        s.matrix = accum;
        s.path = path;
        out.push_back(std::move(s));
        return;
    }
    Mat4 next = accum;
    bool pushed = false;
    if (o->type == "XGroup") next = Multiply(accum, LocalMatrix(doc, *o));
    if (o->type == "XGroup" || o->type == "XInteriorNode" || o->type == "XSkin") {
        std::string n = NameOf(*o);
        if (!n.empty()) { path.push_back(n); pushed = true; }
    }
    if (const Value* ch = o->field("Children"))
        for (size_t i = 0; i < ch->size(); ++i) WalkShapes(doc, ch->at(i).asRef(), next, path, out, visited);
    if (pushed) path.pop_back();
}

}  // namespace

bool EnumerateShapes(const Document& doc, uint32_t descRef, std::vector<ShapeRef>& out, std::string* error) {
    out.clear();
    const uint32_t root = WorldRoot(doc, descRef);
    if (!root) { if (error) *error = "the descriptor has no graph set entry to walk"; return false; }
    std::vector<std::string> path;
    std::unordered_set<uint32_t> visited;
    WalkShapes(doc, root, Identity(), path, out, visited);
    return true;
}

bool ReadShapePrimitive(const Document& doc, const ShapeRef& s, Primitive& out, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    const Object* geom = doc.object(s.geometry);
    if (!geom) return fail(s.name + ": the shape has no triangle geometry");
    auto refObj = [&](const char* f) -> const Object* {
        const Value* v = geom->field(f);
        return v ? doc.object(v->asRef()) : nullptr;
    };
    const Object *idxO = refObj("IndexSet"), *coO = refObj("CoordSet"), *noO = refObj("NormalSet"), *uvO = refObj("TexCoordSet");
    if (!idxO || !coO) return fail(s.name + ": geometry is missing its index or position set");
    out = Primitive{};
    out.name = s.name;
    out.matrix = s.matrix;
    if (const Value* idx = idxO->field("Index"))
        for (size_t i = 0; i < idx->size(); ++i) out.indices.push_back(uint32_t(idx->at(i).asUInt()));
    auto p = FloatsOf(coO->field("Coord"));
    for (size_t i = 0; i + 2 < p.size(); i += 3) out.positions.push_back({p[i], p[i + 1], p[i + 2]});
    if (noO) {
        auto n = FloatsOf(noO->field("Normal"));
        for (size_t i = 0; i + 2 < n.size(); i += 3) out.normals.push_back({n[i], n[i + 1], n[i + 2]});
    }
    if (uvO) {
        auto u = FloatsOf(uvO->field("TexCoord"));
        for (size_t i = 0; i + 1 < u.size(); i += 2) out.uvs.push_back({u[i], u[i + 1]});
    }
    return true;
}

namespace {

void WalkTree(const Document& doc, uint32_t ref, int parentNode, bool atRoot, Mesh& out, std::unordered_set<uint32_t>& visited,
              std::string* error, bool& failed) {
    const Object* o = ref ? doc.object(ref) : nullptr;
    if (failed || !o || o->opaque || o->inTail || !visited.insert(ref).second) return;
    auto attach = [&](uint32_t shapeRef, int node) {
        ShapeRef s = ShapeRefOf(doc, shapeRef);
        if (!s.geometry) return;
        if (node < 0) {  // a shape straight under the root: give it a node of its own so the mesh stays a tree
            Node nd;
            nd.name = s.name.empty() ? "shape" : s.name;
            out.nodes.push_back(std::move(nd));
            node = int(out.nodes.size()) - 1;
        }
        Primitive p;
        if (!ReadShapePrimitive(doc, s, p, error)) { failed = true; return; }
        p.matrix = Identity();
        p.node = node;
        out.primitives.push_back(std::move(p));
    };
    if (o->type == "XShape" || o->type == "XSkinShape") { attach(ref, parentNode); return; }
    const Value* ch = o->field("Children");
    int me = parentNode;
    if (!(o->type == "XInteriorNode" && atRoot)) {
        // A Core-less group named "<parent>Shape" that only holds shapes is the vanilla shape holder: its shapes belong to the
        // parent node (WriteMesh adds the holder again). Any other Core-less group, such as SentryGun's "$animTex0" that a
        // texture-animation clip addresses by name, is a node of its own.
        bool holder = o->type == "XGroup" && !HasCore(doc, *o) && parentNode >= 0 && ch && ch->size() > 0 &&
                      NameOf(*o) == out.nodes[size_t(parentNode)].name + "Shape";
        if (holder)
            for (size_t i = 0; i < ch->size(); ++i) if (!IsShape(doc, ch->at(i).asRef())) holder = false;
        if (!holder) {
            Node nd;
            nd.name = NameOf(*o);
            if (nd.name.empty()) nd.name = o->type + "_" + std::to_string(ref);
            if (o->type == "XGroup") nd.local = LocalMatrix(doc, *o);
            nd.parent = parentNode;
            out.nodes.push_back(std::move(nd));
            me = int(out.nodes.size()) - 1;
        }
    }
    if (ch)
        for (size_t i = 0; i < ch->size(); ++i) WalkTree(doc, ch->at(i).asRef(), me, false, out, visited, error, failed);
}

}  // namespace

bool ReadMeshTree(const Document& doc, uint32_t descRef, Mesh& out, std::string* error) {
    out = Mesh{};
    const Object* d = doc.object(descRef);
    if (const Value* rid = d ? d->field("ResourceId") : nullptr) out.resourceId = rid->str;
    if (const Value* sec = d ? d->field("SectionId") : nullptr) out.sectionId = uint16_t(sec->asUInt());
    const uint32_t root = WorldRoot(doc, descRef);
    if (!root) { if (error) *error = "the descriptor has no graph set entry to walk"; return false; }
    std::unordered_set<uint32_t> visited;
    bool failed = false;
    WalkTree(doc, root, -1, true, out, visited, error, failed);
    if (failed) return false;
    if (out.primitives.empty()) { if (error) *error = "no triangle shapes under the world graph"; return false; }
    return true;
}

// ---------------------------------------------------------------- images

std::vector<ImageInfo> ListImages(const Document& doc, uint32_t descRef) {
    std::vector<ImageInfo> out;
    Closure c;
    if (!CollectClosure(doc, descRef, c)) return out;
    std::unordered_map<uint32_t, size_t> at;
    for (auto r : c.order) {
        const Object& o = doc.objects[r - 1];
        if (o.type != "XImage") continue;
        ImageInfo ii;
        ii.index = int(out.size());
        ii.ref = r;
        if (const Value* n = o.field("Name")) ii.name = n->str;
        if (const Value* w = o.field("Width")) ii.width = uint32_t(w->asUInt());
        if (const Value* h = o.field("Height")) ii.height = uint32_t(h->asUInt());
        if (const Value* f = o.field("Format")) ii.format = uint32_t(f->asUInt());
        if (const Value* m = o.field("MipLevels")) ii.mips = uint32_t(m->asUInt());
        at[r] = out.size();
        out.push_back(std::move(ii));
    }
    std::vector<ShapeRef> shapes;
    if (EnumerateShapes(doc, descRef, shapes))
        for (auto& s : shapes) {
            Closure sc;
            if (!s.shader || !CollectClosure(doc, s.shader, sc)) continue;
            for (auto r : sc.order) {
                auto it = at.find(r);
                if (it == at.end()) continue;
                auto& u = out[it->second].usedBy;
                if (std::find(u.begin(), u.end(), s.name) == u.end()) u.push_back(s.name);
            }
        }
    return out;
}

// ---------------------------------------------------------------- tree

namespace {

std::string Fmt3(double x, double y, double z) {
    char b[96];
    std::snprintf(b, sizeof(b), "(%.4g, %.4g, %.4g)", x, y, z);
    return b;
}

void TreeNode(const Document& doc, uint32_t ref, int depth, std::unordered_set<uint32_t>& seen,
              const std::unordered_map<uint32_t, int>& imageIndex, std::ostringstream& out, const char* label) {
    const Object* o = ref ? doc.object(ref) : nullptr;
    std::string pad(size_t(depth) * 2, ' ');
    if (!o) { out << pad << "(missing #" << ref << ")\n"; return; }
    out << pad << (label ? std::string(label) + " " : std::string()) << DescribeObject(doc, ref);
    if (!seen.insert(ref).second) { out << "  (shown above)\n"; return; }
    if (o->type == "XGroup") {
        const Value* core = o->field("Core");
        const Object* xf = core ? doc.object(core->asRef()) : nullptr;
        const Value* m = xf ? xf->field("Matrix") : nullptr;
        if (m && m->type == Type::Math) {
            auto c = m->components();
            if (c.size() == 12) {
                bool basisIdentity = true;
                const double id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                for (int i = 0; i < 9; ++i) if (std::fabs(c[size_t(i)] - id[i]) > 1e-5) basisIdentity = false;
                out << "  at " << Fmt3(c[9], c[10], c[11]) << (basisIdentity ? "" : " rotated/scaled");
            }
        } else {
            out << "  (no transform)";
        }
    }
    if (o->type == "XShape" || o->type == "XSkinShape") {
        const Value* g = o->field("Geometry");
        const Object* geom = g ? doc.object(g->asRef()) : nullptr;
        if (geom && geom->type == "XIndexedTriangleSet") {
            const Value* pc = geom->field("PrimitiveCount");
            const Value* cs = geom->field("CoordSet");
            const Object* co = cs ? doc.object(cs->asRef()) : nullptr;
            const Value* cv = co ? co->field("Coord") : nullptr;
            out << "  " << (cv ? cv->size() : 0) << " verts, " << (pc ? pc->asUInt() : 0) << " tris";
        }
        const Value* sh = o->field("Shader");
        if (sh && sh->asRef()) {
            Closure sc;
            std::string imgs;
            if (CollectClosure(doc, sh->asRef(), sc))
                for (auto r : sc.order) {
                    auto it = imageIndex.find(r);
                    if (it != imageIndex.end()) imgs += (imgs.empty() ? "" : ",") + std::to_string(it->second);
                }
            out << "  shader " << DescribeObject(doc, sh->asRef()) << (imgs.empty() ? "" : " images[" + imgs + "]");
        }
        if (o->type == "XSkinShape")
            if (const Value* bones = o->field("Bones")) out << "  " << bones->size() << " palette bone(s)";
    }
    if (o->type == "XBone") {
        const Value* pm = o->field("PoseMatrix");
        if (pm && pm->type == Type::Math) {
            auto c = pm->components();
            if (c.size() == 16) out << "  pose at " << Fmt3(c[12], c[13], c[14]);
        }
    }
    out << "\n";
    if (depth > 40) return;
    if (o->type == "XSkin")
        if (const Value* sk = o->field("Skeleton")) TreeNode(doc, sk->asRef(), depth + 1, seen, imageIndex, out, "skeleton:");
    if (const Value* ch = o->field("Children"))
        for (size_t i = 0; i < ch->size(); ++i) TreeNode(doc, ch->at(i).asRef(), depth + 1, seen, imageIndex, out, nullptr);
}

}  // namespace

std::string TreeText(const Document& doc, uint32_t descRef) {
    std::ostringstream out;
    std::unordered_map<uint32_t, int> imageIndex;
    for (auto& ii : ListImages(doc, descRef)) imageIndex[ii.ref] = ii.index;
    std::unordered_set<uint32_t> seen;
    uint32_t root = WorldRoot(doc, descRef);
    if (!root) return "(no world graph)\n";
    TreeNode(doc, root, 0, seen, imageIndex, out, nullptr);
    return out.str();
}

}  // namespace melange::xom::mesh
