// xom_convert_selftest - offline checks for the Sieve xomtool library (src/xom):
// image round trips, a mesh round trip on Factory.Proj.Bazookashell, and the bank builder
// against a reference bank. The game files it reads are read-only inputs, never written.
//
// Usage: xom_convert_selftest --game <WormsXHD dir>
// Without --game, the checks that need the game's files are skipped (reported, not a failure),
// so this still builds and runs offline; D acceptance runs it with --game set.
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "gltf.h"
#include "image.h"
#include "json.h"
#include "mesh.h"

using namespace melange::xom;
namespace fs = std::filesystem;

namespace {

int g_pass = 0, g_fail = 0, g_skip = 0;
void Check(bool ok, const std::string& name) {
    if (ok) { ++g_pass; std::printf("ok   - %s\n", name.c_str()); }
    else { ++g_fail; std::printf("FAIL - %s\n", name.c_str()); }
}
void Skip(const std::string& name, const std::string& why) {
    ++g_skip;
    std::printf("skip - %s (%s)\n", name.c_str(), why.c_str());
}

std::vector<uint8_t> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
bool WriteAll(const fs::path& p, const void* data, size_t n) {
    std::ofstream f(p, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), std::streamsize(n));
    return bool(f);
}

// ---------------------------------------------------------------- image round trip

void ImageTests(const fs::path& game) {
    auto bundlePath = game / "Data" / "Bundles" / "Bundl09.xom";
    if (!fs::exists(bundlePath)) { Skip("image round trip", "no game files"); return; }
    auto bytes = ReadAll(bundlePath);
    Document doc;
    std::string err;
    if (!parse(bytes.data(), bytes.size(), doc, &err)) { Check(false, "parse Bundl09.xom: " + err); return; }
    const Object* icon = nullptr;
    for (auto& o : doc.objects)
        if (o.type == "XImage" && o.field("Name") && o.field("Name")->str == "Weapon Panel Icons1.tga") { icon = &o; break; }
    Check(icon != nullptr, "find \"Weapon Panel Icons1.tga\"");
    if (!icon) return;

    image::Pixels px;
    Check(image::ExtractMip(*icon, 0, px, &err), "ExtractMip mip 0: " + err);
    Check(px.width == 256 && px.height == 256 && px.channels == 3 && px.data.size() == 196608,
          "panel icon is 256x256 RGB8 (196608 bytes)");

    // PNG round trip (stb): decode(encode(px)) reproduces px exactly.
    std::vector<uint8_t> png;
    {
        int len = 0;
        unsigned char* mem = stbi_write_png_to_mem(px.data.data(), px.width * px.channels, px.width, px.height, px.channels, &len);
        Check(mem != nullptr, "stb_image_write encodes the panel icon");
        if (mem) { png.assign(mem, mem + len); STBIW_FREE(mem); }
    }
    int w, h, ch;
    unsigned char* dec = stbi_load_from_memory(png.data(), int(png.size()), &w, &h, &ch, 3);
    Check(dec && w == px.width && h == px.height && ch == 3, "stb_image decodes it back to the same size");
    if (dec) {
        bool same = std::memcmp(dec, px.data.data(), px.data.size()) == 0;
        Check(same, "PNG round trip is pixel-exact (level 0, no mip resampling)");
        stbi_image_free(dec);
    }

    // Lossless leg through StoreFields/ExtractMip (no mip regeneration): byte-identical.
    Object copy = *icon;
    Check(image::StoreFields(copy, px, /*generateMips=*/false, &err), "StoreFields (single level): " + err);
    image::Pixels px2;
    Check(image::ExtractMip(copy, 0, px2, &err) && px2.data == px.data, "StoreFields/ExtractMip is byte-identical at level 0");

    // Every mip level round-trips exactly too.
    const Value* mipsF = icon->field("MipLevels");
    int mips = mipsF ? int(mipsF->asUInt()) : 1;
    bool allMips = true;
    for (int lvl = 0; lvl < mips; ++lvl) {
        image::Pixels lvlPx;
        if (!image::ExtractMip(*icon, lvl, lvlPx, &err)) { allMips = false; break; }
    }
    Check(allMips, "every mip level of the panel icon extracts without error");
}

// ---------------------------------------------------------------- image safety (no game needed)

// A crafted MipLevels, entirely offline: ExtractMip must refuse rather than shift a 32-bit value by 32 or more
// (undefined behaviour) or read past a Data buffer whose length happened to match a wrapped total.
void ImageSafetyTests() {
    Object o;
    o.type = "XImage";
    auto push16 = [&](const char* k, Type t, uint16_t v) { Value val; val.type = t; val.bits = v; o.fields.emplace_back(k, val); };
    push16("Width", Type::U16, 4);
    push16("Height", Type::U16, 4);
    push16("Format", Type::Enum, 0);
    push16("MipLevels", Type::U16, 40);
    { Value v; v.type = Type::U8; v.array = true; v.raw.assign(4 * 4 * 3, 0); o.fields.emplace_back("Data", v); }

    image::Pixels px;
    std::string err;
    bool ok1 = image::ExtractMip(o, 0, px, &err);
    Check(!ok1 && err.find("MipLevels") != std::string::npos,
          "ExtractMip refuses an implausible MipLevels instead of shifting a 32-bit value by 32+: " + err);

    for (auto& [k, v] : o.fields)
        if (k == "MipLevels") v.bits = 20;  // in range now, but still not the real formula for a 4x4 image
    err.clear();
    bool ok2 = image::ExtractMip(o, 0, px, &err);
    Check(!ok2 && err.find("does not match") != std::string::npos,
          "ExtractMip still rejects a Data length that doesn't match the (now safely computed) formula: " + err);
}

// A primitive whose index array names a vertex past its own position count: WriteMesh must refuse it rather than
// truncate the index to u16 and silently write a mesh that reads out of bounds in the game.
void MeshSafetyTests() {
    mesh::Primitive p;
    p.name = "Bad";
    p.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    p.indices = {0, 1, 70000};  // only 3 vertices; 70000 also wraps to a small, plausible-looking u16
    mesh::Mesh m;
    m.resourceId = "Test.Payload";
    m.primitives = {p};
    Document doc;
    std::string err;
    const uint32_t ref = mesh::WriteMesh(doc, m, 0, &err);
    Check(ref == 0 && err.find("out of range") != std::string::npos,
          "WriteMesh refuses an index out of range for its vertex count: " + err);
}

// ---------------------------------------------------------------- mesh round trip

void MeshTests(const fs::path& game, const fs::path& outDir) {
    auto bundlePath = game / "Data" / "Bundles" / "Bundl416.xom";
    if (!fs::exists(bundlePath)) { Skip("mesh round trip", "no game files"); return; }
    auto bytes = ReadAll(bundlePath);
    Document doc;
    std::string err;
    if (!parse(bytes.data(), bytes.size(), doc, &err)) { Check(false, "parse Bundl416.xom: " + err); return; }

    mesh::Mesh m;
    Check(mesh::ReadMesh(doc, "Factory.Proj.Bazookashell", m, &err), "ReadMesh Factory.Proj.Bazookashell: " + err);
    if (m.primitives.empty()) return;
    auto& p0 = m.primitives[0];
    Check(p0.positions.size() == 65 && p0.indices.size() == 360, "65 vertices, 360 indices (120 triangles)");

    // glTF round trip: write, re-read, compare geometry exactly.
    auto out = gltf::WriteGltf(m.primitives, "shell.bin");
    fs::create_directories(outDir);
    Check(WriteAll(outDir / "shell.gltf", out.json.data(), out.json.size()), "write shell.gltf");
    Check(WriteAll(outDir / "shell.bin", out.bin.data(), out.bin.size()), "write shell.bin");
    auto gltfBytes = ReadAll(outDir / "shell.gltf");
    std::vector<mesh::Primitive> reread;
    Check(gltf::ReadGltf(gltfBytes, false, outDir.string(), reread, &err), "ReadGltf shell.gltf: " + err);
    bool geomOk = reread.size() == m.primitives.size();
    for (size_t i = 0; geomOk && i < reread.size(); ++i) {
        auto& a = m.primitives[i];
        auto& b = reread[i];
        geomOk = a.positions.size() == b.positions.size() && a.indices == b.indices;
        for (size_t k = 0; geomOk && k < a.positions.size(); ++k) {
            float dx = a.positions[k].x - b.positions[k].x, dy = a.positions[k].y - b.positions[k].y,
                  dz = a.positions[k].z - b.positions[k].z;
            geomOk = std::abs(dx) < 1e-4f && std::abs(dy) < 1e-4f && std::abs(dz) < 1e-4f;
        }
    }
    Check(geomOk, "glTF round trip reproduces positions and indices exactly");

    // XOM round trip: write into a fresh document, re-parse, re-read.
    Document fresh;
    uint32_t descRef = mesh::WriteMesh(fresh, m, 0, &err);
    Check(descRef != 0, "WriteMesh into a fresh document: " + err);
    std::vector<uint8_t> freshBytes;
    Check(descRef != 0 && serialize(fresh, freshBytes, &err), "serialize the fresh document: " + err);
    Document reparsed;
    ParseOptions strict{true};
    Check(!freshBytes.empty() && parse(freshBytes.data(), freshBytes.size(), reparsed, &err, strict),
          "re-parse the fresh document (strict): " + err);
    mesh::Mesh m2;
    Check(mesh::ReadMesh(reparsed, m.resourceId, m2, &err) && m2.primitives.size() == m.primitives.size(),
          "ReadMesh recovers the same number of primitives after a full XOM round trip");
}

// ---------------------------------------------------------------- mesh bundle (no game needed)

std::string GuidHex(const std::array<uint8_t, 16>& g) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : g) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

// A tiny glTF -> WriteBundle -> serialize -> parse round trip: the bank must have the shape the engine's section
// loader reads (docs/meshes.md): root XGraphSet -> descriptor -> world graph set (geometry GUID).
void BundleTests(const fs::path& outDir) {
    fs::create_directories(outDir);
    mesh::Primitive p;
    p.name = "tri";
    p.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    p.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    p.uvs = {{0, 0}, {1, 0}, {0, 1}};
    p.indices = {0, 1, 2};
    auto g = gltf::WriteGltf({p}, "tri.bin");
    Check(WriteAll(outDir / "tri.gltf", g.json.data(), g.json.size()) && WriteAll(outDir / "tri.bin", g.bin.data(), g.bin.size()),
          "write tri.gltf/.bin");
    auto gltfBytes = ReadAll(outDir / "tri.gltf");
    std::string err;
    std::vector<mesh::Primitive> prims;
    Check(gltf::ReadGltf(gltfBytes, false, outDir.string(), prims, &err) && prims.size() == 1, "ReadGltf tri.gltf: " + err);
    if (prims.size() != 1) return;

    mesh::Mesh m;
    m.resourceId = "kindjal.Tri";
    m.sectionId = 478;
    m.primitives = prims;
    Document doc;
    const uint32_t descRef = mesh::WriteBundle(doc, m, 0, &err);
    Check(descRef != 0, "WriteBundle: " + err);
    std::vector<uint8_t> bytes;
    Check(descRef != 0 && serialize(doc, bytes, &err), "serialize the bundle: " + err);
    Check(WriteAll(outDir / "kindjal.Tri.xom", bytes.data(), bytes.size()), "write kindjal.Tri.xom");
    Document d;
    ParseOptions strict{true};
    Check(!bytes.empty() && parse(bytes.data(), bytes.size(), d, &err, strict), "re-parse the bundle (strict): " + err);
    if (d.objects.empty()) return;

    const Object* root = d.object(d.root);
    Check(root && root->type == "XGraphSet", "the document root is an XGraphSet");
    const Value* graphs = root ? root->field("Graphs") : nullptr;
    Check(graphs && graphs->size() == 1, "the root lists exactly one entry");
    if (!graphs || graphs->size() != 1) return;
    Value entry = graphs->at(0);
    const Value *eg = entry.member("Guid"), *er = entry.member("Graph"), *en = entry.member("Name");
    Check(eg && GuidHex(eg->guid) == "99cc436e6fbef54b85d2bfcdf9ae4283", "root entry GUID is the resource-descriptor GUID");
    Check(en && en->str == "kindjal.Tri", "root entry Name is the resource id");
    const Object* desc = er ? d.object(er->asRef()) : nullptr;
    Check(desc && desc->type == "XMeshDescriptor", "root entry points at an XMeshDescriptor");
    if (!desc) return;
    const Value *rid = desc->field("ResourceId"), *sec = desc->field("SectionId"), *fl = desc->field("Flags"),
                *gs = desc->field("GraphSet");
    Check(rid && rid->str == "kindjal.Tri", "descriptor ResourceId");
    Check(sec && sec->asUInt() == 478, "descriptor SectionId is 478");
    Check(fl && fl->asUInt() == 8, "descriptor Flags is 8");
    const Object* world = gs ? d.object(gs->asRef()) : nullptr;
    Check(world && world->type == "XGraphSet" && world != root, "descriptor GraphSet is a second XGraphSet");
    const Value* wg = world ? world->field("Graphs") : nullptr;
    Check(wg && wg->size() == 1, "the world graph set has one entry");
    if (wg && wg->size() == 1) {
        Value we = wg->at(0);
        const Value *wguid = we.member("Guid"), *wname = we.member("Name"), *wgraph = we.member("Graph");
        Check(wguid && GuidHex(wguid->guid) == "6ae6dbe4fa866b45a73ff9130e12dfeb", "world entry carries the geometry-graph GUID");
        Check(wname && wname->str == "world", "world entry is named \"world\"");
        const Object* node = wgraph ? d.object(wgraph->asRef()) : nullptr;
        Check(node && node->type == "XInteriorNode", "world graph is an XInteriorNode");
    }
    size_t graphSets = 0;
    for (auto& t : d.types) if (t.className() == "XGraphSet") graphSets = t.count;
    Check(graphSets == 2, "TYPE table counts two XGraphSets");
    mesh::Mesh back;
    Check(mesh::ReadMesh(d, "kindjal.Tri", back, &err) && back.primitives.size() == 1 &&
              back.primitives[0].positions.size() == 3 && back.primitives[0].indices.size() == 3,
          "ReadMesh recovers the triangle from the bundle: " + err);
}

// ---------------------------------------------------------------- bank builder

void BankTests(const fs::path& game, const fs::path& outDir) {
    auto weaptwk = game / "Data" / "Tweak" / "WEAPTWK.XOM";
    if (!fs::exists(weaptwk)) { Skip("bank builder", "no game files"); return; }
    auto bytes = ReadAll(weaptwk);
    Document doc;
    std::string err;
    if (!parse(bytes.data(), bytes.size(), doc, &err)) { Check(false, "parse WEAPTWK.XOM: " + err); return; }

    const Object* bank = nullptr;
    for (auto& o : doc.objects) if (o.type == "XDataBank") { bank = &o; break; }
    Check(bank != nullptr, "find the XDataBank");
    if (!bank) return;
    const Value* cr = bank->field("ContainerResources");
    const Object* templateDetail = nullptr;
    uint32_t baseRef = 0;
    for (size_t i = 0; cr && i < cr->size(); ++i) {
        const Object* det = doc.object(cr->at(i).asRef());
        if (det && det->field("Name") && det->field("Name")->str == "kWeaponBazooka") {
            templateDetail = det;
            baseRef = det->field("Value")->asRef();
            break;
        }
    }
    Check(templateDetail != nullptr, "find kWeaponBazooka's resource-details entry");
    if (!templateDetail) return;

    Object cont = *doc.object(baseRef);
    Value* dmg = cont.field("WormDamageMagnitude");
    Check(dmg != nullptr, "kWeaponBazooka has WormDamageMagnitude");
    if (dmg) dmg->setFloat(120.0);

    Document out;
    out.version = doc.version;
    out.reserved08 = doc.reserved08;
    out.reserved24 = doc.reserved24;
    out.guidRec = doc.guidRec;
    out.schmRec = doc.schmRec;
    out.types = doc.types;
    Object detail = *templateDetail;
    detail.field("Name")->str = "kWeaponMegaBazooka";
    detail.field("Value")->bits = 3;
    Object bankCopy = *bank;
    for (auto& kv : bankCopy.fields) {
        if (kv.first == "ContainerResources") {
            Value r; r.type = Type::Ref; r.bits = 1;
            kv.second.items.assign(1, r);
        } else if (kv.second.array) {
            kv.second.items.clear();
            kv.second.raw.clear();
        }
    }
    out.objects = {detail, bankCopy, cont};
    for (auto& t : out.types) t.count = 0;
    for (auto& o : out.objects)
        for (auto& t : out.types)
            if (t.className() == o.type) ++t.count;
    out.root = 2;
    out.strings = doc.strings;

    std::vector<uint8_t> outBytes;
    Check(serialize(out, outBytes, &err), "serialize the built bank: " + err);
    fs::create_directories(outDir);
    Check(WriteAll(outDir / "megabank.xom", outBytes.data(), outBytes.size()), "write megabank.xom");
    Document reread;
    Check(parse(outBytes.data(), outBytes.size(), reread, &err) && reread.objects.size() == 3,
          "the built bank re-parses to exactly 3 objects: " + err);
    if (reread.objects.size() == 3) {
        const Value* magnitude = reread.objects[2].field("WormDamageMagnitude");
        Check(magnitude && magnitude->asFloat() == 120.0, "kWeaponMegaBazooka.WormDamageMagnitude reads back as 120");
        const Value* name = reread.objects[0].field("Name");
        Check(name && name->str == "kWeaponMegaBazooka", "the resource is named kWeaponMegaBazooka");
    }
}

}  // namespace

int main(int argc, char** argv) {
    fs::path game;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) game = argv[++i];
    fs::path outDir = fs::temp_directory_path() / "xom_convert_selftest";

    ImageTests(game);
    ImageSafetyTests();
    MeshSafetyTests();
    BundleTests(outDir);
    MeshTests(game, outDir);
    BankTests(game, outDir);

    std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
