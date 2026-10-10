// melange::xom::mesh - see uvlayout.h.
#include "uvlayout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "json.h"

namespace melange::xom::mesh {
namespace {

// Distinct, saturated, readable over both dark and light textures.
const uint8_t kPalette[][3] = {{255, 64, 64}, {64, 224, 64}, {64, 160, 255}, {255, 208, 32}, {255, 64, 255}, {32, 224, 224},
                               {255, 140, 32}, {160, 96, 255}, {160, 255, 64}, {255, 96, 160}, {32, 255, 160}, {255, 255, 255}};

void Line(image::Pixels& img, int x0, int y0, int x1, int y1, const uint8_t* rgb) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (int guard = 0; guard < 1 << 16; ++guard) {
        if (x0 >= 0 && y0 >= 0 && x0 < img.width && y0 < img.height) {
            uint8_t* p = &img.data[(size_t(y0) * img.width + size_t(x0)) * 4];
            p[0] = rgb[0]; p[1] = rgb[1]; p[2] = rgb[2]; p[3] = 255;
        }
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

struct Dsu {
    std::vector<uint32_t> p;
    explicit Dsu(size_t n) : p(n) { for (size_t i = 0; i < n; ++i) p[i] = uint32_t(i); }
    uint32_t find(uint32_t x) { while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; } return x; }
    void join(uint32_t a, uint32_t b) { a = find(a); b = find(b); if (a != b) p[std::max(a, b)] = std::min(a, b); }
};

struct Island {
    size_t shape = 0;              // index into the per-image list of drawing shapes
    size_t triangles = 0;
    std::vector<uint32_t> tris;    // first index-list position of each triangle
    float u0 = 1e30f, v0 = 1e30f, u1 = -1e30f, v1 = -1e30f;
    size_t vertexCount = 0;
};

}  // namespace

bool BuildUvLayouts(const Document& doc, uint32_t descRef, const std::string& prefix, std::vector<UvLayout>& out,
                    std::string* error) {
    out.clear();
    std::vector<ShapeRef> shapes;
    if (!EnumerateShapes(doc, descRef, shapes, error)) return false;
    for (auto& ii : ListImages(doc, descRef)) {
        std::vector<Island> islands;
        std::vector<Primitive> drawn;  // the shapes whose shader reaches this image
        bool outOfRange = false;
        for (auto& s : shapes) {
            if (!s.geometry || !s.shader) continue;
            Closure sc;
            if (!CollectClosure(doc, s.shader, sc)) continue;
            if (std::find(sc.order.begin(), sc.order.end(), ii.ref) == sc.order.end()) continue;
            Primitive prim;
            if (!ReadShapePrimitive(doc, s, prim)) continue;
            if (prim.uvs.size() != prim.positions.size() || prim.uvs.empty()) continue;
            Dsu dsu(prim.uvs.size());
            const size_t triCount = prim.indices.size() / 3;
            for (size_t t = 0; t < triCount; ++t) {
                const uint32_t a = prim.indices[t * 3], b = prim.indices[t * 3 + 1], c = prim.indices[t * 3 + 2];
                if (a >= prim.uvs.size() || b >= prim.uvs.size() || c >= prim.uvs.size()) continue;
                dsu.join(a, b);
                dsu.join(a, c);
            }
            std::map<uint32_t, size_t> rootToIsland;
            const size_t shapeIdx = drawn.size();
            // An island is a connected component of the shape's vertices, so a vertex is in exactly one island: one flag per
            // vertex of the shape counts every island's vertices (a per-island array would cost islands x vertices).
            std::vector<char> counted(prim.uvs.size(), 0);
            for (size_t t = 0; t < triCount; ++t) {
                const uint32_t a = prim.indices[t * 3], b = prim.indices[t * 3 + 1], c = prim.indices[t * 3 + 2];
                if (a >= prim.uvs.size() || b >= prim.uvs.size() || c >= prim.uvs.size()) continue;
                const uint32_t root = dsu.find(a);
                auto it = rootToIsland.find(root);
                if (it == rootToIsland.end()) {
                    it = rootToIsland.emplace(root, islands.size()).first;
                    Island isl;
                    isl.shape = shapeIdx;
                    islands.push_back(std::move(isl));
                }
                Island& isl = islands[it->second];
                isl.tris.push_back(uint32_t(t * 3));
                ++isl.triangles;
                for (uint32_t vi : {a, b, c}) {
                    const Vec2 uv = prim.uvs[vi];
                    isl.u0 = std::min(isl.u0, uv.u); isl.u1 = std::max(isl.u1, uv.u);
                    isl.v0 = std::min(isl.v0, uv.v); isl.v1 = std::max(isl.v1, uv.v);
                    if (uv.u < -1e-4f || uv.u > 1.0001f || uv.v < -1e-4f || uv.v > 1.0001f) outOfRange = true;
                    if (!counted[vi]) { counted[vi] = 1; ++isl.vertexCount; }
                }
            }
            drawn.push_back(std::move(prim));
        }
        if (islands.empty()) continue;

        UvLayout lay;
        lay.imageIndex = ii.index;
        lay.imageName = ii.name;
        std::string err;
        if (!image::ExtractMip(doc.objects[ii.ref - 1], 0, lay.original, &err)) {
            if (error) *error = "image " + std::to_string(ii.index) + ": " + err;
            return false;
        }
        const int w = lay.original.width, h = lay.original.height;
        lay.scale = std::max(1, std::min(8, 512 / std::max(w, h)));
        lay.overlay.width = uint16_t(w * lay.scale);
        lay.overlay.height = uint16_t(h * lay.scale);
        lay.overlay.channels = 4;
        lay.overlay.data.resize(size_t(lay.overlay.width) * lay.overlay.height * 4);
        for (int y = 0; y < lay.overlay.height; ++y)
            for (int x = 0; x < lay.overlay.width; ++x) {
                const uint8_t* s = &lay.original.data[(size_t(y / lay.scale) * size_t(w) + size_t(x / lay.scale)) * size_t(lay.original.channels)];
                uint8_t* d = &lay.overlay.data[(size_t(y) * lay.overlay.width + size_t(x)) * 4];
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = lay.original.channels == 4 ? s[3] : 255;
            }

        // V is up (the XImage is stored bottom-up and ExtractMip flips it), so the PNG row is (1 - v) * height.
        Json islandList = Json::Arr();
        size_t id = 0;
        for (auto& is : islands) {
            const Primitive& prim = drawn[is.shape];
            const uint8_t* col = kPalette[id % (sizeof(kPalette) / sizeof(kPalette[0]))];
            auto px = [&](float u) { return int(std::floor(u * float(lay.overlay.width))); };
            auto py = [&](float v) { return int(std::floor((1.0f - v) * float(lay.overlay.height))); };
            for (uint32_t t : is.tris) {
                const Vec2 a = prim.uvs[prim.indices[t]], b = prim.uvs[prim.indices[t + 1]], c = prim.uvs[prim.indices[t + 2]];
                Line(lay.overlay, px(a.u), py(a.v), px(b.u), py(b.v), col);
                Line(lay.overlay, px(b.u), py(b.v), px(c.u), py(c.v), col);
                Line(lay.overlay, px(c.u), py(c.v), px(a.u), py(a.v), col);
            }
            Json j = Json::Obj();
            j.set("id", Json::UInt(id));
            j.set("shape", Json::Str(prim.name));
            j.set("triangles", Json::UInt(is.triangles));
            j.set("vertices", Json::UInt(is.vertexCount));
            Json uv = Json::Arr();
            for (float f : {is.u0, is.v0, is.u1, is.v1}) uv.arr.push_back(Json::Num(double(f)));
            j.set("uvBounds", std::move(uv));
            // Native-resolution pixel box in PNG row order: x0, y0 (top), x1, y1 (bottom).
            Json pb = Json::Arr();
            for (double f : {std::floor(double(is.u0) * w), std::floor((1.0 - double(is.v1)) * h), std::ceil(double(is.u1) * w),
                             std::ceil((1.0 - double(is.v0)) * h)})
                pb.arr.push_back(Json::Num(f));
            j.set("pixelBounds", std::move(pb));
            char hex[16];
            std::snprintf(hex, sizeof(hex), "#%02x%02x%02x", col[0], col[1], col[2]);
            j.set("color", Json::Str(hex));
            islandList.arr.push_back(std::move(j));
            lay.triangles += is.triangles;
            ++id;
        }
        lay.islands = id;
        lay.originalFile = prefix + std::to_string(ii.index) + ".original.png";
        lay.overlayFile = prefix + std::to_string(ii.index) + ".layout.png";

        Json root = Json::Obj();
        root.set("format", Json::Str("melange-uvlayout/1"));
        Json img = Json::Obj();
        img.set("index", Json::Int(ii.index));
        img.set("name", Json::Str(ii.name));
        img.set("width", Json::UInt(ii.width));
        img.set("height", Json::UInt(ii.height));
        img.set("mipLevels", Json::UInt(ii.mips));
        img.set("format", Json::Str(ii.format == 1 ? "RGBA8" : "RGB8"));
        root.set("image", std::move(img));
        root.set("originalPng", Json::Str(lay.originalFile));
        root.set("layoutPng", Json::Str(lay.overlayFile));
        root.set("layoutScale", Json::Int(lay.scale));
        root.set("convention", Json::Str("u to the right, v up: native pixel x = u * width, PNG row y = (1 - v) * height. The layout PNG is the original enlarged layoutScale times (nearest) with the island outlines drawn on it."));
        root.set("outOfRangeUvs", Json::Bool(outOfRange));
        root.set("replaceWith", Json::Str("xomtool clone ... --texture " + std::to_string(ii.index) + "=<painted.png>"));
        root.set("islands", std::move(islandList));
        lay.json = WriteJson(root);
        out.push_back(std::move(lay));
    }
    return true;
}

}  // namespace melange::xom::mesh
