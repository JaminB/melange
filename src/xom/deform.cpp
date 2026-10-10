// melange::xom::mesh - see deform.h.
#include "deform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "json.h"

namespace melange::xom::mesh {
namespace {

struct V3 { float x = 0, y = 0, z = 0; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
float& Comp(V3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }
float Comp(const V3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

// ---------------------------------------------------------------- script

struct Op {
    enum Kind { Scale, Translate, Bend, Push, Noise, Region } kind = Scale;
    std::vector<std::string> select;  // top level only; empty = inherit
    V3 s{1, 1, 1}, t, about, boxLo, boxHi;
    bool hasAbout = false, hasLength = false;
    int axis = 1, dir = 2;
    float amountDeg = 0, length = 0, dist = 0, amp = 0, freq = 1, falloff = 0;
    uint32_t seed = 0;
    bool vectorNoise = false;
    std::vector<Op> then;
};

struct Parser {
    std::string err;
    bool fail(const std::string& e) { if (err.empty()) err = e; return false; }

    bool Num(const Json* j, float& out, const std::string& what) {
        if (!j || j->kind != Json::Kind::Number) return fail(what + " must be a number");
        bool ok = true;
        double d = j->asDouble(&ok);
        if (!ok || !std::isfinite(d)) return fail(what + " is not a finite number");
        out = float(d);
        return true;
    }
    bool Vec(const Json* j, V3& out, const std::string& what) {
        if (!j || j->kind != Json::Kind::Array || j->arr.size() != 3) return fail(what + " must be [x, y, z]");
        return Num(&j->arr[0], out.x, what) && Num(&j->arr[1], out.y, what) && Num(&j->arr[2], out.z, what);
    }
    bool Axis(const Json* j, int& out, const std::string& what) {
        if (!j || j->kind != Json::Kind::String || j->str.size() != 1) return fail(what + " must be \"x\", \"y\" or \"z\"");
        char c = char(std::tolower(uint8_t(j->str[0])));
        if (c < 'x' || c > 'z') return fail(what + " must be \"x\", \"y\" or \"z\"");
        out = c - 'x';
        return true;
    }

    bool ParseOps(const Json& arr, std::vector<Op>& out, bool top, const std::string& where) {
        if (arr.kind != Json::Kind::Array) return fail(where + " must be an array of ops");
        for (size_t i = 0; i < arr.arr.size(); ++i) {
            Op op;
            if (!ParseOp(arr.arr[i], op, top, where + "[" + std::to_string(i) + "]")) return false;
            out.push_back(std::move(op));
        }
        return true;
    }

    bool ParseOp(const Json& j, Op& op, bool top, const std::string& where) {
        if (j.kind != Json::Kind::Object) return fail(where + " must be an object");
        const Json* name = j.find("op");
        if (!name || name->kind != Json::Kind::String) return fail(where + " has no \"op\" name");
        const std::string& k = name->str;
        if (const Json* sel = j.find("select")) {
            if (!top) return fail(where + ": \"select\" is only valid on a top-level op (a region's ops use the region's selection)");
            if (sel->kind == Json::Kind::String) op.select.push_back(sel->str);
            else if (sel->kind == Json::Kind::Array) {
                for (auto& e : sel->arr) {
                    if (e.kind != Json::Kind::String) return fail(where + ": \"select\" entries must be strings");
                    op.select.push_back(e.str);
                }
            } else return fail(where + ": \"select\" must be a glob string or an array of them");
        }
        if (const Json* a = j.find("about")) { if (!Vec(a, op.about, where + ".about")) return false; op.hasAbout = true; }
        if (k == "scale") {
            op.kind = Op::Scale;
            const Json* s = j.find("s");
            if (!s) return fail(where + ": scale needs \"s\"");
            if (s->kind == Json::Kind::Number) { float v; if (!Num(s, v, where + ".s")) return false; op.s = {v, v, v}; }
            else if (!Vec(s, op.s, where + ".s")) return false;
        } else if (k == "translate") {
            op.kind = Op::Translate;
            if (!j.find("t")) return fail(where + ": translate needs \"t\"");
            if (!Vec(j.find("t"), op.t, where + ".t")) return false;
        } else if (k == "bend") {
            op.kind = Op::Bend;
            op.axis = 1;
            if (const Json* a = j.find("axis")) if (!Axis(a, op.axis, where + ".axis")) return false;
            op.dir = (op.axis + 1) % 3;
            if (const Json* a = j.find("dir")) if (!Axis(a, op.dir, where + ".dir")) return false;
            if (op.dir == op.axis) return fail(where + ": bend \"dir\" must differ from \"axis\"");
            if (!j.find("amount")) return fail(where + ": bend needs \"amount\" (degrees)");
            if (!Num(j.find("amount"), op.amountDeg, where + ".amount")) return false;
            if (const Json* l = j.find("length")) {
                if (!Num(l, op.length, where + ".length")) return false;
                if (!(op.length > 0)) return fail(where + ": bend \"length\" must be positive");
                op.hasLength = true;
            }
        } else if (k == "push") {
            op.kind = Op::Push;
            if (!j.find("dist")) return fail(where + ": push needs \"dist\"");
            if (!Num(j.find("dist"), op.dist, where + ".dist")) return false;
        } else if (k == "noise") {
            op.kind = Op::Noise;
            if (!j.find("amp")) return fail(where + ": noise needs \"amp\"");
            if (!Num(j.find("amp"), op.amp, where + ".amp")) return false;
            if (const Json* f = j.find("freq")) if (!Num(f, op.freq, where + ".freq")) return false;
            if (!(op.freq > 0)) return fail(where + ": noise \"freq\" must be positive");
            if (const Json* sd = j.find("seed")) {
                if (sd->kind != Json::Kind::Number) return fail(where + ".seed must be an integer");
                op.seed = uint32_t(sd->asInt64());
            }
            if (const Json* m = j.find("mode")) {
                if (m->kind != Json::Kind::String || (m->str != "normal" && m->str != "vector"))
                    return fail(where + ": noise \"mode\" must be \"normal\" or \"vector\"");
                op.vectorNoise = m->str == "vector";
            }
        } else if (k == "region") {
            op.kind = Op::Region;
            const Json* b = j.find("box");
            if (!b || b->kind != Json::Kind::Array) return fail(where + ": region needs \"box\": [[x0,y0,z0],[x1,y1,z1]]");
            V3 lo, hi;
            if (b->arr.size() == 2) { if (!Vec(&b->arr[0], lo, where + ".box[0]") || !Vec(&b->arr[1], hi, where + ".box[1]")) return false; }
            else if (b->arr.size() == 6) {
                float f[6];
                for (size_t i = 0; i < 6; ++i) if (!Num(&b->arr[i], f[i], where + ".box")) return false;
                lo = {f[0], f[1], f[2]};
                hi = {f[3], f[4], f[5]};
            } else return fail(where + ": region \"box\" must be [min, max] (two [x,y,z]) or six numbers");
            op.boxLo = {std::min(lo.x, hi.x), std::min(lo.y, hi.y), std::min(lo.z, hi.z)};
            op.boxHi = {std::max(lo.x, hi.x), std::max(lo.y, hi.y), std::max(lo.z, hi.z)};
            if (const Json* f = j.find("falloff")) {
                if (!Num(f, op.falloff, where + ".falloff")) return false;
                if (op.falloff < 0) return fail(where + ": region \"falloff\" must not be negative");
            }
            const Json* th = j.find("then");
            if (!th) return fail(where + ": region needs \"then\": [ops]");
            if (!ParseOps(*th, op.then, false, where + ".then")) return false;
        } else {
            return fail(where + ": unknown op \"" + k + "\" (scale, translate, bend, push, noise, region)");
        }
        return true;
    }
};

// ---------------------------------------------------------------- noise

uint32_t Hash(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed * 0x9E3779B1u ^ uint32_t(x) * 0x85EBCA6Bu ^ uint32_t(y) * 0xC2B2AE35u ^ uint32_t(z) * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
float Lattice(int32_t x, int32_t y, int32_t z, uint32_t seed) { return float(Hash(x, y, z, seed) & 0xFFFFFFu) / 8388607.5f - 1.0f; }
float ValueNoise(V3 p, uint32_t seed) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int32_t ix = int32_t(fx), iy = int32_t(fy), iz = int32_t(fz);
    auto sm = [](float t) { return t * t * (3 - 2 * t); };
    const float tx = sm(p.x - fx), ty = sm(p.y - fy), tz = sm(p.z - fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float c[2][2][2];
    for (int dz = 0; dz < 2; ++dz) for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx) c[dz][dy][dx] = Lattice(ix + dx, iy + dy, iz + dz, seed);
    float y0 = lerp(lerp(c[0][0][0], c[0][0][1], tx), lerp(c[0][1][0], c[0][1][1], tx), ty);
    float y1 = lerp(lerp(c[1][0][0], c[1][0][1], tx), lerp(c[1][1][0], c[1][1][1], tx), ty);
    return lerp(y0, y1, tz);
}

// ---------------------------------------------------------------- shapes under edit

struct Target {
    ShapeRef ref;
    uint32_t coordRef = 0, normalRef = 0, geometryRef = 0;
    std::vector<V3> orig, pos;
    std::vector<uint32_t> indices;
    std::vector<V3> origNormals;  // stored (may be empty)
    float winding = 1;            // +1 when the stored normals agree with counter-clockwise triangle winding
    bool selectedByAny = false;
};

// Per-vertex area-weighted normals of the triangles using each index (indices that share a vertex share its normal;
// duplicates split for a hard edge or a UV seam keep their own). `weld`: also sum across vertices at the same position.
std::vector<V3> VertexNormals(const std::vector<V3>& pos, const std::vector<uint32_t>& idx, bool weld, float winding,
                              std::vector<char>* hasArea = nullptr) {
    std::vector<V3> n(pos.size());
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a >= pos.size() || b >= pos.size() || c >= pos.size()) continue;
        V3 fn = Cross(pos[b] - pos[a], pos[c] - pos[a]) * winding;  // length = 2 * area, so this is area-weighted
        n[a] = n[a] + fn;
        n[b] = n[b] + fn;
        n[c] = n[c] + fn;
    }
    if (weld) {
        std::map<std::array<uint32_t, 3>, V3> sum;
        auto key = [&](const V3& p) {
            std::array<uint32_t, 3> k;
            const float x = p.x + 0.0f, y = p.y + 0.0f, z = p.z + 0.0f;  // -0 and +0 are the same place
            std::memcpy(&k[0], &x, 4); std::memcpy(&k[1], &y, 4); std::memcpy(&k[2], &z, 4);
            return k;
        };
        for (size_t i = 0; i < pos.size(); ++i) { V3& s = sum[key(pos[i])]; s = s + n[i]; }
        for (size_t i = 0; i < pos.size(); ++i) n[i] = sum[key(pos[i])];
    }
    if (hasArea) hasArea->assign(pos.size(), 0);
    for (size_t i = 0; i < n.size(); ++i) {
        float l = Len(n[i]);
        if (l > 1e-20f) { n[i] = n[i] * (1.0f / l); if (hasArea) (*hasArea)[i] = 1; }
        else n[i] = {0, 0, 0};
    }
    return n;
}

struct Ctx {
    V3 lo, hi;  // bounds the default pivots come from: the selection's, or the enclosing region's box
};

float Smooth(float t) { t = std::min(1.0f, std::max(0.0f, t)); return t * t * (3 - 2 * t); }

void ApplyOp(const Op& op, const Ctx& ctx, Target& tg, std::vector<V3>& pos);

void ApplyList(const std::vector<Op>& ops, const Ctx& ctx, Target& tg, std::vector<V3>& pos) {
    for (auto& op : ops) ApplyOp(op, ctx, tg, pos);
}

void ApplyOp(const Op& op, const Ctx& ctx, Target& tg, std::vector<V3>& pos) {
    const V3 centre = (ctx.lo + ctx.hi) * 0.5f;
    switch (op.kind) {
        case Op::Scale: {
            const V3 about = op.hasAbout ? op.about : centre;
            for (auto& p : pos) p = {about.x + (p.x - about.x) * op.s.x, about.y + (p.y - about.y) * op.s.y, about.z + (p.z - about.z) * op.s.z};
            break;
        }
        case Op::Translate:
            for (auto& p : pos) p = p + op.t;
            break;
        case Op::Bend: {
            V3 about = op.about;
            if (!op.hasAbout) {
                about = centre;
                Comp(about, op.axis) = Comp(ctx.lo, op.axis);
            }
            const float a0 = Comp(about, op.axis), d0 = Comp(about, op.dir);
            const float L = op.hasLength ? op.length : std::max(1e-6f, Comp(ctx.hi, op.axis) - a0);
            const float total = op.amountDeg * 3.14159265358979f / 180.0f;
            if (std::fabs(total) < 1e-9f) break;
            const float k = total / L;         // curvature, radians per unit along the axis
            const float r = 1.0f / k;          // signed radius; the circle's centre lies at +r along dir
            for (auto& p : pos) {
                const float a = Comp(p, op.axis) - a0, d = Comp(p, op.dir) - d0;
                if (a <= 0) continue;          // before the pivot: untouched
                const float ac = std::min(a, L), e = a - ac;
                const float phi = k * ac, rho = r - d;
                // Point on the arc at arc length `ac`, offset by d across it, then the straight run beyond the arc end
                // along the final tangent.
                float na = rho * std::sin(phi) + e * std::cos(phi);
                float nd = r - rho * std::cos(phi) + e * std::sin(phi);
                Comp(p, op.axis) = a0 + na;
                Comp(p, op.dir) = d0 + nd;
            }
            break;
        }
        case Op::Push: {
            auto n = VertexNormals(pos, tg.indices, true, tg.winding);
            for (size_t i = 0; i < pos.size(); ++i) pos[i] = pos[i] + n[i] * op.dist;
            break;
        }
        case Op::Noise: {
            const uint32_t seed = op.seed;
            if (op.vectorNoise) {
                for (auto& p : pos) {
                    V3 q = p * op.freq;
                    p = p + V3{ValueNoise(q, seed), ValueNoise(q, seed + 1013u), ValueNoise(q, seed + 2027u)} * op.amp;
                }
            } else {
                auto n = VertexNormals(pos, tg.indices, true, tg.winding);
                for (size_t i = 0; i < pos.size(); ++i) pos[i] = pos[i] + n[i] * (ValueNoise(pos[i] * op.freq, seed) * op.amp);
            }
            break;
        }
        case Op::Region: {
            std::vector<float> w(pos.size());
            for (size_t i = 0; i < pos.size(); ++i) {
                const V3& p = pos[i];
                V3 d{std::max({op.boxLo.x - p.x, 0.0f, p.x - op.boxHi.x}), std::max({op.boxLo.y - p.y, 0.0f, p.y - op.boxHi.y}),
                     std::max({op.boxLo.z - p.z, 0.0f, p.z - op.boxHi.z})};
                const float dist = Len(d);
                w[i] = dist <= 0 ? 1.0f : (op.falloff > 0 ? Smooth(1.0f - dist / op.falloff) : 0.0f);
            }
            Ctx inner = ctx;
            inner.lo = op.boxLo;
            inner.hi = op.boxHi;
            std::vector<V3> moved = pos;
            ApplyList(op.then, inner, tg, moved);
            for (size_t i = 0; i < pos.size(); ++i) pos[i] = pos[i] + (moved[i] - pos[i]) * w[i];
            break;
        }
    }
}

}  // namespace

bool GlobMatch(const std::string& pattern, const std::string& text) {
    auto lc = [](char c) { return char(std::tolower(uint8_t(c))); };
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || lc(pattern[p]) == lc(text[t]))) { ++p; ++t; }
        else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = t; }
        else if (star != std::string::npos) { p = star + 1; t = ++mark; }
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool ApplyDeform(Document& doc, uint32_t descRef, const std::string& scriptJson, DeformReport* report, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = "deform: " + e; return false; };

    Json root;
    std::string perr;
    if (!ParseJson(scriptJson, root, &perr)) return fail("the script is not valid JSON: " + perr);
    Parser ps;
    std::vector<Op> ops;
    std::vector<std::string> defaultSelect;
    if (root.kind == Json::Kind::Array) {
        if (!ps.ParseOps(root, ops, true, "ops")) return fail(ps.err);
    } else if (root.kind == Json::Kind::Object) {
        if (const Json* sel = root.find("select")) {
            if (sel->kind == Json::Kind::String) defaultSelect.push_back(sel->str);
            else if (sel->kind == Json::Kind::Array) {
                for (auto& e : sel->arr) {
                    if (e.kind != Json::Kind::String) return fail("\"select\" entries must be strings");
                    defaultSelect.push_back(e.str);
                }
            } else return fail("\"select\" must be a glob string or an array of them");
        }
        const Json* o = root.find("ops");
        if (!o) return fail("the script object needs an \"ops\" array");
        if (!ps.ParseOps(*o, ops, true, "ops")) return fail(ps.err);
    } else {
        return fail("the script must be an array of ops or an object with \"ops\"");
    }
    if (ops.empty()) return fail("the script has no ops");

    std::vector<ShapeRef> shapes;
    if (!EnumerateShapes(doc, descRef, shapes, error)) return false;

    // One Target per distinct CoordSet: two shapes sharing one coordinate array are one edit (and must be selected
    // together, or the unselected one would move too).
    std::vector<Target> targets;
    std::unordered_map<uint32_t, size_t> byCoord;
    for (auto& s : shapes) {
        if (!s.geometry) continue;
        const Object* geom = doc.object(s.geometry);
        const Value* cs = geom->field("CoordSet");
        const Object* co = cs ? doc.object(cs->asRef()) : nullptr;
        const Value* cv = co ? co->field("Coord") : nullptr;
        if (!cv) continue;
        auto it = byCoord.find(cs->asRef());
        if (it != byCoord.end()) continue;  // shared coordinate array: one edit, checked against the selections below
        Target tg;
        tg.ref = s;
        tg.coordRef = cs->asRef();
        tg.geometryRef = s.geometry;
        if (const Value* ns = geom->field("NormalSet")) tg.normalRef = ns->asRef();
        auto f = ReadFloatArray(*cv);
        for (size_t i = 0; i + 2 < f.size(); i += 3) tg.orig.push_back({f[i], f[i + 1], f[i + 2]});
        const Object* idxO = doc.object(geom->field("IndexSet") ? geom->field("IndexSet")->asRef() : 0);
        const Value* idx = idxO ? idxO->field("Index") : nullptr;
        if (idx) for (size_t i = 0; i < idx->size(); ++i) tg.indices.push_back(uint32_t(idx->at(i).asUInt()));
        tg.pos = tg.orig;
        byCoord[tg.coordRef] = targets.size();
        targets.push_back(std::move(tg));
    }
    if (targets.empty()) return fail("the mesh has no triangle shapes to deform");

    // Which way the stored normals face relative to triangle winding, so recomputed ones agree with them.
    for (auto& tg : targets) {
        const Object* no = doc.object(tg.normalRef);
        const Value* nv = no ? no->field("Normal") : nullptr;
        auto f = nv ? ReadFloatArray(*nv) : std::vector<float>();
        for (size_t i = 0; i + 2 < f.size(); i += 3) tg.origNormals.push_back({f[i], f[i + 1], f[i + 2]});
        if (tg.origNormals.size() == tg.orig.size()) {
            auto mine = VertexNormals(tg.orig, tg.indices, false, 1.0f);
            double agree = 0;
            for (size_t i = 0; i < mine.size(); ++i) agree += double(Dot(mine[i], tg.origNormals[i]));
            tg.winding = agree < 0 ? -1.0f : 1.0f;
        } else {
            tg.origNormals.clear();
        }
    }

    // Resolve every op's selection first so a typo fails before anything moves.
    std::vector<std::vector<size_t>> selected(ops.size());
    for (size_t oi = 0; oi < ops.size(); ++oi) {
        const std::vector<std::string>& globs = ops[oi].select.empty() ? (defaultSelect.empty() ? std::vector<std::string>{"*"} : defaultSelect) : ops[oi].select;
        for (size_t ti = 0; ti < targets.size(); ++ti) {
            bool hit = false;
            for (auto& g : globs) {
                if (GlobMatch(g, targets[ti].ref.name)) hit = true;
                for (auto& n : targets[ti].ref.path) if (GlobMatch(g, n)) hit = true;
            }
            if (hit) { selected[oi].push_back(ti); targets[ti].selectedByAny = true; }
        }
        if (selected[oi].empty()) {
            std::string g;
            for (auto& s : globs) g += (g.empty() ? "" : ", ") + s;
            std::string have;
            for (auto& t : targets) have += (have.empty() ? "" : ", ") + t.ref.name;
            return fail("op " + std::to_string(oi) + ": \"" + g + "\" matches no shape (shapes: " + have + ")");
        }
    }
    // A coordinate array drawn by two shapes moves for both; refuse unless the second is selected too.
    for (auto& s : shapes) {
        if (!s.geometry) continue;
        const Value* cs = doc.object(s.geometry)->field("CoordSet");
        if (!cs) continue;
        auto it = byCoord.find(cs->asRef());
        if (it == byCoord.end() || targets[it->second].ref.shape == s.shape) continue;
        // A Target carries one index list and one normal set (the first shape's). A second shape with a different triangle
        // list or normal set over the same coordinates would be deformed and re-normalled from the wrong triangles, so
        // refuse rather than leave it with stale normals.
        {
            const Object* mine = doc.object(s.geometry);
            const Value* ia = mine->field("IndexSet");
            const Value* na = mine->field("NormalSet");
            const uint32_t idxRef = ia ? ia->asRef() : 0, nrmRef = na ? na->asRef() : 0;
            const Object* theirs = doc.object(targets[it->second].geometryRef);
            const Value* ib = theirs->field("IndexSet");
            if (idxRef != (ib ? ib->asRef() : 0) || nrmRef != targets[it->second].normalRef)
                return fail("shapes \"" + targets[it->second].ref.name + "\" and \"" + s.name +
                            "\" share one coordinate array but not the same index and normal sets; deforming them together is not supported");
        }
        for (size_t oi = 0; oi < ops.size(); ++oi) {
            bool a = std::find(selected[oi].begin(), selected[oi].end(), it->second) != selected[oi].end();
            bool b = false;
            const std::vector<std::string>& globs = ops[oi].select.empty() ? (defaultSelect.empty() ? std::vector<std::string>{"*"} : defaultSelect) : ops[oi].select;
            for (auto& g : globs) { if (GlobMatch(g, s.name)) b = true; for (auto& n : s.path) if (GlobMatch(g, n)) b = true; }
            if (a != b)
                return fail("shapes \"" + targets[it->second].ref.name + "\" and \"" + s.name + "\" share one coordinate array, so op " +
                            std::to_string(oi) + " would move both; select both or neither");
        }
    }

    // Run the ops. Bounds for default pivots are the selection's current union.
    for (size_t oi = 0; oi < ops.size(); ++oi) {
        Ctx ctx;
        bool first = true;
        for (auto ti : selected[oi])
            for (auto& p : targets[ti].pos) {
                if (first) { ctx.lo = ctx.hi = p; first = false; continue; }
                ctx.lo = {std::min(ctx.lo.x, p.x), std::min(ctx.lo.y, p.y), std::min(ctx.lo.z, p.z)};
                ctx.hi = {std::max(ctx.hi.x, p.x), std::max(ctx.hi.y, p.y), std::max(ctx.hi.z, p.z)};
            }
        for (auto ti : selected[oi]) ApplyOp(ops[oi], ctx, targets[ti], targets[ti].pos);
    }

    for (auto& tg : targets) {
        if (!tg.selectedByAny) continue;
        for (size_t i = 0; i < tg.pos.size(); ++i)
            if (!std::isfinite(Len(tg.pos[i] - tg.orig[i])))
                return fail("shape \"" + tg.ref.name + "\" produced a non-finite coordinate (check scale/bend values)");
    }

    // Everything computed; from here on only writes. Coordinates, then the normals of the vertices the edit reached.
    float maxDisp = 0;
    DeformReport rep;
    for (auto& tg : targets) {
        if (!tg.selectedByAny) continue;
        DeformReport::Shape sr;
        sr.name = tg.ref.name;
        sr.skinned = tg.ref.skinned;
        sr.vertices = tg.pos.size();
        std::vector<char> movedV(tg.pos.size(), 0);
        for (size_t i = 0; i < tg.pos.size(); ++i) {
            const V3 d = tg.pos[i] - tg.orig[i];
            const float m = Len(d);
            if (m > 0) { movedV[i] = 1; ++sr.moved; sr.maxMove = std::max(sr.maxMove, m); }
        }
        maxDisp = std::max(maxDisp, sr.maxMove);
        if (sr.moved) {
            std::vector<float> flat;
            flat.reserve(tg.pos.size() * 3);
            for (auto& p : tg.pos) { flat.push_back(p.x); flat.push_back(p.y); flat.push_back(p.z); }
            Object* co = doc.object(tg.coordRef);
            WriteFloatArray(*co->field("Coord"), flat);

            // A vertex needs a new normal if any triangle that uses it has a moved corner.
            std::vector<char> touched(tg.pos.size(), 0);
            for (size_t t = 0; t + 2 < tg.indices.size(); t += 3) {
                const uint32_t a = tg.indices[t], b = tg.indices[t + 1], c = tg.indices[t + 2];
                if (a >= movedV.size() || b >= movedV.size() || c >= movedV.size()) continue;
                if (movedV[a] || movedV[b] || movedV[c]) touched[a] = touched[b] = touched[c] = 1;
            }
            Object* no = doc.object(tg.normalRef);
            Value* nv = no ? no->field("Normal") : nullptr;
            if (nv && tg.origNormals.size() == tg.pos.size()) {
                std::vector<char> hasArea;
                auto fresh = VertexNormals(tg.pos, tg.indices, false, tg.winding, &hasArea);
                std::vector<float> nf;
                nf.reserve(tg.pos.size() * 3);
                for (size_t i = 0; i < tg.pos.size(); ++i) {
                    V3 n = tg.origNormals[i];
                    if (touched[i] && hasArea[i]) { n = fresh[i]; ++sr.renormalised; }
                    nf.push_back(n.x); nf.push_back(n.y); nf.push_back(n.z);
                }
                WriteFloatArray(*nv, nf);
            } else if (tg.normalRef) {
                rep.notes.push_back("shape \"" + tg.ref.name + "\": its normal set does not match its vertex count, normals left alone");
            }

            // The triangle set's box is the new exact box.
            Object* geom = doc.object(tg.geometryRef);
            if (Value* bb = geom->field("BoundBox")) if (bb->type == Type::Math && bb->raw.size() == 24) {
                V3 lo = tg.pos[0], hi = tg.pos[0];
                for (auto& p : tg.pos) {
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
                float f[6] = {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z};
                std::memcpy(bb->raw.data(), f, 24);
            }
            if (tg.ref.skinned)
                rep.notes.push_back("skinned shape \"" + tg.ref.name + "\" was deformed in bind pose; XBone PoseMatrix and the skin weights are untouched");
        }
        rep.shapes.push_back(std::move(sr));
    }
    rep.maxDisplacement = maxDisp;

    // Bound spheres only ever need to grow: every vertex stayed within maxDisp of where it was, so adding that to the
    // radius of each non-empty sphere above the shapes keeps the old guarantee (sphere (x,y,z,r) with r > 0). Only the
    // scene graph (Children chain from the world root) is touched, not a skin's skeleton.
    if (maxDisp > 0) {
        std::vector<uint32_t> stack{WorldRoot(doc, descRef)};
        std::unordered_set<uint32_t> seen;
        while (!stack.empty()) {
            const uint32_t r = stack.back();
            stack.pop_back();
            if (!r || !doc.object(r) || !seen.insert(r).second) continue;
            Object& o = doc.objects[r - 1];
            if (const Value* ch = o.field("Children"))
                for (size_t i = 0; i < ch->size(); ++i) stack.push_back(ch->at(i).asRef());
            if (o.type != "XShape" && o.type != "XSkinShape" && o.type != "XGroup" && o.type != "XInteriorNode" && o.type != "XSkin") continue;
            Value* b = o.field("Bounds");
            if (!b || b->type != Type::Math) continue;
            auto cmp = b->components();
            if (cmp.size() != 4 || !(cmp[3] > 0)) continue;
            cmp[3] += double(maxDisp);
            b->setComponents(cmp);
        }
    }
    if (report) *report = std::move(rep);
    return true;
}

}  // namespace melange::xom::mesh
