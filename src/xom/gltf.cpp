// melange::xom::gltf - see gltf.h.
#include "gltf.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include "json.h"

namespace melange::xom::gltf {
namespace {

using mesh::Mat4;

Mat4 QuatToMatrix(float x, float y, float z, float w) {
    Mat4 m = mesh::Identity();
    float xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;
    m[0] = 1 - 2 * (yy + zz); m[1] = 2 * (xy + wz);     m[2] = 2 * (xz - wy);     m[3] = 0;
    m[4] = 2 * (xy - wz);     m[5] = 1 - 2 * (xx + zz); m[6] = 2 * (yz + wx);     m[7] = 0;
    m[8] = 2 * (xz + wy);     m[9] = 2 * (yz - wx);     m[10] = 1 - 2 * (xx + yy); m[11] = 0;
    m[12] = 0; m[13] = 0; m[14] = 0; m[15] = 1;
    return m;
}
Mat4 ScaleMatrix(float sx, float sy, float sz) {
    Mat4 m = mesh::Identity();
    m[0] = sx; m[5] = sy; m[10] = sz;
    return m;
}
Mat4 TranslateMatrix(float x, float y, float z) {
    Mat4 m = mesh::Identity();
    m[12] = x; m[13] = y; m[14] = z;
    return m;
}

bool NodeMatrix(const Json& node, Mat4& out) {
    if (const Json* m = node.find("matrix")) {
        if (m->kind != Json::Kind::Array || m->arr.size() != 16) return false;
        for (int i = 0; i < 16; ++i) out[size_t(i)] = float(m->arr[size_t(i)].asDouble());
        return true;
    }
    Mat4 t = mesh::Identity(), r = mesh::Identity(), s = mesh::Identity();
    if (const Json* v = node.find("translation"))
        if (v->arr.size() == 3) t = TranslateMatrix(float(v->arr[0].asDouble()), float(v->arr[1].asDouble()), float(v->arr[2].asDouble()));
    if (const Json* v = node.find("rotation"))
        if (v->arr.size() == 4)
            r = QuatToMatrix(float(v->arr[0].asDouble()), float(v->arr[1].asDouble()), float(v->arr[2].asDouble()),
                              float(v->arr[3].asDouble()));
    if (const Json* v = node.find("scale"))
        if (v->arr.size() == 3) s = ScaleMatrix(float(v->arr[0].asDouble()), float(v->arr[1].asDouble()), float(v->arr[2].asDouble()));
    out = mesh::Multiply(mesh::Multiply(t, r), s);
    return true;
}

size_t ComponentSize(int64_t componentType) {
    switch (componentType) {
        case 5120: case 5121: return 1;  // BYTE/UNSIGNED_BYTE
        case 5122: case 5123: return 2;  // SHORT/UNSIGNED_SHORT
        case 5125: case 5126: return 4;  // UNSIGNED_INT/FLOAT
        default: return 0;
    }
}
int TypeCount(const std::string& t) {
    if (t == "SCALAR") return 1;
    if (t == "VEC2") return 2;
    if (t == "VEC3") return 3;
    if (t == "VEC4") return 4;
    return 0;
}

struct Accessor {
    int64_t bufferView = -1, byteOffset = 0, componentType = 0, count = 0;
    std::string type;
    bool normalized = false;
};

bool ReadAccessorFloats(const Accessor& a, const Json& bufferViews, const std::vector<uint8_t>& bin,
                         std::vector<double>& out, int expectComponents, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    if (a.bufferView < 0 || a.bufferView >= int64_t(bufferViews.arr.size())) return fail("accessor: bad bufferView");
    const Json& bv = bufferViews.arr[size_t(a.bufferView)];
    int64_t byteOffset = bv.find("byteOffset") ? bv.find("byteOffset")->asInt64() : 0;
    int64_t byteStride = bv.find("byteStride") ? bv.find("byteStride")->asInt64() : 0;
    int nc = TypeCount(a.type);
    if (nc == 0 || nc != expectComponents) return fail("accessor: unexpected type " + a.type);
    size_t compSize = ComponentSize(a.componentType);
    if (!compSize) return fail("accessor: unsupported componentType");
    size_t stride = byteStride ? size_t(byteStride) : compSize * size_t(nc);
    size_t base = size_t(byteOffset + a.byteOffset);
    out.resize(size_t(a.count) * size_t(nc));
    for (int64_t i = 0; i < a.count; ++i) {
        for (int c = 0; c < nc; ++c) {
            size_t off = base + size_t(i) * stride + size_t(c) * compSize;
            if (off + compSize > bin.size()) return fail("accessor reads past the end of the buffer");
            double v = 0;
            if (a.componentType == 5126) {
                float f; std::memcpy(&f, bin.data() + off, 4); v = double(f);
            } else if (a.componentType == 5125) {
                uint32_t u; std::memcpy(&u, bin.data() + off, 4); v = double(u);
            } else if (a.componentType == 5123) {
                uint16_t u; std::memcpy(&u, bin.data() + off, 2); v = double(u);
            } else if (a.componentType == 5121) {
                v = double(bin[off]);
            } else if (a.componentType == 5122) {
                int16_t s16; std::memcpy(&s16, bin.data() + off, 2); v = double(s16);
            } else if (a.componentType == 5120) {
                v = double(int8_t(bin[off]));
            }
            out[size_t(i) * size_t(nc) + size_t(c)] = v;
        }
    }
    return true;
}

bool ReadAccessorIndices(const Accessor& a, const Json& bufferViews, const std::vector<uint8_t>& bin,
                          std::vector<uint32_t>& out, std::string* error) {
    std::vector<double> raw;
    if (!ReadAccessorFloats(a, bufferViews, bin, raw, 1, error)) return false;
    out.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) out[i] = uint32_t(raw[i]);
    return true;
}

void WalkNode(const Json& root, const Json& nodes, const Json& meshes, const Json& accessors, const Json& bufferViews,
              const std::vector<uint8_t>& bin, int64_t nodeIdx, const Mat4& parent, std::vector<mesh::Primitive>& out,
              bool& failed, std::string* error) {
    if (failed || nodeIdx < 0 || nodeIdx >= int64_t(nodes.arr.size())) return;
    const Json& node = nodes.arr[size_t(nodeIdx)];
    Mat4 local;
    NodeMatrix(node, local);
    Mat4 world = mesh::Multiply(parent, local);
    if (const Json* meshIdx = node.find("mesh")) {
        int64_t mi = meshIdx->asInt64();
        if (mi >= 0 && mi < int64_t(meshes.arr.size())) {
            const Json& m = meshes.arr[size_t(mi)];
            const Json* prims = m.find("primitives");
            if (prims)
                for (size_t pi = 0; pi < prims->arr.size(); ++pi) {
                    const Json& p = prims->arr[pi];
                    if (const Json* mode = p.find("mode"))
                        if (mode->asInt64() != 4) continue;  // TRIANGLES only
                    const Json* attrs = p.find("attributes");
                    if (!attrs) { failed = true; if (error) *error = "primitive has no attributes"; return; }
                    const Json* posIdx = attrs->find("POSITION");
                    if (!posIdx) { failed = true; if (error) *error = "primitive has no POSITION attribute"; return; }
                    auto getAccessor = [&](int64_t idx, Accessor& a) {
                        const Json& aj = accessors.arr[size_t(idx)];
                        a.bufferView = aj.find("bufferView") ? aj.find("bufferView")->asInt64() : -1;
                        a.byteOffset = aj.find("byteOffset") ? aj.find("byteOffset")->asInt64() : 0;
                        a.componentType = aj.find("componentType") ? aj.find("componentType")->asInt64() : 0;
                        a.count = aj.find("count") ? aj.find("count")->asInt64() : 0;
                        a.type = aj.find("type") ? aj.find("type")->str : "";
                    };
                    mesh::Primitive prim;
                    prim.matrix = world;
                    if (const Json* n = m.find("name")) prim.name = n->str;
                    Accessor posA;
                    getAccessor(posIdx->asInt64(), posA);
                    std::vector<double> pos;
                    if (!ReadAccessorFloats(posA, bufferViews, bin, pos, 3, error)) { failed = true; return; }
                    for (size_t i = 0; i + 2 < pos.size(); i += 3)
                        prim.positions.push_back({float(pos[i]), float(pos[i + 1]), float(pos[i + 2])});
                    if (const Json* nrmIdx = attrs->find("NORMAL")) {
                        Accessor a; getAccessor(nrmIdx->asInt64(), a);
                        std::vector<double> nrm;
                        if (!ReadAccessorFloats(a, bufferViews, bin, nrm, 3, error)) { failed = true; return; }
                        for (size_t i = 0; i + 2 < nrm.size(); i += 3)
                            prim.normals.push_back({float(nrm[i]), float(nrm[i + 1]), float(nrm[i + 2])});
                    }
                    if (const Json* uvIdx = attrs->find("TEXCOORD_0")) {
                        Accessor a; getAccessor(uvIdx->asInt64(), a);
                        std::vector<double> uv;
                        if (!ReadAccessorFloats(a, bufferViews, bin, uv, 2, error)) { failed = true; return; }
                        for (size_t i = 0; i + 1 < uv.size(); i += 2) prim.uvs.push_back({float(uv[i]), float(uv[i + 1])});
                    }
                    if (const Json* idxIdx = p.find("indices")) {
                        Accessor a; getAccessor(idxIdx->asInt64(), a);
                        if (!ReadAccessorIndices(a, bufferViews, bin, prim.indices, error)) { failed = true; return; }
                    } else {
                        for (uint32_t i = 0; i < prim.positions.size(); ++i) prim.indices.push_back(i);
                    }
                    out.push_back(std::move(prim));
                }
        }
    }
    if (const Json* children = node.find("children"))
        for (auto& c : children->arr) WalkNode(root, nodes, meshes, accessors, bufferViews, bin, c.asInt64(), world, out, failed, error);
}

}  // namespace

bool ReadGltf(const std::vector<uint8_t>& fileBytes, bool isGlb, const std::string& binDir,
              std::vector<mesh::Primitive>& out, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    std::string jsonText;
    std::vector<uint8_t> bin;
    if (isGlb) {
        if (fileBytes.size() < 12 || std::memcmp(fileBytes.data(), "glTF", 4) != 0) return fail("not a .glb file");
        size_t o = 12;
        while (o + 8 <= fileBytes.size()) {
            uint32_t len, type;
            std::memcpy(&len, fileBytes.data() + o, 4);
            std::memcpy(&type, fileBytes.data() + o + 4, 4);
            o += 8;
            if (o + len > fileBytes.size()) return fail(".glb chunk runs past the end of the file");
            if (type == 0x4E4F534Au) jsonText.assign(reinterpret_cast<const char*>(fileBytes.data() + o), len);
            else if (type == 0x004E4942u) bin.assign(fileBytes.begin() + long(o), fileBytes.begin() + long(o + len));
            o += len;
        }
        if (jsonText.empty()) return fail(".glb has no JSON chunk");
    } else {
        jsonText.assign(reinterpret_cast<const char*>(fileBytes.data()), fileBytes.size());
    }
    Json root;
    if (!ParseJson(jsonText, root, error)) return false;
    const Json* asset = root.find("asset");
    if (!asset) return fail("not a glTF file (missing \"asset\")");
    const Json* buffers = root.find("buffers");
    const Json* bufferViews = root.find("bufferViews");
    const Json* accessors = root.find("accessors");
    const Json* meshes = root.find("meshes");
    const Json* nodes = root.find("nodes");
    if (!bufferViews || !accessors || !meshes || !nodes) return fail("glTF file has no mesh data");
    if (!isGlb) {
        if (!buffers || buffers->arr.empty()) return fail("glTF file has no buffer");
        const Json* uri = buffers->arr[0].find("uri");
        if (!uri || uri->str.compare(0, 5, "data:") == 0) return fail("only a file-referenced buffer is supported (no data: URIs)");
        std::string path = binDir.empty() ? uri->str : binDir + "/" + uri->str;
        std::ifstream f(path, std::ios::binary);
        if (!f) return fail("cannot open the glTF buffer file: " + path);
        bin.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    const Json* scenes = root.find("scenes");
    const Json* sceneIdx = root.find("scene");
    std::vector<int64_t> roots;
    if (scenes && !scenes->arr.empty()) {
        int64_t si = sceneIdx ? sceneIdx->asInt64() : 0;
        if (si < 0 || si >= int64_t(scenes->arr.size())) si = 0;
        const Json* sn = scenes->arr[size_t(si)].find("nodes");
        if (sn) for (auto& n : sn->arr) roots.push_back(n.asInt64());
    } else {
        for (size_t i = 0; i < nodes->arr.size(); ++i) roots.push_back(int64_t(i));
    }
    bool failed = false;
    for (auto r : roots) WalkNode(root, *nodes, *meshes, *accessors, *bufferViews, bin, r, mesh::Identity(), out, failed, error);
    if (failed) return false;
    if (out.empty()) return fail("no triangle mesh primitives found");
    return true;
}

Output WriteGltf(const std::vector<mesh::Primitive>& primitives, const std::string& binFileName) {
    Output res;
    std::vector<uint8_t>& bin = res.bin;
    auto align4 = [&]() { while (bin.size() % 4) bin.push_back(0); };
    auto pushFloats = [&](const std::vector<float>& v) {
        size_t start = bin.size();
        bin.resize(start + v.size() * 4);
        std::memcpy(bin.data() + start, v.data(), v.size() * 4);
        return start;
    };

    Json bufferViews = Json::Arr();
    Json accessors = Json::Arr();
    Json meshes = Json::Arr();
    Json nodes = Json::Arr();
    Json sceneNodes = Json::Arr();

    auto addBufferView = [&](size_t byteOffset, size_t byteLength) {
        Json bv = Json::Obj();
        bv.set("buffer", Json::Int(0));
        bv.set("byteOffset", Json::UInt(byteOffset));
        bv.set("byteLength", Json::UInt(byteLength));
        bufferViews.arr.push_back(std::move(bv));
        return int64_t(bufferViews.arr.size()) - 1;
    };
    auto minMax3 = [](const std::vector<mesh::Vec3>& v, float lo[3], float hi[3]) {
        lo[0] = lo[1] = lo[2] = 1e30f; hi[0] = hi[1] = hi[2] = -1e30f;
        for (auto& p : v) {
            lo[0] = std::min(lo[0], p.x); lo[1] = std::min(lo[1], p.y); lo[2] = std::min(lo[2], p.z);
            hi[0] = std::max(hi[0], p.x); hi[1] = std::max(hi[1], p.y); hi[2] = std::max(hi[2], p.z);
        }
    };

    for (size_t pi = 0; pi < primitives.size(); ++pi) {
        const mesh::Primitive& prim = primitives[pi];
        std::vector<float> posFlat;
        for (auto& p : prim.positions) { posFlat.push_back(p.x); posFlat.push_back(p.y); posFlat.push_back(p.z); }
        align4();
        size_t posOff = pushFloats(posFlat);
        int64_t posBv = addBufferView(posOff, posFlat.size() * 4);
        float lo[3], hi[3];
        minMax3(prim.positions, lo, hi);
        Json posAcc = Json::Obj();
        posAcc.set("bufferView", Json::Int(posBv));
        posAcc.set("componentType", Json::Int(5126));
        posAcc.set("count", Json::UInt(prim.positions.size()));
        posAcc.set("type", Json::Str("VEC3"));
        Json minJ = Json::Arr(), maxJ = Json::Arr();
        for (int k = 0; k < 3; ++k) { minJ.arr.push_back(Json::Num(double(lo[k]))); maxJ.arr.push_back(Json::Num(double(hi[k]))); }
        posAcc.set("min", std::move(minJ));
        posAcc.set("max", std::move(maxJ));
        int64_t posAccIdx = int64_t(accessors.arr.size());
        accessors.arr.push_back(std::move(posAcc));

        Json attrs = Json::Obj();
        attrs.set("POSITION", Json::Int(posAccIdx));

        if (!prim.normals.empty()) {
            std::vector<float> nrmFlat;
            for (auto& n : prim.normals) { nrmFlat.push_back(n.x); nrmFlat.push_back(n.y); nrmFlat.push_back(n.z); }
            align4();
            size_t off = pushFloats(nrmFlat);
            int64_t bv = addBufferView(off, nrmFlat.size() * 4);
            Json a = Json::Obj();
            a.set("bufferView", Json::Int(bv));
            a.set("componentType", Json::Int(5126));
            a.set("count", Json::UInt(prim.normals.size()));
            a.set("type", Json::Str("VEC3"));
            attrs.set("NORMAL", Json::Int(int64_t(accessors.arr.size())));
            accessors.arr.push_back(std::move(a));
        }
        if (!prim.uvs.empty()) {
            std::vector<float> uvFlat;
            for (auto& u : prim.uvs) { uvFlat.push_back(u.u); uvFlat.push_back(u.v); }
            align4();
            size_t off = pushFloats(uvFlat);
            int64_t bv = addBufferView(off, uvFlat.size() * 4);
            Json a = Json::Obj();
            a.set("bufferView", Json::Int(bv));
            a.set("componentType", Json::Int(5126));
            a.set("count", Json::UInt(prim.uvs.size()));
            a.set("type", Json::Str("VEC2"));
            attrs.set("TEXCOORD_0", Json::Int(int64_t(accessors.arr.size())));
            accessors.arr.push_back(std::move(a));
        }

        align4();
        size_t idxOff = bin.size();
        for (auto i : prim.indices) { uint32_t v = i; size_t s = bin.size(); bin.resize(s + 4); std::memcpy(bin.data() + s, &v, 4); }
        int64_t idxBv = addBufferView(idxOff, prim.indices.size() * 4);
        Json idxAcc = Json::Obj();
        idxAcc.set("bufferView", Json::Int(idxBv));
        idxAcc.set("componentType", Json::Int(5125));
        idxAcc.set("count", Json::UInt(prim.indices.size()));
        idxAcc.set("type", Json::Str("SCALAR"));
        int64_t idxAccIdx = int64_t(accessors.arr.size());
        accessors.arr.push_back(std::move(idxAcc));

        Json primJ = Json::Obj();
        primJ.set("attributes", std::move(attrs));
        primJ.set("indices", Json::Int(idxAccIdx));
        primJ.set("mode", Json::Int(4));
        Json primsArr = Json::Arr();
        primsArr.arr.push_back(std::move(primJ));
        Json meshJ = Json::Obj();
        if (!prim.name.empty()) meshJ.set("name", Json::Str(prim.name));
        meshJ.set("primitives", std::move(primsArr));
        int64_t meshIdx = int64_t(meshes.arr.size());
        meshes.arr.push_back(std::move(meshJ));

        Json nodeJ = Json::Obj();
        if (!prim.name.empty()) nodeJ.set("name", Json::Str(prim.name));
        nodeJ.set("mesh", Json::Int(meshIdx));
        if (prim.matrix != mesh::Identity()) {
            Json m = Json::Arr();
            for (float f : prim.matrix) m.arr.push_back(Json::Num(double(f)));
            nodeJ.set("matrix", std::move(m));
        }
        int64_t nodeIdx = int64_t(nodes.arr.size());
        nodes.arr.push_back(std::move(nodeJ));
        sceneNodes.arr.push_back(Json::Int(nodeIdx));
        (void)pi;
    }

    Json root = Json::Obj();
    Json asset = Json::Obj();
    asset.set("version", Json::Str("2.0"));
    asset.set("generator", Json::Str("melange xomtool"));
    root.set("asset", std::move(asset));
    Json buffers = Json::Arr();
    Json bufJ = Json::Obj();
    bufJ.set("uri", Json::Str(binFileName));
    bufJ.set("byteLength", Json::UInt(bin.size()));
    buffers.arr.push_back(std::move(bufJ));
    root.set("buffers", std::move(buffers));
    root.set("bufferViews", std::move(bufferViews));
    root.set("accessors", std::move(accessors));
    root.set("meshes", std::move(meshes));
    root.set("nodes", std::move(nodes));
    Json scene = Json::Obj();
    scene.set("nodes", std::move(sceneNodes));
    Json scenesArr = Json::Arr();
    scenesArr.arr.push_back(std::move(scene));
    root.set("scenes", std::move(scenesArr));
    root.set("scene", Json::Int(0));
    res.json = WriteJson(root);
    return res;
}

}  // namespace melange::xom::gltf
