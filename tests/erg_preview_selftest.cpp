// erg_preview_selftest - offline checks for D (asset previews). Builds synthetic mesh/texture/theme-material
// fixtures with src/xom itself (never a game asset), points erg::preview at them, and checks the conversion,
// glTF packaging and cache logic structurally.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "erg/preview.h"
#include "xom/gltf.h"
#include "xom/image.h"
#include "xom/json.h"
#include "xom/mesh.h"
#include "xom/xom.h"


using namespace melange::xom;
namespace preview = melange::erg::preview;
namespace fs = std::filesystem;

namespace {
int g_pass = 0, g_fail = 0;
void Check(bool ok, const std::string& name) {
    if (ok) { ++g_pass; std::printf("ok   - %s\n", name.c_str()); }
    else { ++g_fail; std::printf("FAIL - %s\n", name.c_str()); }
    std::fflush(stdout);
}

bool WriteAll(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (n) f.write(reinterpret_cast<const char*>(data), std::streamsize(n));
    return bool(f);
}

// A single-object document: one fresh TYPE entry plus the one object, following xomtool's own recipe for adding
// an XImage where none existed yet (tools/xomtool/main.cpp CmdConvertTextureIn).
void AddFreshType(Document& doc, const char* className) {
    const ClassDef* cd = findClass(className);
    std::array<uint8_t, 16> g{};
    auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (int i = 0; i < 16; ++i) g[size_t(i)] = uint8_t((hv(cd->guid[2 * i]) << 4) | hv(cd->guid[2 * i + 1]));
    TypeEntry t;
    t.name = className;
    t.guid = g;
    std::string padded = className;
    padded.resize(32, '\0');
    std::memcpy(t.rawName.data(), padded.data(), 32);
    doc.types.push_back(t);
}

void RecountTypes(Document& doc) {
    for (auto& t : doc.types) t.count = 0;
    for (auto& o : doc.objects)
        for (auto& t : doc.types)
            if (t.className() == o.type) ++t.count;
}

// Bundl00.xom: one mesh, ResourceId "Test.Triangle", no shader (exercises the untextured / grey-fallback path).
void BuildMeshBundle(const fs::path& bundlesDir) {
    mesh::Primitive tri;
    tri.name = "tri";
    tri.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    tri.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    tri.uvs = {{0, 0}, {1, 0}, {0, 1}};
    tri.indices = {0, 1, 2};
    mesh::Mesh m;
    m.resourceId = "Test.Triangle";
    m.primitives = {tri};

    Document doc;
    std::string err;
    const uint32_t descRef = mesh::WriteMesh(doc, m, 0, &err);
    Check(descRef != 0, "WriteMesh builds the fixture mesh: " + err);
    doc.root = descRef;
    std::vector<uint8_t> bytes;
    Check(descRef != 0 && serialize(doc, bytes, &err), "serialize the mesh fixture bundle: " + err);
    Check(WriteAll(bundlesDir / "Bundl00.xom", bytes.data(), bytes.size()), "write Bundl00.xom");
}

// Bundl01.xom: one standalone XImage, Name "Swatch.tga", a solid, distinctive colour.
void BuildImageBundle(const fs::path& bundlesDir) {
    // 4x4, not smaller: image.cpp's stride/offset formula is proven against real (>=4x4) XImages; the safety
    // tests in xom_convert_selftest.cpp use the same minimum.
    image::Pixels px;
    px.width = 4;
    px.height = 4;
    px.channels = 3;
    px.data.assign(size_t(px.width) * px.height * 3, 0);
    for (size_t i = 0; i < size_t(px.width) * px.height; ++i) {
        px.data[i * 3 + 0] = 200;
        px.data[i * 3 + 1] = 40;
        px.data[i * 3 + 2] = 40;
    }

    Document doc;
    AddFreshType(doc, "XImage");
    doc.objects.push_back(image::MakeXImage("Swatch.tga", px, /*generateMips=*/false));
    RecountTypes(doc);
    doc.root = 1;

    std::vector<uint8_t> bytes;
    std::string err;
    Check(serialize(doc, bytes, &err), "serialize the image fixture bundle: " + err);
    Check(WriteAll(bundlesDir / "Bundl01.xom", bytes.data(), bytes.size()), "write Bundl01.xom");
}

// ThemeCamelot.txt: 3 six-line records, the first line naming the surface texture - one resolves to Bundl01's "Swatch", one is NULL, one names a texture
// that does not exist anywhere in the index.
void WriteMaterialFile(const fs::path& path) {
    const char* text =
        "Swatch\nC05\nC05\nC19\nFoo/Grass\nC05\n"
        "\n"
        "NULL\nC06\nC06\nNULL\nNULL\nC05\n"
        "\n"
        "Missing1\nC07\nC07\nC19\nBar/Rock\nC07\n";
    WriteAll(path, text, std::strlen(text));
}

std::array<int, 3> AtlasCellCenter(const unsigned char* rgb, int width, int cell) {
    const int cx = cell % 8, cy = cell / 8;
    const int x = cx * 16 + 8, y = cy * 16 + 8;
    const size_t o = (size_t(y) * size_t(width) + size_t(x)) * 3;
    return {rgb[o], rgb[o + 1], rgb[o + 2]};
}

}  // namespace

int main() {
    const fs::path root = fs::temp_directory_path() / "erg_preview_selftest";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Data" / "Bundles", ec);
    fs::create_directories(root / "Data" / "Themes" / "ThemeCamelot", ec);

    BuildMeshBundle(root / "Data" / "Bundles");
    BuildImageBundle(root / "Data" / "Bundles");
    WriteMaterialFile(root / "Data" / "Themes" / "ThemeCamelot" / "ThemeCamelot.txt");
    preview::SetGameDir(root.wstring());

    // ---- DetailKey
    const std::string key = preview::DetailKey("", "Test.Triangle");
    Check(!key.empty(), "DetailKey resolves a known resource");
    Check(preview::DetailKey("", "No.Such.Resource").empty(), "DetailKey returns \"\" for an unknown resource");
    Check(key == preview::DetailKey("", "test.triangle"), "DetailKey resolves case-insensitively");

    // ---- mesh conversion
    preview::Asset asset;
    std::string err;
    Check(preview::Get(key, "glb", &asset, &err), "Get converts the mesh to glTF: " + err);
    Check(asset.contentType == "model/gltf-binary", "the mesh asset's content type is model/gltf-binary");
    Check(asset.bytes.size() > 20 && std::memcmp(asset.bytes.data(), "glTF", 4) == 0,
          "the .glb starts with the glTF magic");
    if (asset.bytes.size() > 20) {
        uint32_t jsonLen = 0;
        std::memcpy(&jsonLen, asset.bytes.data() + 12, 4);
        const std::string json(reinterpret_cast<const char*>(asset.bytes.data() + 20), jsonLen);
        Json doc;
        std::string perr;
        Check(ParseJson(json, doc, &perr), "the embedded glTF JSON parses: " + perr);
        const Json* materials = doc.find("materials");
        Check(materials && materials->arr.size() == 1, "a flat-colour material was injected");
        const Json* buffers = doc.find("buffers");
        Check(buffers && buffers->arr.size() == 1 && !buffers->arr[0].find("uri"),
              "the embedded buffer has no \"uri\" (self-contained, as a .glb needs)");
    }
    {
        std::vector<mesh::Primitive> reread;
        Check(gltf::ReadGltf(asset.bytes, /*isGlb=*/true, "", reread, &err), "gltf::ReadGltf reads the .glb back: " + err);
        Check(reread.size() == 1 && reread[0].positions.size() == 3 && reread[0].indices.size() == 3,
              "the geometry round-trips through the public glTF reader");
    }

    // ---- key/ext validation
    preview::Asset bad;
    Check(!preview::Get(key, "png", &bad, &err), "Get refuses a mesh key with ext=png");
    Check(!preview::Get("../etc/passwd", "glb", &bad, &err), "Get refuses a path-traversal-shaped key");
    Check(!preview::Get("mesh_no.such.thing", "glb", &bad, &err), "Get refuses an unresolvable mesh key");

    // ---- cache hit
    const preview::Stats s0 = preview::GetStats();
    preview::Asset asset2;
    Check(preview::Get(key, "glb", &asset2, &err) && asset2.bytes == asset.bytes,
          "a second Get returns byte-identical bytes");
    const preview::Stats s1 = preview::GetStats();
    Check(s1.hits == s0.hits + 1 && s1.conversions == s0.conversions,
          "the second Get is served from the cache (a hit, not a new conversion)");

    // ---- theme atlas
    Check(preview::ThemeAtlasKey("NOTATHEME").empty(), "ThemeAtlasKey refuses a theme outside the fixed eleven");
    const std::string atlasKey = preview::ThemeAtlasKey("CAMELOT");
    Check(!atlasKey.empty(), "ThemeAtlasKey accepts a fixed theme");

    preview::Asset atlas;
    err.clear();
    Check(preview::Get(atlasKey, "png", &atlas, &err), "Get converts the theme atlas: " + err);
    Check(atlas.contentType == "image/png", "the atlas asset's content type is image/png");
    int w = 0, h = 0, ch = 0;
    unsigned char* dec =
        stbi_load_from_memory(atlas.bytes.data(), int(atlas.bytes.size()), &w, &h, &ch, 3);
    Check(dec != nullptr && w == 128 && h == 128, "the atlas decodes to a 128x128 grid of 16px cells");
    if (dec) {
        const auto c0 = AtlasCellCenter(dec, w, 0);   // "Foo/Swatch": resolves to Bundl01's texture
        const auto c1 = AtlasCellCenter(dec, w, 1);   // NULL: no texture at all
        Check(std::abs(c0[0] - 200) < 10 && std::abs(c0[1] - 40) < 10 && std::abs(c0[2] - 40) < 10,
              "the resolved material's cell matches its source texture's colour");
        Check(!(c1[0] == 24 && c1[1] == 24 && c1[2] == 24),
              "an unresolved material still gets a distinct fallback colour, not the unused-cell background");
        stbi_image_free(dec);
    }

    std::string idxErr;
    const std::string idxJson = preview::ThemeAtlasIndex("CAMELOT", &idxErr);
    Check(!idxJson.empty(), "ThemeAtlasIndex returns JSON: " + idxErr);
    {
        Json idx;
        std::string perr;
        Check(ParseJson(idxJson, idx, &perr), "the atlas index parses: " + perr);
        const Json* materials = idx.find("materials");
        Check(materials && materials->arr.size() == 3, "the index lists all 3 fixture records");
        if (materials && materials->arr.size() == 3) {
            Check(materials->arr[0].find("resolved") && materials->arr[0].find("resolved")->boolean,
                  "record 0 (Swatch) is marked resolved");
            Check(materials->arr[1].find("resolved") && !materials->arr[1].find("resolved")->boolean,
                  "record 1 (NULL) is marked unresolved");
            Check(materials->arr[2].find("resolved") && !materials->arr[2].find("resolved")->boolean,
                  "record 2 (an unresolvable name) is marked unresolved");
        }
    }

    // ---- TrimCache is safe to call and does not evict what a large default budget should keep
    preview::TrimCache();
    preview::Asset stillCached;
    Check(preview::Get(key, "glb", &stillCached, &err),
          "the mesh cache entry survives TrimCache under the default budget");

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
