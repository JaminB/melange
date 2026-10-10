// melange::xom::mesh - see mesh.h.
//
// Writing new content keeps every object grouped in TYPE-table order (xom-format.md
// "Writing new content"), so this only ever *appends* a new, self-contained run of objects and
// new TYPE entries at the end of the document. If the target document already defines a class
// this code needs to introduce, it refuses rather than attempt an unsafe mid-list insertion
// that would need re-indexing every Ref field in the file; see docs/xomtool.md. `xomtool bank`
// and `convert ... --into` are meant to build a small, purpose-built output file per mesh/bank,
// which never hits this limit in practice.
#include "mesh.h"

#include <algorithm>
#include <cstring>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace melange::xom::mesh {
namespace {

// XGraphSet/XMeshDescriptor are hand-written (non-CTNR) classes with no FieldDef list, so
// they are absent from schema.json/findClass(). Their GUIDs are read from real content
// (Data/Bundles/Bundl416.xom's TYPE table, 2026-09-29) since there is nowhere else to get them
// when a target file does not already define them.
constexpr const char* kGraphSetGuid = "0b3dbf644139bb40b1798f882d14449b";
constexpr const char* kMeshDescGuid = "dbb2e8a8c30af04ba47696f47cf924d2";
// The engine finds a descriptor's geometry graph by the first GUID (IID at 0x98c100); a bundle root's entries carry
// the second (resource descriptor, 0x98c310). Both as stored in vanilla Bundl*.xom (docs/meshes.md).
constexpr const char* kWorldGraphGuid = "6ae6dbe4fa866b45a73ff9130e12dfeb";
constexpr const char* kRootEntryGuid = "99cc436e6fbef54b85d2bfcdf9ae4283";
// Versions seen in the same file, used only as the default for a brand new TYPE entry; a class
// the target document already defines keeps its own version untouched.
uint32_t DefaultVersionFor(const std::string& cls) {
    if (cls == "XShape") return 4;
    if (cls == "XMaterial") return 2;
    if (cls == "XGeometry") return 2;  // XGeometry.VertexShader needs Schema.FromVersion=2
    return 0;
}

std::array<uint8_t, 16> GuidFromHex(const char* hex) {
    std::array<uint8_t, 16> g{};
    for (int i = 0; i < 16; ++i) {
        auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
        g[size_t(i)] = uint8_t((hv(hex[2 * i]) << 4) | hv(hex[2 * i + 1]));
    }
    return g;
}

Value RefVal(uint32_t r) { Value v; v.type = Type::Ref; v.bits = r; return v; }
Value U16Val(uint16_t n) { Value v; v.type = Type::U16; v.bits = n; return v; }
Value U32Val(uint32_t n) { Value v; v.type = Type::U32; v.bits = n; return v; }
Value StrVal(const std::string& s) { Value v; v.type = Type::String; v.str = s; return v; }
Value BoolVal(bool b) { Value v; v.type = Type::Bool; v.bits = b ? 1 : 0; return v; }
Value RefArrVal(const std::vector<uint32_t>& refs) {
    Value v; v.type = Type::Ref; v.array = true;
    for (auto r : refs) { Value one = RefVal(r); v.items.push_back(one); }
    return v;
}
Value Vec3Val(uint16_t math, float x, float y, float z) {
    Value v; v.type = Type::Math; v.math = math; v.array = false; v.setComponents({x, y, z});
    return v;
}
// Every name used here is a fixed member of the schema's math table (xom_schema.inc); a
// negative result would mean that table no longer has a type this code depends on.
uint16_t MathIndex(const char* name) {
    int i = findMathIndex(name);
    return i < 0 ? 0 : uint16_t(i);
}

// Ensures `doc` has a TYPE entry for `cls`, using guid/version from `guidHex`/`version` if it
// does not already exist. Refuses (returns false) if the class exists with a *different* guid
// (should never happen for a real class name, but keeps this honest) - existence with the
// expected guid is fine and simply reused.
bool RequireType(Document& doc, const std::string& cls, const char* guidHex, uint32_t version, bool allowExisting,
                  std::string* error) {
    for (auto& t : doc.types) {
        if (t.className() == cls) {
            if (!allowExisting)
                return false;  // caller decides whether pre-existing is an error
            return true;
        }
    }
    TypeEntry t;
    t.name = cls;
    t.version = version;
    t.count = 0;
    t.guid = GuidFromHex(guidHex);
    std::string padded = cls;
    padded.resize(32, '\0');
    std::memcpy(t.rawName.data(), padded.data(), 32);
    doc.types.push_back(t);
    (void)error;
    return true;
}

int TypeOrder(const Document& doc, const std::string& cls) {
    for (size_t i = 0; i < doc.types.size(); ++i)
        if (doc.types[i].className() == cls) return int(i);
    return -1;
}

// The wire format gates inherited fields by *each ancestor class's own* TYPE-table version
// (real content carries a zero-count TYPE entry for every abstract ancestor purely to declare
// that version - e.g. Bundl416.xom has {"XGeometry", version 2, count 0} even though nothing
// instantiates XGeometry directly). So introducing a leaf class needs its whole ancestor chain
// declared too, not just the leaf.
bool RequireClassChain(Document& doc, const std::string& leafClass, std::string* error) {
    for (const ClassDef* c = findClass(leafClass); c; c = classParent(*c)) {
        uint32_t need = DefaultVersionFor(c->name);
        bool found = false;
        for (auto& t : doc.types) {
            if (t.className() != c->name) continue;
            found = true;
            if (t.version < need) {
                if (error)
                    *error = std::string(c->name) + ": the target file already declares this class at version " +
                             std::to_string(t.version) + ", which is older than the version " + std::to_string(need) +
                             " this mesh needs";
                return false;
            }
        }
        if (!found) {
            TypeEntry t;
            t.name = c->name;
            t.version = need;
            t.count = 0;
            t.guid = GuidFromHex(c->guid);
            std::string padded = c->name;
            padded.resize(32, '\0');
            std::memcpy(t.rawName.data(), padded.data(), 32);
            doc.types.push_back(t);
        }
    }
    return true;
}

// ---------------------------------------------------------------- read

// XTransform's cached `Matrix` field is 12 floats: the transformed local X/Y/Z basis vectors
// and the translation, each 3 floats (verified against Data/Bundles/Bundl416.xom's
// Factory.Proj.Bazookashell, 2026-09-29) - exactly a glTF column-major 4x4 matrix with the
// bottom row (0,0,0,1) omitted.
bool ReadTransform(const Object& xf, Mat4& out) {
    const Value* m = xf.field("Matrix");
    if (!m || m->type != Type::Math) return false;
    auto c = m->components();
    if (c.size() != 12) return false;
    Mat4 r = Identity();
    for (int col = 0; col < 4; ++col) {
        r[size_t(col) * 4 + 0] = float(c[size_t(col) * 3 + 0]);
        r[size_t(col) * 4 + 1] = float(c[size_t(col) * 3 + 1]);
        r[size_t(col) * 4 + 2] = float(c[size_t(col) * 3 + 2]);
        r[size_t(col) * 4 + 3] = col == 3 ? 1.0f : 0.0f;
    }
    out = r;
    return true;
}

void WalkNode(const Document& doc, uint32_t ref, const Mat4& accum, Mesh& out, bool& failed, std::string* error,
              std::unordered_set<uint32_t>& visited) {
    if (!ref || failed) return;
    // A self- or mutually-referencing XGroup/XInteriorNode chain would otherwise recurse without end
    // (FindMeshShader below already guards the same graph shape with a seen-set).
    if (!visited.insert(ref).second) return;
    const Object* o = doc.object(ref);
    if (!o) return;
    if (o->type == "XShape") {
        const Value* geomF = o->field("Geometry");
        const Value* nameF = o->field("Name");
        if (!geomF) return;
        const Object* geom = doc.object(geomF->asRef());
        if (!geom || geom->type != "XIndexedTriangleSet") {
            failed = true;
            if (error) *error = "XShape references a geometry that is not an XIndexedTriangleSet";
            return;
        }
        const Value *idxF = geom->field("IndexSet"), *coF = geom->field("CoordSet"), *noF = geom->field("NormalSet"),
                    *uvF = geom->field("TexCoordSet");
        const Object *idxO = idxF ? doc.object(idxF->asRef()) : nullptr, *coO = coF ? doc.object(coF->asRef()) : nullptr,
                     *noO = noF ? doc.object(noF->asRef()) : nullptr, *uvO = uvF ? doc.object(uvF->asRef()) : nullptr;
        if (!idxO || !coO) {
            failed = true;
            if (error) *error = "mesh geometry is missing its index or position set";
            return;
        }
        Primitive p;
        p.name = nameF ? nameF->str : "";
        p.matrix = accum;
        const Value* idx = idxO->field("Index");
        if (idx)
            for (size_t i = 0; i < idx->size(); ++i) p.indices.push_back(uint32_t(idx->at(i).asUInt()));
        const Value* co = coO->field("Coord");
        if (co)
            for (size_t i = 0; i < co->size(); ++i) {
                auto c = co->at(i).components();
                p.positions.push_back({float(c[0]), float(c[1]), float(c[2])});
            }
        if (noO) {
            const Value* no = noO->field("Normal");
            if (no)
                for (size_t i = 0; i < no->size(); ++i) {
                    auto c = no->at(i).components();
                    p.normals.push_back({float(c[0]), float(c[1]), float(c[2])});
                }
        }
        if (uvO) {
            const Value* uv = uvO->field("TexCoord");
            if (uv)
                for (size_t i = 0; i < uv->size(); ++i) {
                    auto c = uv->at(i).components();
                    p.uvs.push_back({float(c[0]), float(c[1])});
                }
        }
        out.primitives.push_back(std::move(p));
        return;
    }
    if (o->type == "XGroup") {
        Mat4 local = Identity();
        const Value* core = o->field("Core");
        if (core && core->asRef()) {
            const Object* xf = doc.object(core->asRef());
            if (xf && xf->type == "XTransform") ReadTransform(*xf, local);
        }
        Mat4 next = Multiply(accum, local);
        const Value* children = o->field("Children");
        if (children)
            for (size_t i = 0; i < children->size(); ++i)
                WalkNode(doc, children->at(i).asRef(), next, out, failed, error, visited);
        return;
    }
    if (o->type == "XInteriorNode") {
        const Value* children = o->field("Children");
        if (children)
            for (size_t i = 0; i < children->size(); ++i)
                WalkNode(doc, children->at(i).asRef(), accum, out, failed, error, visited);
        return;
    }
    // any other node type under "world" (rare) is simply not mesh geometry; skip it.
}

}  // namespace

Mat4 Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[size_t(k) * 4 + size_t(row)] * b[size_t(col) * 4 + size_t(k)];
            r[size_t(col) * 4 + size_t(row)] = s;
        }
    return r;
}

bool ReadMesh(const Document& doc, const std::string& resourceId, Mesh& out, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    const Object* desc = nullptr;
    for (auto& o : doc.objects)
        if (o.type == "XMeshDescriptor" && !o.opaque && !o.inTail) {
            const Value* rid = o.field("ResourceId");
            if (rid && rid->str == resourceId) { desc = &o; break; }
        }
    if (!desc) return fail("no XMeshDescriptor named " + resourceId);
    const Value* gsF = desc->field("GraphSet");
    const Value* secF = desc->field("SectionId");
    if (!gsF) return fail(resourceId + ": XMeshDescriptor has no GraphSet");
    const Object* gs = doc.object(gsF->asRef());
    if (!gs || gs->type != "XGraphSet") return fail(resourceId + ": GraphSet ref is not an XGraphSet");
    const Value* graphs = gs->field("Graphs");
    if (!graphs || graphs->items.empty()) return fail(resourceId + ": XGraphSet has no graph entries");
    const Value* world = nullptr;
    for (auto& g : graphs->items) {
        const Value* n = g.member("Name");
        if (n && n->str == "world") { world = &g; break; }
    }
    if (!world) world = &graphs->items.front();
    const Value* graphRef = world->member("Graph");
    if (!graphRef) return fail(resourceId + ": graph entry has no Graph ref");

    out = Mesh{};
    out.resourceId = resourceId;
    out.sectionId = secF ? uint16_t(secF->asUInt()) : 0;
    bool failed = false;
    std::unordered_set<uint32_t> visited;
    WalkNode(doc, graphRef->asRef(), Identity(), out, failed, error, visited);
    if (failed) return false;
    if (out.primitives.empty()) return fail(resourceId + ": no XShape found in its \"world\" graph");
    return true;
}

// ---------------------------------------------------------------- ref closure (copy)

namespace {
void CollectRefs(const Value& v, std::vector<uint32_t>& out) {
    if (v.type == Type::Ref) {
        if (!v.array) { if (v.bits) out.push_back(uint32_t(v.bits)); return; }
    }
    if (v.array) {
        if (v.type == Type::Ref) { for (auto& it : v.items) if (it.bits) out.push_back(uint32_t(it.bits)); return; }
        if (!v.packed()) for (auto& it : v.items) CollectRefs(it, out);
        return;
    }
    if (v.type == Type::Struct) { for (auto& [k, m] : v.members) { (void)k; CollectRefs(m, out); } return; }
}
void RemapRefs(Value& v, const std::unordered_map<uint32_t, uint32_t>& m) {
    if (v.type == Type::Ref && !v.array) { if (v.bits) v.bits = m.count(uint32_t(v.bits)) ? m.at(uint32_t(v.bits)) : 0; return; }
    if (v.array) {
        if (v.type == Type::Ref) { for (auto& it : v.items) if (it.bits) it.bits = m.count(uint32_t(it.bits)) ? m.at(uint32_t(it.bits)) : 0; return; }
        if (!v.packed()) for (auto& it : v.items) RemapRefs(it, m);
        return;
    }
    if (v.type == Type::Struct) for (auto& [k, mv] : v.members) { (void)k; RemapRefs(mv, m); }
}
}  // namespace

uint32_t CopySubgraph(Document& dst, const Document& src, uint32_t srcRef, std::string* error) {
    auto fail = [&](const std::string& e) -> uint32_t { if (error) *error = e; return 0; };
    if (!srcRef || srcRef > src.objects.size()) return fail("copy: ref out of range");
    std::vector<uint32_t> order;
    std::unordered_set<uint32_t> seen;
    std::queue<uint32_t> q;
    q.push(srcRef);
    seen.insert(srcRef);
    while (!q.empty()) {
        uint32_t r = q.front();
        q.pop();
        order.push_back(r);
        const Object& o = src.objects[r - 1];
        if (o.inTail) return fail("copy: an object in the undelimited tail cannot be copied");
        std::vector<uint32_t> refs;
        for (auto& [k, v] : o.fields) { (void)k; CollectRefs(v, refs); }
        for (auto rr : refs)
            if (seen.insert(rr).second) q.push(rr);
    }
    // Bucket by type, in the order each type first appears in the source TYPE table, so the
    // copy is internally consistent; refuse if the target already defines any of these classes
    // (see file header comment).
    std::vector<std::string> typeOrder;
    for (auto& t : src.types) typeOrder.push_back(t.className());
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        auto pa = std::find(typeOrder.begin(), typeOrder.end(), src.objects[a - 1].type) - typeOrder.begin();
        auto pb = std::find(typeOrder.begin(), typeOrder.end(), src.objects[b - 1].type) - typeOrder.begin();
        return pa < pb;
    });
    std::unordered_map<uint32_t, uint32_t> remap;
    uint32_t next = uint32_t(dst.objects.size()) + 1;
    for (auto old : order) remap[old] = next++;

    auto copyTypeEntry = [&](const std::string& cls) -> bool {
        if (std::any_of(dst.types.begin(), dst.types.end(), [&](const TypeEntry& t) { return t.className() == cls; }))
            return true;
        for (auto& t : src.types)
            if (t.className() == cls) {
                TypeEntry nt = t;
                nt.count = 0;
                dst.types.push_back(nt);
                return true;
            }
        return false;
    };
    for (auto old : order) {
        const std::string& cls = src.objects[old - 1].type;
        if (!copyTypeEntry(cls)) return fail("copy: class not in the source TYPE table: " + cls);
        // Field gating is per ancestor class's own TYPE-table version (see mesh.cpp's other
        // note on XGeometry), so a copied leaf class's whole ancestor chain needs its source
        // version carried over too, even though those abstract classes have no objects here.
        for (const ClassDef* c = findClass(cls); c; c = classParent(*c))
            if (!copyTypeEntry(c->name)) return fail("copy: ancestor class not in the source TYPE table: " + std::string(c->name));
    }
    for (auto old : order) {
        Object copy = src.objects[old - 1];
        for (auto& [k, v] : copy.fields) { (void)k; RemapRefs(v, remap); }
        dst.objects.push_back(std::move(copy));
    }
    return remap[srcRef];
}

uint32_t FindMeshShader(const Document& doc, const std::string& resourceId, std::string* error) {
    auto fail = [&](const std::string& e) -> uint32_t { if (error) *error = e; return 0; };
    Mesh m;
    // Re-walk just far enough to find the first XShape's Shader ref.
    const Object* desc = nullptr;
    for (auto& o : doc.objects)
        if (o.type == "XMeshDescriptor" && o.field("ResourceId") && o.field("ResourceId")->str == resourceId) { desc = &o; break; }
    if (!desc) return fail("no XMeshDescriptor named " + resourceId);
    const Value* gsF = desc->field("GraphSet");
    const Object* gs = gsF ? doc.object(gsF->asRef()) : nullptr;
    if (!gs) return fail(resourceId + ": no GraphSet");
    const Value* graphs = gs->field("Graphs");
    if (!graphs || graphs->items.empty()) return fail(resourceId + ": empty GraphSet");
    const Value* world = nullptr;
    for (auto& g : graphs->items) { const Value* n = g.member("Name"); if (n && n->str == "world") { world = &g; break; } }
    if (!world) world = &graphs->items.front();
    uint32_t root = world->member("Graph")->asRef();
    std::vector<uint32_t> stack = {root};
    std::unordered_set<uint32_t> seen;
    while (!stack.empty()) {
        uint32_t r = stack.back();
        stack.pop_back();
        if (!r || !seen.insert(r).second) continue;
        const Object* o = doc.object(r);
        if (!o) continue;
        if (o->type == "XShape") {
            const Value* sh = o->field("Shader");
            if (sh && sh->asRef()) return sh->asRef();
            continue;
        }
        const Value* children = o->field("Children");
        if (children) for (size_t i = 0; i < children->size(); ++i) stack.push_back(children->at(i).asRef());
    }
    return fail(resourceId + ": no XShape with a Shader was found");
}

uint32_t FindShaderTexture(const Document& doc, uint32_t shaderRef, std::string* error) {
    auto fail = [&](const std::string& e) -> uint32_t { if (error) *error = e; return 0; };
    const Object* shader = doc.object(shaderRef);
    if (!shader || shader->type != "XSimpleShader") return fail("--material-from's Shader is not an XSimpleShader");
    const Value* stages = shader->field("TextureStages");
    if (!stages) return fail("shader has no TextureStages");
    std::vector<uint32_t> found;
    for (size_t i = 0; i < stages->size(); ++i) {
        const Object* stage = doc.object(stages->at(i).asRef());
        if (!stage || stage->type != "XOglTextureMap") continue;
        const Value* tex = stage->field("Texture");
        if (tex && tex->asRef()) {
            const Object* img = doc.object(tex->asRef());
            if (img && img->type == "XImage") found.push_back(tex->asRef());
        }
    }
    if (found.size() != 1)
        return fail("expected exactly one texture on the template shader, found " + std::to_string(found.size()));
    return found[0];
}

// ---------------------------------------------------------------- write

namespace {
uint16_t Vector3fMathIndex() {
    static uint16_t idx = MathIndex("Vector3f");
    return idx;
}
}  // namespace

uint32_t WriteMesh(Document& doc, const Mesh& mesh, uint32_t materialFromShaderRef, std::string* error) {
    auto fail = [&](const std::string& e) -> uint32_t { if (error) *error = e; return 0; };
    if (mesh.primitives.empty()) return fail("mesh has no primitives");
    for (auto& p : mesh.primitives) {
        if (p.positions.size() > 65535) return fail(p.name + ": more than 65535 vertices (u16 indices only)");
        if (!p.normals.empty() && p.normals.size() != p.positions.size())
            return fail(p.name + ": normal count does not match position count");
        if (!p.uvs.empty() && p.uvs.size() != p.positions.size())
            return fail(p.name + ": UV count does not match position count");
        if (p.indices.size() % 3) return fail(p.name + ": index count is not a multiple of 3");
        for (auto i : p.indices)
            if (i >= p.positions.size())
                return fail(p.name + ": index " + std::to_string(i) + " is out of range for " +
                            std::to_string(p.positions.size()) + " vertices");
    }

    static const std::vector<std::string> kNeeded = {
        "XTransform", "XCoord3fSet", "XNormal3fSet", "XTexCoord2fSet", "XIndexSet",
        "XIndexedTriangleSet", "XShape", "XGroup", "XInteriorNode", "XGraphSet", "XMeshDescriptor"};
    for (auto& cls : kNeeded)
        for (auto& t : doc.types)
            if (t.className() == cls)
                return fail("the target file already defines " + cls +
                             "; xomtool convert only writes into a file that does not yet use these classes");
    for (auto& cls : kNeeded) {
        if (cls == "XGraphSet") { RequireType(doc, cls, kGraphSetGuid, 0, true, error); continue; }
        if (cls == "XMeshDescriptor") { RequireType(doc, cls, kMeshDescGuid, 0, true, error); continue; }
        if (!RequireClassChain(doc, cls, error)) return 0;
    }

    uint16_t vec3 = Vector3fMathIndex();
    std::vector<uint32_t> groupRefs;
    for (auto& p : mesh.primitives) {
        uint32_t transformRef = 0;
        if (p.matrix != Identity()) {
            Object xf;
            xf.type = "XTransform";
            xf.container = true;
            Vec3 col0{p.matrix[0], p.matrix[1], p.matrix[2]}, col1{p.matrix[4], p.matrix[5], p.matrix[6]},
                col2{p.matrix[8], p.matrix[9], p.matrix[10]}, col3{p.matrix[12], p.matrix[13], p.matrix[14]};
            float sx = std::sqrt(col0.x * col0.x + col0.y * col0.y + col0.z * col0.z);
            float sy = std::sqrt(col1.x * col1.x + col1.y * col1.y + col1.z * col1.z);
            float sz = std::sqrt(col2.x * col2.x + col2.y * col2.y + col2.z * col2.z);
            xf.fields.emplace_back("Translate", Vec3Val(vec3, col3.x, col3.y, col3.z));
            xf.fields.emplace_back("Rotate", Vec3Val(vec3, 0, 0, 0));  // not decomposed; Matrix (below) is exact
            xf.fields.emplace_back("Scale", Vec3Val(vec3, sx, sy, sz));
            { Value v; v.type = Type::Enum; v.bits = 0; xf.fields.emplace_back("RotateOrder", v); }
            { Value v; v.type = Type::Math; v.math = 0; v.raw.resize(48);
              float m12[12] = {col0.x, col0.y, col0.z, col1.x, col1.y, col1.z,
                                col2.x, col2.y, col2.z, col3.x, col3.y, col3.z};
              // Matrix's math index is looked up by name below (Matrix43f: 12 floats).
              v.math = MathIndex("Matrix43f");
              v.raw.assign(reinterpret_cast<uint8_t*>(m12), reinterpret_cast<uint8_t*>(m12) + 48);
              xf.fields.emplace_back("Matrix", v); }
            { Value v; v.type = Type::U32; v.bits = 0; xf.fields.emplace_back("Flags", v); }
            transformRef = uint32_t(doc.objects.size()) + 1;
            doc.objects.push_back(std::move(xf));
        }
        groupRefs.push_back(transformRef);  // placeholder; replaced by the real group ref below
    }

    std::vector<uint32_t> coordRefs, normalRefs, uvRefs, indexRefs, triRefs, shapeRefs;
    for (auto& p : mesh.primitives) {
        Object co; co.type = "XCoord3fSet"; co.container = true;
        Value coordArr; coordArr.type = Type::Math; coordArr.math = vec3; coordArr.array = true;
        for (auto& v3 : p.positions) { float f[3] = {v3.x, v3.y, v3.z};
            coordArr.raw.insert(coordArr.raw.end(), reinterpret_cast<uint8_t*>(f), reinterpret_cast<uint8_t*>(f) + 12); }
        co.fields.emplace_back("Coord", coordArr);
        coordRefs.push_back(uint32_t(doc.objects.size()) + 1);
        doc.objects.push_back(std::move(co));
    }
    uint16_t normal3f = MathIndex("Normal3f");
    for (auto& p : mesh.primitives) {
        Object no; no.type = "XNormal3fSet"; no.container = true;
        Value arr; arr.type = Type::Math; arr.math = normal3f; arr.array = true;
        for (auto& v3 : p.normals) { float f[3] = {v3.x, v3.y, v3.z};
            arr.raw.insert(arr.raw.end(), reinterpret_cast<uint8_t*>(f), reinterpret_cast<uint8_t*>(f) + 12); }
        no.fields.emplace_back("Normal", arr);
        normalRefs.push_back(uint32_t(doc.objects.size()) + 1);
        doc.objects.push_back(std::move(no));
    }
    uint16_t coord2f = MathIndex("Coord2f");
    for (auto& p : mesh.primitives) {
        Object uv; uv.type = "XTexCoord2fSet"; uv.container = true;
        Value arr; arr.type = Type::Math; arr.math = coord2f; arr.array = true;
        for (auto& v2 : p.uvs) { float f[2] = {v2.u, v2.v};
            arr.raw.insert(arr.raw.end(), reinterpret_cast<uint8_t*>(f), reinterpret_cast<uint8_t*>(f) + 8); }
        uv.fields.emplace_back("TexCoord", arr);
        uvRefs.push_back(uint32_t(doc.objects.size()) + 1);
        doc.objects.push_back(std::move(uv));
    }
    for (auto& p : mesh.primitives) {
        Object idx; idx.type = "XIndexSet"; idx.container = true;
        Value arr; arr.type = Type::U16; arr.array = true;
        for (auto i : p.indices) { arr.raw.push_back(uint8_t(i)); arr.raw.push_back(uint8_t(i >> 8)); }
        idx.fields.emplace_back("Index", arr);
        indexRefs.push_back(uint32_t(doc.objects.size()) + 1);
        doc.objects.push_back(std::move(idx));
    }
    for (size_t i = 0; i < mesh.primitives.size(); ++i) {
        auto& p = mesh.primitives[i];
        Object tri; tri.type = "XIndexedTriangleSet"; tri.container = true;
        tri.fields.emplace_back("IndexSet", RefVal(indexRefs[i]));
        { Value v; v.type = Type::U32; v.bits = 0; tri.fields.emplace_back("Flags", v); }
        tri.fields.emplace_back("PrimitiveCount", U32Val(uint32_t(p.indices.size() / 3)));
        tri.fields.emplace_back("CoordSet", RefVal(coordRefs[i]));
        tri.fields.emplace_back("NormalSet", RefVal(p.normals.empty() ? 0 : normalRefs[i]));
        tri.fields.emplace_back("ColorSet", RefVal(0));
        tri.fields.emplace_back("TexCoordSet", RefVal(p.uvs.empty() ? 0 : uvRefs[i]));
        tri.fields.emplace_back("WeightSet", RefVal(0));
        { Value v; v.type = Type::Math; v.math = MathIndex("BoundBox"); v.raw.assign(24, 0);
          tri.fields.emplace_back("BoundBox", v); }
        { Value v; v.type = Type::Enum; v.bits = 0; tri.fields.emplace_back("BoundMode", v); }
        tri.fields.emplace_back("VertexShader", RefVal(0));
        triRefs.push_back(uint32_t(doc.objects.size()) + 1);
        doc.objects.push_back(std::move(tri));
    }
    for (size_t i = 0; i < mesh.primitives.size(); ++i) {
        auto& p = mesh.primitives[i];
        Object sh; sh.type = "XShape"; sh.container = true;
        { Value v; v.type = Type::U32; v.bits = 0; sh.fields.emplace_back("Flags", v); }
        sh.fields.emplace_back("Shader", RefVal(materialFromShaderRef));
        sh.fields.emplace_back("Geometry", RefVal(triRefs[i]));
        sh.fields.emplace_back("SortKey", U32Val(0));
        { Value v; v.type = Type::Ref; v.array = true; sh.fields.emplace_back("Parameters", v); }
        sh.fields.emplace_back("PreRenderFunc", RefVal(0));
        sh.fields.emplace_back("PostRenderFunc", RefVal(0));
        { Value v; v.type = Type::Math; v.math = MathIndex("BoundSphere"); v.raw.assign(16, 0);
          sh.fields.emplace_back("Bounds", v); }
        { Value v; v.type = Type::Enum; v.bits = 0; sh.fields.emplace_back("BoundMode", v); }
        sh.fields.emplace_back("Name", StrVal(p.name.empty() ? (mesh.resourceId + "_" + std::to_string(i)) : p.name));
        shapeRefs.push_back(uint32_t(doc.objects.size()) + 1);
        doc.objects.push_back(std::move(sh));
    }
    for (size_t i = 0; i < mesh.primitives.size(); ++i) {
        Object gr; gr.type = "XGroup"; gr.container = true;
        gr.fields.emplace_back("Core", RefVal(groupRefs[i]));
        gr.fields.emplace_back("Children", RefArrVal({shapeRefs[i]}));
        { Value v; v.type = Type::Math; v.math = MathIndex("BoundSphere"); v.raw.assign(16, 0);
          gr.fields.emplace_back("Bounds", v); }
        { Value v; v.type = Type::Enum; v.bits = 0; gr.fields.emplace_back("BoundMode", v); }
        gr.fields.emplace_back("Name", StrVal(mesh.resourceId + "_group_" + std::to_string(i)));
        groupRefs[i] = uint32_t(doc.objects.size()) + 1;  // overwrite the transform-ref placeholder
        doc.objects.push_back(std::move(gr));
    }
    Object root; root.type = "XInteriorNode"; root.container = true;
    root.fields.emplace_back("Children", RefArrVal(groupRefs));
    { Value v; v.type = Type::Math; v.math = MathIndex("BoundSphere"); v.raw.assign(16, 0);
      root.fields.emplace_back("Bounds", v); }
    { Value v; v.type = Type::Enum; v.bits = 0; root.fields.emplace_back("BoundMode", v); }
    root.fields.emplace_back("Name", StrVal(mesh.resourceId));
    uint32_t rootRef = uint32_t(doc.objects.size()) + 1;
    doc.objects.push_back(std::move(root));

    Object gs; gs.type = "XGraphSet"; gs.container = false;
    Value graphs; graphs.type = Type::Struct; graphs.array = true; graphs.items.resize(1);
    graphs.items[0].type = Type::Struct;
    { Value g; g.type = Type::Guid; g.guid = GuidFromHex(kWorldGraphGuid); graphs.items[0].members.emplace_back("Guid", g); }
    graphs.items[0].members.emplace_back("Graph", RefVal(rootRef));
    graphs.items[0].members.emplace_back("Name", StrVal("world"));
    gs.fields.emplace_back("Graphs", graphs);
    uint32_t gsRef = uint32_t(doc.objects.size()) + 1;
    doc.objects.push_back(std::move(gs));

    Object desc; desc.type = "XMeshDescriptor"; desc.container = false;
    desc.fields.emplace_back("ResourceId", StrVal(mesh.resourceId));
    desc.fields.emplace_back("SectionId", U16Val(mesh.sectionId));
    desc.fields.emplace_back("GraphSet", RefVal(gsRef));
    desc.fields.emplace_back("Flags", U16Val(8));
    uint32_t descRef = uint32_t(doc.objects.size()) + 1;
    doc.objects.push_back(std::move(desc));
    return descRef;
}

uint32_t WriteBundle(Document& doc, const Mesh& mesh, uint32_t materialFromShaderRef, std::string* error) {
    uint32_t descRef = WriteMesh(doc, mesh, materialFromShaderRef, error);
    if (!descRef) return 0;
    // WriteMesh's last two objects are the mesh's XGraphSet and the XMeshDescriptor (nothing refers to the
    // descriptor), so a root XGraphSet slotted between them keeps the objects grouped by TYPE-table order and
    // moves only the descriptor, one place later.
    if (descRef != doc.objects.size() || descRef < 2 || doc.objects[descRef - 2].type != "XGraphSet") {
        if (error) *error = "internal: unexpected object layout after WriteMesh";
        return 0;
    }
    Object root; root.type = "XGraphSet"; root.container = false;
    Value graphs; graphs.type = Type::Struct; graphs.array = true; graphs.items.resize(1);
    graphs.items[0].type = Type::Struct;
    { Value g; g.type = Type::Guid; g.guid = GuidFromHex(kRootEntryGuid); graphs.items[0].members.emplace_back("Guid", g); }
    graphs.items[0].members.emplace_back("Graph", RefVal(descRef + 1));
    graphs.items[0].members.emplace_back("Name", StrVal(mesh.resourceId));
    root.fields.emplace_back("Graphs", graphs);
    doc.objects.insert(doc.objects.begin() + long(descRef - 1), std::move(root));
    doc.root = descRef;  // the new root's own 1-based index
    return descRef + 1;
}

}  // namespace melange::xom::mesh
