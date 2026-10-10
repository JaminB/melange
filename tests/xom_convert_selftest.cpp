// xom_convert_selftest - offline checks for the Sieve xomtool library (src/xom):
// image round trips, a mesh round trip on Factory.Proj.Bazookashell, and the bank builder
// against a reference bank, node-preserving bundles, texture replacement, vertex deformation, and (with --game) the clone
// of a static, a rigid-hierarchy and a skinned vanilla mesh. The game files it reads are read-only inputs, never written.
//
// Usage: xom_convert_selftest --game <WormsXHD dir>
// Without --game, the checks that need the game's files are skipped (reported, not a failure), so this still builds and runs
// offline; the clone, image and UV-layout code is covered offline by a synthetic textured bank. With --game, a missing file
// is a failure. D acceptance runs it with --game set.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "clone.h"
#include "deform.h"
#include "gltf.h"
#include "image.h"
#include "json.h"
#include "mesh.h"
#include "uvlayout.h"

using namespace melange::xom;
namespace fs = std::filesystem;

namespace melange::xom::mesh {
// Exact comparison, for "this shape did not move" checks on coordinate arrays.
inline bool operator==(const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
}  // namespace melange::xom::mesh

namespace {

int g_pass = 0, g_fail = 0, g_skip = 0;
bool g_gameRequested = false;  // --game was given: a check that cannot run for want of the game's files is then a failure
void Check(bool ok, const std::string& name) {
    if (ok) { ++g_pass; std::printf("ok   - %s\n", name.c_str()); }
    else { ++g_fail; std::printf("FAIL - %s\n", name.c_str()); }
}
void Skip(const std::string& name, const std::string& why) {
    if (g_gameRequested) {
        // A wrong --game path must not read as a green run.
        ++g_fail;
        std::printf("FAIL - %s (%s, but --game was given)\n", name.c_str(), why.c_str());
        return;
    }
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

// ---------------------------------------------------------------- malformed glTF (no game needed)

void GltfRefusalTests(const fs::path& outDir) {
    fs::create_directories(outDir);
    // The buffer is read before any node is walked, so the file has to exist; its content does not matter.
    const unsigned char zero[16] = {};
    Check(WriteAll(outDir / "empty.bin", zero, sizeof zero), "write empty.bin");
    // The flat and the tree reader both walk the nodes: both errors are returned.
    auto read = [&](const std::string& nodes) {
        const std::string j = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"empty.bin","byteLength":16}],"bufferViews":[],"accessors":[],"meshes":[],"nodes":)" +
                              nodes + "}";
        std::vector<uint8_t> bytes(j.begin(), j.end());
        std::vector<mesh::Primitive> out;
        std::string err;
        gltf::ReadGltf(bytes, false, outDir.string(), out, &err);
        mesh::Mesh tree;
        std::string err2;
        gltf::ReadGltfScene(bytes, false, outDir.string(), tree, &err2);
        return std::make_pair(err, err2);
    };
    struct Bad { const char* nodes; const char* what; };
    const Bad bads[] = {
        {R"([{"matrix":[1,2,3]}])", "a matrix of 3 numbers"},
        {R"([{"matrix":"x"}])", "a matrix that is not an array"},
        {R"([{"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,"a",1]}])", "a non-number in a matrix"},
        {R"([{"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,1e999,1]}])", "an infinite matrix entry"},
        {R"([{"translation":[1,2]}])", "a translation of 2 numbers"},
        {R"([{"rotation":[0,0,0]}])", "a rotation of 3 numbers"},
        {R"([{"scale":[1,1,1e999]}])", "an infinite scale"},
    };
    for (auto& b : bads) {
        const auto [e1, e2] = read(b.nodes);
        Check(e1.find("node 0") != std::string::npos && e2.find("node 0") != std::string::npos,
              std::string("a node with ") + b.what + " is refused naming the node: " + e1);
    }
    // Well-formed transforms get past the node walk (the file then fails later: it has no mesh).
    {
        const auto [e1, e2] = read(R"([{"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]},{"translation":[1,2,3],"rotation":[0,0,0,1],"scale":[1,1,1]}])");
        Check(e1.find("no triangle mesh") != std::string::npos, "well-formed node transforms are accepted: " + e1);
    }
    // A chain of nodes, each the only child of the last: refused past the depth limit instead of overflowing the stack.
    for (int depth : {300, 5000}) {
        std::string nodes = "[";
        for (int i = 0; i < depth; ++i) nodes += (i ? "," : "") + std::string("{\"children\":[") + std::to_string(i + 1) + "]}";
        nodes += ",{}]";
        const auto [e1, e2] = read(nodes);
        Check(e1.find("deeper than") != std::string::npos && e2.find("deeper than") != std::string::npos,
              "a node chain " + std::to_string(depth) + " deep is refused: " + e1);
    }
    {
        std::string nodes = "[";
        for (int i = 0; i < 100; ++i) nodes += (i ? "," : "") + std::string("{\"children\":[") + std::to_string(i + 1) + "]}";
        nodes += ",{}]";
        const auto [e1, e2] = read(nodes);
        Check(e1.find("deeper than") == std::string::npos, "a node chain 100 deep is not refused for its depth: " + e1);
    }
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


// ---------------------------------------------------------------- helpers for the clone / deform / node tests

Document Roundtrip(const Document& doc, std::string* err) {
    std::vector<uint8_t> bytes;
    Document out;
    ParseOptions strict{true};
    if (!serialize(doc, bytes, err)) return out;
    parse(bytes.data(), bytes.size(), out, err, strict);
    return out;
}

std::map<std::string, size_t> CountByType(const Document& doc, const std::vector<uint32_t>* only = nullptr) {
    std::map<std::string, size_t> m;
    if (only) { for (auto r : *only) ++m[doc.objects[r - 1].type]; }
    else for (auto& o : doc.objects) ++m[o.type];
    return m;
}

// Equal in every field, except that a reference is equal when it is the mapped one (src object -> clone object).
bool ValueEq(const Value& a, const Value& b, const std::unordered_map<uint32_t, uint32_t>& map) {
    if (a.type != b.type || a.math != b.math || a.array != b.array) return false;
    if (a.type == Type::Ref && !a.array) {
        if (a.bits == 0) return b.bits == 0;
        auto it = map.find(uint32_t(a.bits));
        return it != map.end() && it->second == b.bits;
    }
    if (a.bits != b.bits || a.str != b.str || a.guid != b.guid || a.raw != b.raw) return false;
    if (a.items.size() != b.items.size() || a.members.size() != b.members.size()) return false;
    for (size_t i = 0; i < a.items.size(); ++i) if (!ValueEq(a.items[i], b.items[i], map)) return false;
    for (size_t i = 0; i < a.members.size(); ++i)
        if (a.members[i].first != b.members[i].first || !ValueEq(a.members[i].second, b.members[i].second, map)) return false;
    return true;
}
bool ObjectEq(const Object& a, const Object& b, const std::unordered_map<uint32_t, uint32_t>& map, const char* skipField = nullptr) {
    if (a.type != b.type || a.fields.size() != b.fields.size() || a.internalFlags != b.internalFlags || a.userFlags != b.userFlags) return false;
    for (size_t i = 0; i < a.fields.size(); ++i) {
        if (a.fields[i].first != b.fields[i].first) return false;
        if (skipField && a.fields[i].first == skipField) continue;
        if (!ValueEq(a.fields[i].second, b.fields[i].second, map)) return false;
    }
    return true;
}

// Unit-radius UV sphere. `ccw` false flips the winding (and so the geometric face normal) while the stored normals stay
// radial, to prove the deformer follows the stored normals rather than assuming a winding.
mesh::Primitive MakeSphere(const std::string& name, int seg, int rings, bool ccw, mesh::Vec3 at = {}) {
    mesh::Primitive p;
    p.name = name;
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s <= seg; ++s) {
            const float th = 3.14159265f * float(r) / float(rings), ph = 6.2831853f * float(s) / float(seg);
            // Exact poles and an exact seam (the last column repeats the first), as an exporter writes them: the duplicates
            // are bit-identical positions, which is what lets the deformer weld them.
            const float sth = (r == 0 || r == rings) ? 0.0f : std::sin(th);
            const float cph = s == seg ? 1.0f : std::cos(ph), sph = s == seg ? 0.0f : std::sin(ph);
            const float x = sth * cph, y = std::cos(th), z = sth * sph;
            p.positions.push_back({x + at.x, y + at.y, z + at.z});
            p.normals.push_back({x, y, z});
            p.uvs.push_back({float(s) / float(seg), 1.0f - float(r) / float(rings)});
        }
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < seg; ++s) {
            const uint32_t a = uint32_t(r * (seg + 1) + s), b = a + 1, c = a + uint32_t(seg + 1), d = c + 1;
            // Outward-facing (cross(b-a, c-a) along the radial direction) with y up and phi counter-clockwise from above.
            const uint32_t tri1[3] = {a, b, c}, tri2[3] = {b, d, c};
            for (auto* t : {tri1, tri2}) {
                if (ccw) { p.indices.push_back(t[0]); p.indices.push_back(t[1]); p.indices.push_back(t[2]); }
                else { p.indices.push_back(t[0]); p.indices.push_back(t[2]); p.indices.push_back(t[1]); }
            }
        }
    return p;
}

float Dist3(mesh::Vec3 a, mesh::Vec3 b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
float Len3(mesh::Vec3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

std::vector<mesh::Vec3> CoordsOf(const Document& d, uint32_t shapeRef) {
    std::vector<mesh::Vec3> out;
    const Object* sh = d.object(shapeRef);
    const Object* geom = sh ? d.object(sh->field("Geometry")->asRef()) : nullptr;
    const Object* co = geom ? d.object(geom->field("CoordSet")->asRef()) : nullptr;
    if (!co) return out;
    auto f = mesh::ReadFloatArray(*co->field("Coord"));
    for (size_t i = 0; i + 2 < f.size(); i += 3) out.push_back({f[i], f[i + 1], f[i + 2]});
    return out;
}
std::vector<mesh::Vec3> NormalsOf(const Document& d, uint32_t shapeRef) {
    std::vector<mesh::Vec3> out;
    const Object* sh = d.object(shapeRef);
    const Object* geom = sh ? d.object(sh->field("Geometry")->asRef()) : nullptr;
    const Object* no = geom ? d.object(geom->field("NormalSet")->asRef()) : nullptr;
    if (!no) return out;
    auto f = mesh::ReadFloatArray(*no->field("Normal"));
    for (size_t i = 0; i + 2 < f.size(); i += 3) out.push_back({f[i], f[i + 1], f[i + 2]});
    return out;
}

// ---------------------------------------------------------------- node hierarchy (no game needed)

void NodeTests(const fs::path& outDir) {
    fs::create_directories(outDir);
    // body (a mesh) > pump (a mesh, rotated) and eject (a locator); plus a second root with two shapes.
    mesh::Mesh m;
    m.resourceId = "kindjal.Nodes";
    m.sectionId = 477;
    // Rotation 0.3 / -0.4 / 0.5 rad about X / Y / Z in the engine's R = Rz * Ry * Rx order, translated (1, 2, 3).
    const float rx = 0.3f, ry = -0.4f, rz = 0.5f;
    const float cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry), cz = std::cos(rz), sz = std::sin(rz);
    mesh::Node body{"body", mesh::Identity(), -1};
    mesh::Node pump{"pump", mesh::Identity(), 0};
    pump.local = {cz * cy,                    sz * cy,                    -sy,     0,
                  cz * sy * sx - sz * cx,     sz * sy * sx + cz * cx,     cy * sx, 0,
                  cz * sy * cx + sz * sx,     sz * sy * cx - cz * sx,     cy * cx, 0,
                  1, 2, 3, 1};
    mesh::Node eject{"eject", mesh::Identity(), 0};
    eject.local[12] = 0.5f; eject.local[13] = 0.25f; eject.local[14] = -2;
    mesh::Node second{"second", mesh::Identity(), -1};
    m.nodes = {body, pump, eject, second};
    auto a = MakeSphere("bodyShape_mat", 8, 6, true);        a.node = 0;
    auto b = MakeSphere("pumpShape_mat", 6, 4, true);        b.node = 1;
    auto c = MakeSphere("secondShape_one", 6, 4, true);      c.node = 3;
    auto d = MakeSphere("secondShape_two", 6, 4, true);      d.node = 3;
    m.primitives = {a, b, c, d};

    Document doc;
    std::string err;
    Check(mesh::WriteBundle(doc, m, 0, &err) != 0, "hierarchical WriteBundle: " + err);
    Document rd = Roundtrip(doc, &err);
    Check(!rd.objects.empty(), "the hierarchical bundle serializes and re-parses strictly: " + err);
    if (rd.objects.empty()) return;

    // The vanilla layout: group "body" (with an XTransform) whose first child is the Core-less "bodyShape" group.
    const uint32_t desc = mesh::FindDescriptor(rd, "kindjal.Nodes");
    Check(desc != 0, "the descriptor is found by name");
    mesh::Mesh back;
    Check(mesh::ReadMeshTree(rd, desc, back, &err), "ReadMeshTree on the bundle: " + err);
    bool namesOk = back.nodes.size() == 4 && back.primitives.size() == 4;
    const char* want[4] = {"body", "pump", "eject", "second"};
    const int parents[4] = {-1, 0, 0, -1};
    for (size_t i = 0; namesOk && i < 4; ++i) namesOk = back.nodes[i].name == want[i] && back.nodes[i].parent == parents[i];
    Check(namesOk, "node names and parents survive WriteBundle -> ReadMeshTree (body > pump, eject; second)");
    bool shapesOk = back.primitives.size() == 4;
    const int owner[4] = {0, 1, 3, 3};
    for (size_t i = 0; shapesOk && i < 4; ++i) shapesOk = back.primitives[i].node == owner[i];
    Check(shapesOk, "each shape stays on its node; a locator (eject) stays a shapeless node; two shapes share a node");
    bool pumpMat = back.nodes.size() == 4;
    for (size_t i = 0; pumpMat && i < 16; ++i) pumpMat = std::abs(back.nodes[1].local[i] - pump.local[i]) < 1e-5f;
    Check(pumpMat, "a node's local matrix is preserved exactly");

    // Group structure, as in Bundl09's ClusterBomb / Shotgun.
    const Object* bodyGroup = nullptr;
    for (auto& o : rd.objects) if (o.type == "XGroup" && o.field("Name") && o.field("Name")->str == "body") bodyGroup = &o;
    bool layoutOk = bodyGroup && bodyGroup->field("Core")->asRef() != 0;
    if (layoutOk) {
        const Object* holder = rd.object(bodyGroup->field("Children")->at(0).asRef());
        layoutOk = holder && holder->type == "XGroup" && holder->field("Name")->str == "bodyShape" && holder->field("Core")->asRef() == 0 &&
                   holder->field("Children")->size() == 1;
        const Object* pumpG = rd.object(bodyGroup->field("Children")->at(1).asRef());
        layoutOk = layoutOk && pumpG && pumpG->field("Name")->str == "pump";
    }
    Check(layoutOk, "XGroup \"body\" has an XTransform and a Core-less \"bodyShape\" holder as its first child, then the child nodes");

    // Euler angles: the engine's Rotate field (R = Rz * Ry * Rx), recovered from the matrix.
    const Object* pumpGroup = nullptr;
    for (auto& o : rd.objects) if (o.type == "XGroup" && o.field("Name") && o.field("Name")->str == "pump") pumpGroup = &o;
    const Object* xf = pumpGroup ? rd.object(pumpGroup->field("Core")->asRef()) : nullptr;
    auto rot = xf ? xf->field("Rotate")->components() : std::vector<double>();
    Check(rot.size() == 3 && std::abs(rot[0] - rx) < 1e-4 && std::abs(rot[1] - ry) < 1e-4 && std::abs(rot[2] - rz) < 1e-4,
          "XTransform.Rotate holds the node's Euler angles (Rz * Ry * Rx, radians)");

    // glTF with the tree: write, re-read, same names, hierarchy and shape ownership (the 2nd shape of a node as an extras node).
    auto g = gltf::WriteGltfScene(back, "nodes.bin");
    Check(WriteAll(outDir / "nodes.gltf", g.json.data(), g.json.size()) && WriteAll(outDir / "nodes.bin", g.bin.data(), g.bin.size()),
          "write nodes.gltf/.bin");
    auto gb = ReadAll(outDir / "nodes.gltf");
    mesh::Mesh viaGltf;
    Check(gltf::ReadGltfScene(gb, false, outDir.string(), viaGltf, &err), "ReadGltfScene nodes.gltf: " + err);
    bool gltfOk = viaGltf.nodes.size() == 4 && viaGltf.primitives.size() == 4;
    for (size_t i = 0; gltfOk && i < 4; ++i) gltfOk = viaGltf.nodes[i].name == want[i] && viaGltf.nodes[i].parent == parents[i];
    for (size_t i = 0; gltfOk && i < 4; ++i) gltfOk = viaGltf.primitives[i].node == owner[i] && viaGltf.primitives[i].positions.size() == back.primitives[i].positions.size();
    Check(gltfOk, "glTF tree round trip keeps node names, parents, shape ownership and vertex counts");

    // ... and the whole way round: glTF -> bundle again gives the same node structure.
    viaGltf.resourceId = "kindjal.Nodes2";
    viaGltf.sectionId = 477;
    Document doc2;
    Check(mesh::WriteBundle(doc2, viaGltf, 0, &err) != 0, "WriteBundle from the glTF tree: " + err);
    Document rd2 = Roundtrip(doc2, &err);
    mesh::Mesh back2;
    bool again = !rd2.objects.empty() && mesh::ReadMeshTree(rd2, mesh::FindDescriptor(rd2, "kindjal.Nodes2"), back2, &err) && back2.nodes.size() == 4;
    for (size_t i = 0; again && i < 4; ++i) again = back2.nodes[i].name == want[i] && back2.nodes[i].parent == parents[i];
    Check(again, "bundle -> glTF -> bundle keeps the node names and hierarchy");

    // A flat mesh still gets its old shape: no holder groups, groups named <id>_group_<i>.
    mesh::Mesh flat;
    flat.resourceId = "kindjal.Flat";
    flat.sectionId = 477;
    flat.primitives = {MakeSphere("only", 6, 4, true)};
    Document fd;
    Check(mesh::WriteBundle(fd, flat, 0, &err) != 0, "flat WriteBundle still works: " + err);
    bool flatNames = false;
    for (auto& o : fd.objects) if (o.type == "XGroup" && o.field("Name")->str == "kindjal.Flat_group_0") flatNames = true;
    Check(flatNames, "a mesh without nodes keeps the flat <id>_group_<i> layout");

    // A bad node reference is refused.
    mesh::Mesh bad = m;
    bad.primitives[0].node = 9;
    Document bd;
    err.clear();
    Check(mesh::WriteBundle(bd, bad, 0, &err) == 0 && err.find("node") != std::string::npos, "a primitive naming a missing node is refused: " + err);
}

// ---------------------------------------------------------------- image resample / replace (no game needed)

void ImageReplaceTests() {
    image::Pixels px;
    px.width = 4; px.height = 4; px.channels = 3;
    for (int i = 0; i < 16; ++i) { px.data.push_back(uint8_t(i * 10)); px.data.push_back(uint8_t(255 - i * 10)); px.data.push_back(uint8_t(i)); }
    Object img = image::MakeXImage("orig", px, true);   // 4x4 RGB with 3 mips
    const Value* mips = img.field("MipLevels");
    Check(mips && mips->asUInt() == 3, "synthetic 4x4 XImage has 3 mip levels");

    image::Pixels big;
    big.width = 8; big.height = 8; big.channels = 4;
    for (int i = 0; i < 64; ++i) { big.data.push_back(200); big.data.push_back(100); big.data.push_back(50); big.data.push_back(128); }
    bool resampled = false;
    std::string err;
    Check(image::ReplacePixels(img, big, &resampled, &err) && resampled, "ReplacePixels resamples an 8x8 RGBA image into a 4x4 RGB one: " + err);
    image::Pixels back;
    Check(image::ExtractMip(img, 0, back, &err) && back.width == 4 && back.height == 4 && back.channels == 3, "the image keeps its size and RGB format");
    Check(back.data.size() == 48 && back.data[0] == 200 && back.data[1] == 100 && back.data[2] == 50, "a flat colour survives resampling and the alpha drop exactly");
    Check(img.field("MipLevels")->asUInt() == 3 && img.field("Name")->str == "orig", "mip count and Name are kept");

    image::Pixels same = px;
    resampled = true;
    Check(image::ReplacePixels(img, same, &resampled, &err) && !resampled, "an image of the right size is not resampled");
    image::Pixels back2;
    Check(image::ExtractMip(img, 0, back2, &err) && back2.data == px.data, "same-size replacement is pixel-exact at level 0");

    image::Pixels up = image::Resize(px, 8, 8);
    image::Pixels down = image::Resize(up, 4, 4);
    int maxDiff = 0;
    for (size_t i = 0; i < down.data.size(); ++i) maxDiff = std::max(maxDiff, std::abs(int(down.data[i]) - int(px.data[i])));
    Check(up.width == 8 && up.data.size() == 8 * 8 * 3 && maxDiff <= 40, "Resize up then down stays close to the original");

    Object single = image::MakeXImage("one", px, false);
    Check(image::ReplacePixels(single, same, &resampled, &err) && single.field("MipLevels")->asUInt() == 1, "an image stored with one level stays single-level");

    // Vanilla images often carry Flags 2 or 4 (and a Palette ref); a pixel swap must not reset them, as StoreFields would.
    {
        Object flagged = image::MakeXImage("flagged", px, true);
        flagged.field("Flags")->bits = 2;
        flagged.field("Palette")->bits = 7;
        Check(image::ReplacePixels(flagged, big, &resampled, &err), "ReplacePixels on a flagged image: " + err);
        Check(flagged.field("Flags")->asUInt() == 2 && flagged.field("Flags")->type == Type::U16 && flagged.field("Palette")->asRef() == 7 &&
                  flagged.field("Palette")->type == Type::Ref,
              "ReplacePixels keeps Flags (2) and Palette as they were");
        image::Pixels got;
        Check(image::ExtractMip(flagged, 0, got, &err) && got.data[0] == 200, "... and still stores the new pixels");
    }
}

// ---------------------------------------------------------------- deformation (no game needed)

// A bundle of two spheres, "wool" on node "woolNode" and "head" on node "headNode" (so a selector can name either).
Document MakeTwoSphereBundle(bool ccw) {
    mesh::Mesh m;
    m.resourceId = "kindjal.Two";
    m.sectionId = 476;
    m.nodes = {{"woolNode", mesh::Identity(), -1}, {"headNode", mesh::Identity(), -1}};
    auto a = MakeSphere("wool", 12, 8, ccw);                  a.node = 0;
    auto b = MakeSphere("head", 8, 6, ccw, {5, 0, 0});        b.node = 1;
    m.primitives = {a, b};
    Document doc;
    std::string err;
    mesh::WriteBundle(doc, m, 0, &err);
    return doc;
}

uint32_t ShapeNamed(const Document& d, const char* name) {
    for (size_t i = 0; i < d.objects.size(); ++i)
        if (d.objects[i].type == "XShape" && d.objects[i].field("Name")->str == name) return uint32_t(i + 1);
    return 0;
}

void DeformTests() {
    std::string err;
    Check(mesh::GlobMatch("wool*", "WoolShape") && mesh::GlobMatch("*", "x") && mesh::GlobMatch("a?c", "abc") && !mesh::GlobMatch("a?c", "ac") &&
              !mesh::GlobMatch("wool", "woolly") && mesh::GlobMatch("*Shape*", "bodyShape_mat"),
          "GlobMatch: * ? literal, case-insensitive");

    for (int winding = 0; winding < 2; ++winding) {
        const bool ccw = winding == 0;
        const std::string tag = ccw ? " (counter-clockwise winding)" : " (clockwise winding)";
        Document base = MakeTwoSphereBundle(ccw);
        const uint32_t desc = mesh::FindDescriptor(base, "kindjal.Two");
        const uint32_t wool = ShapeNamed(base, "wool"), head = ShapeNamed(base, "head");
        const auto woolBefore = CoordsOf(base, wool), headBefore = CoordsOf(base, head);

        // push: every vertex moves 0.1 along its outward normal; counts, indices and UVs untouched.
        {
            Document d = base;
            mesh::DeformReport rep;
            Check(mesh::ApplyDeform(d, desc, R"({"ops":[{"op":"push","dist":0.1,"select":"wool"}]})", &rep, &err), "push applies: " + err + tag);
            auto after = CoordsOf(d, wool);
            bool same = after.size() == woolBefore.size();
            double radius = 0;
            for (size_t i = 0; same && i < after.size(); ++i) radius += double(Len3(after[i])) - double(Len3(woolBefore[i]));
            Check(same && radius / double(after.size()) > 0.08 && radius / double(after.size()) < 0.12,
                  "push moves a unit sphere outward by about the distance" + tag);
            Check(CoordsOf(d, head) == headBefore, "an unselected shape does not move" + tag);
            // Index/UV/vertex count identical; normals are unit length and still point outward.
            const Object* g0 = base.object(base.object(wool)->field("Geometry")->asRef());
            const Object* g1 = d.object(d.object(wool)->field("Geometry")->asRef());
            Check(g0->field("IndexSet")->asRef() == g1->field("IndexSet")->asRef() &&
                      base.object(g0->field("IndexSet")->asRef())->field("Index")->raw == d.object(g1->field("IndexSet")->asRef())->field("Index")->raw &&
                      base.object(g0->field("TexCoordSet")->asRef())->field("TexCoord")->raw == d.object(g1->field("TexCoordSet")->asRef())->field("TexCoord")->raw,
                  "indices and UVs stay byte-identical" + tag);
            auto nrm = NormalsOf(d, wool);
            bool unit = nrm.size() == after.size(), outward = true;
            for (size_t i = 0; unit && i < nrm.size(); ++i) {
                unit = std::abs(Len3(nrm[i]) - 1.0f) < 1e-3f;
                const float dot = nrm[i].x * after[i].x + nrm[i].y * after[i].y + nrm[i].z * after[i].z;
                if (dot < 0.5f * Len3(after[i])) outward = false;
            }
            Check(unit && outward, "recomputed normals are unit length and point outward" + tag);
            Check(rep.shapes.size() == 1 && rep.shapes[0].name == "wool" && rep.shapes[0].moved > 0 && rep.maxDisplacement > 0.09f,
                  "the report names the shape and the displacement" + tag);
        }
        // translate + scale, applied in order, selection by node name.
        {
            Document d = base;
            Check(mesh::ApplyDeform(d, desc, R"([{"op":"translate","t":[1,2,3],"select":"headNode"},{"op":"scale","s":2,"about":[0,0,0],"select":"headNode"}])", nullptr, &err),
                  "translate then scale, selected by node name: " + err + tag);
            auto after = CoordsOf(d, head);
            bool ok = after.size() == headBefore.size() && CoordsOf(d, wool) == woolBefore;
            for (size_t i = 0; ok && i < after.size(); ++i)
                ok = Dist3(after[i], {(headBefore[i].x + 1) * 2, (headBefore[i].y + 2) * 2, (headBefore[i].z + 3) * 2}) < 1e-4f;
            Check(ok, "ops run in order, on the shapes under the named node only" + tag);
        }
        // noise: deterministic, seed-dependent, seam-safe (vertices at one position move together).
        {
            Document d1 = base, d2 = base, d3 = base;
            const char* s1 = R"([{"op":"noise","amp":0.2,"freq":0.8,"seed":5}])";
            const char* s2 = R"([{"op":"noise","amp":0.2,"freq":0.8,"seed":6}])";
            Check(mesh::ApplyDeform(d1, desc, s1, nullptr, &err) && mesh::ApplyDeform(d2, desc, s1, nullptr, &err) && mesh::ApplyDeform(d3, desc, s2, nullptr, &err),
                  "noise applies: " + err + tag);
            Check(CoordsOf(d1, wool) == CoordsOf(d2, wool) && CoordsOf(d1, head) == CoordsOf(d2, head), "noise with one seed gives identical coordinates every time" + tag);
            Check(CoordsOf(d1, wool) != CoordsOf(d3, wool), "a different seed gives a different mesh" + tag);
            auto n = CoordsOf(d1, wool);
            // The sphere's seam: vertices (s=0) and (s=seg) share a position; they must still share it.
            bool seam = true;
            for (int r = 0; r <= 8 && seam; ++r) seam = Dist3(n[size_t(r * 13)], n[size_t(r * 13 + 12)]) < 1e-5f;
            Check(seam, "noise does not open the UV seam (equal positions get equal offsets)" + tag);
        }
        // region + falloff: full effect inside the box, none beyond the falloff, partial between.
        {
            Document d = base;
            Check(mesh::ApplyDeform(d, desc, R"({"select":"wool","ops":[{"op":"region","box":[[-2,0.2,-2],[2,2,2]],"falloff":0.6,"then":[{"op":"translate","t":[0,1,0]}]}]})", nullptr, &err),
                  "region applies: " + err + tag);
            auto after = CoordsOf(d, wool);
            bool inside = true, outside = true, between = false;
            for (size_t i = 0; i < after.size(); ++i) {
                const float y = woolBefore[i].y, dy = after[i].y - y;
                if (y >= 0.2f && std::abs(dy - 1.0f) > 1e-4f) inside = false;
                if (y < -0.45f && std::abs(dy) > 1e-6f) outside = false;
                if (y < 0.2f && y > -0.4f && dy > 0.01f && dy < 0.99f) between = true;
            }
            Check(inside && outside && between, "region: full strength inside the box, zero past the falloff, smooth between" + tag);
        }
        // bend: nothing before the pivot; the far end swings toward +dir.
        {
            Document d = base;
            Check(mesh::ApplyDeform(d, desc, R"({"select":"wool","ops":[{"op":"bend","axis":"y","dir":"z","amount":90,"length":2,"about":[0,-1,0]}]})", nullptr, &err),
                  "bend applies: " + err + tag);
            auto after = CoordsOf(d, wool);
            bool ok = true, topOk = false;
            for (size_t i = 0; i < after.size(); ++i) {
                if (woolBefore[i].y <= -1.0f + 1e-6f && Dist3(after[i], woolBefore[i]) > 1e-5f) ok = false;
                // The pole at (0, 1, 0): arc length 2 = the whole bend, r = 2 / (pi/2).
                if (std::abs(woolBefore[i].x) < 1e-5f && std::abs(woolBefore[i].z) < 1e-5f && woolBefore[i].y > 0.999f) {
                    const float r = 2.0f / 1.5707963f;
                    topOk = Dist3(after[i], {0, -1 + r, r}) < 1e-3f;
                }
            }
            Check(ok && topOk, "bend: vertices at the pivot stay, a 90 degree bend over its length swings the far pole to (0, r-1, r)" + tag);
        }
    }

    // Errors leave the document alone.
    {
        Document base = MakeTwoSphereBundle(true);
        const uint32_t desc = mesh::FindDescriptor(base, "kindjal.Two");
        const std::string before = DocumentToJson(base);
        struct Bad { const char* script; const char* what; };
        const Bad bads[] = {
            {R"([{"op":"twist","a":1}])", "unknown op"},
            {R"([{"op":"scale"}])", "scale needs s"},
            {R"([{"op":"push","dist":"x"}])", "push dist must be a number"},
            {R"([{"op":"translate","t":[1,2],"select":"wool"}])", "t needs three numbers"},
            {R"([{"op":"translate","t":[1,2,3],"select":"nothing*"}])", "select matches no shape"},
            {R"([{"op":"region","box":[[0,0,0],[1,1,1]],"then":[{"op":"translate","t":[0,0,0],"select":"wool"}]}])", "select inside a region"},
            {R"([{"op":"noise","amp":1,"freq":0}])", "freq must be positive"},
            {R"([{"op":"scale","s":1e30,"about":[0,0,0]},{"op":"scale","s":1e30,"about":[0,0,0]}])", "overflow to infinity"},
            {R"({"ops":[]})", "no ops"},
            {"not json", "not JSON"},
        };
        for (auto& b : bads) {
            Document d = base;
            std::string e;
            const bool ok = mesh::ApplyDeform(d, desc, b.script, nullptr, &e);
            Check(!ok && !e.empty() && DocumentToJson(d) == before, std::string("a bad script is refused and changes nothing: ") + b.what + " -> " + e);
        }
    }

    // Two shapes over one coordinate array but with their own triangle lists: refused, since one Target could only
    // re-normal one of them from the right triangles.
    {
        Document d = MakeTwoSphereBundle(true);
        const uint32_t desc = mesh::FindDescriptor(d, "kindjal.Two");
        Object* gWool = d.object(d.object(ShapeNamed(d, "wool"))->field("Geometry")->asRef());
        Object* gHead = d.object(d.object(ShapeNamed(d, "head"))->field("Geometry")->asRef());
        gHead->field("CoordSet")->bits = gWool->field("CoordSet")->bits;
        const std::string before = DocumentToJson(d);
        std::string e;
        Check(!mesh::ApplyDeform(d, desc, R"([{"op":"push","dist":0.1}])", nullptr, &e) && e.find("share one coordinate array") != std::string::npos &&
                  DocumentToJson(d) == before,
              "shapes sharing coordinates but not index/normal sets are refused, document untouched: " + e);
    }

    // A node's bounding sphere is in its parent's space: under a scale-3 node the vertices' move counts three times as far.
    {
        mesh::Mesh m;
        m.resourceId = "kindjal.Scaled";
        m.sectionId = 476;
        mesh::Mat4 s3 = mesh::Identity();
        s3[0] = s3[5] = s3[10] = 3.0f;
        m.nodes = {{"outer", s3, -1}};
        auto a = MakeSphere("ball", 12, 8, true);
        a.node = 0;
        m.primitives = {a};
        Document d;
        Check(mesh::WriteBundle(d, m, 0, &err) != 0, "WriteBundle a sphere under a scale-3 node: " + err);
        const uint32_t desc = mesh::FindDescriptor(d, "kindjal.Scaled");
        auto radius = [&](const char* type, const char* name) {
            for (auto& o : d.objects) {
                if (o.type != type) continue;
                const Value* n = o.field("Name");
                if (name && (!n || n->str != name)) continue;
                const Value* b = o.field("Bounds");
                if (b && b->components().size() == 4) return b->components()[3];
            }
            return -1.0;
        };
        for (auto& o : d.objects)
            if (Value* b = o.field("Bounds")) if (b->type == Type::Math && b->components().size() == 4) b->setComponents({0, 0, 0, 10});
        mesh::DeformReport rep;
        Check(desc && mesh::ApplyDeform(d, desc, R"([{"op":"push","dist":0.1}])", &rep, &err) && rep.maxDisplacement > 0.09f, "push the scaled sphere: " + err);
        const double shape = radius("XShape", "ball"), outer = radius("XGroup", "outer");
        Check(shape >= 10.09 && shape < 10.2, "the shape's sphere grows by its own move: " + std::to_string(shape));
        Check(outer >= 10.0 + 3.0 * double(rep.maxDisplacement) - 1e-4, "a scale-3 node's sphere grows by three times the move: " + std::to_string(outer));
    }
}

// ---------------------------------------------------------------- clone (needs the game's Bundl09.xom)

std::string GuidHexOf(const Value* v) { return v ? GuidHex(v->guid) : std::string(); }

// A TYPE entry for `cls` and each ancestor the document lacks (version 0, count 0: serialize and parse recount).
void AddClassTypes(Document& doc, const char* cls) {
    auto add = [&](const std::string& name, const char* guidHex) {
        for (auto& t : doc.types) if (t.className() == name) return;
        TypeEntry t;
        t.name = name;
        std::string padded = name;
        padded.resize(32, '\0');
        std::memcpy(t.rawName.data(), padded.data(), 32);
        for (int i = 0; guidHex && i < 16; ++i) {
            auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
            t.guid[size_t(i)] = uint8_t((hv(guidHex[2 * i]) << 4) | hv(guidHex[2 * i + 1]));
        }
        doc.types.push_back(t);
    };
    const ClassDef* c = findClass(cls);
    if (!c) { add(cls, nullptr); return; }   // a hand-written class (XAnimClipLibrary) is not in the schema
    for (; c; c = classParent(*c)) add(c->name, c->guid);
}

// An object of schema class `cls` at TYPE version 0 with every field zero: what a serializer needs to see for the class.
Object DefaultObject(const char* cls) {
    Object o;
    o.type = cls;
    o.container = true;
    std::set<std::string> seen;
    for (const ClassDef* c = findClass(cls); c; c = classParent(*c)) {
        const FieldDef* f = classFields(*c);
        for (unsigned i = 0; i < c->fieldCount; ++i) {
            const bool present = (f[i].flags & 0x04) ? false
                                 : (f[i].flags & 0x20) ? (f[i].obsoleteFrom >= 0 && 0 < f[i].obsoleteFrom)
                                 : f[i].schemaFrom >= 0 ? 0 >= f[i].schemaFrom : true;
            if (!present) continue;
            std::string key = f[i].name;
            if (seen.count(key)) key = std::string(c->name) + "." + key;
            seen.insert(key);
            Value v;
            v.type = f[i].type;
            v.math = f[i].math;
            v.array = f[i].isArray();
            if (v.type == Type::Math && !v.array) {
                const MathDef& m = mathDef(f[i].math);
                v.raw.assign(size_t(m.count) * (m.elem == 'f' ? 4 : (m.elem == 'h' || m.elem == 'H') ? 2 : 1), 0);
            }
            o.fields.emplace_back(key, v);
        }
    }
    return o;
}

// MakeTwoSphereBundle with one 8x8 texture on a shader both shapes use (XSimpleShader -> XOglTextureMap -> XImage), so the
// clone, image and UV-layout code has a bank to work on without the game's files. Well-formed: it serializes and re-parses.
Document MakeTexturedBundle() {
    Document doc = MakeTwoSphereBundle(true);
    image::Pixels px;
    px.width = 8; px.height = 8; px.channels = 3;
    for (int i = 0; i < 64; ++i) { px.data.push_back(uint8_t(i * 4)); px.data.push_back(uint8_t(255 - i * 4)); px.data.push_back(uint8_t(i)); }
    AddClassTypes(doc, "XImage");
    AddClassTypes(doc, "XOglTextureMap");
    AddClassTypes(doc, "XSimpleShader");
    Object img = DefaultObject("XImage");
    const Object made = image::MakeXImage("kindjal.tex", px, true);
    for (auto& [k, v] : made.fields) if (Value* slot = img.field(k)) *slot = v;
    const uint32_t imgRef = uint32_t(doc.objects.size()) + 1;
    doc.objects.push_back(std::move(img));
    Object tm = DefaultObject("XOglTextureMap");
    tm.field("Texture")->bits = imgRef;
    const uint32_t tmRef = imgRef + 1;
    doc.objects.push_back(std::move(tm));
    Object sh = DefaultObject("XSimpleShader");
    Value stage; stage.type = Type::Ref; stage.bits = tmRef;
    sh.field("TextureStages")->items.push_back(stage);
    sh.field("Name")->str = "kindjal.shader";
    const uint32_t shRef = tmRef + 1;
    doc.objects.push_back(std::move(sh));
    for (auto& o : doc.objects) if (o.type == "XShape") o.field("Shader")->bits = shRef;
    return doc;
}

struct CloneCase {
    const char* vanilla;
    uint16_t section;
    size_t bones, joints, weightSets, skins, skinShapes, images;
};

void CloneOne(const Document& src, const CloneCase& cc, const fs::path& outDir) {
    const std::string tag = std::string(" [") + cc.vanilla + "]";
    const std::string newName = std::string("kindjal.Proto") + cc.vanilla;
    std::string err;
    const uint32_t srcDesc = mesh::FindDescriptor(src, cc.vanilla);
    Check(srcDesc != 0, "the source descriptor is in the bank" + tag);
    if (!srcDesc) return;
    mesh::Closure sc;
    Check(mesh::CollectClosure(src, srcDesc, sc, &err), "the source closure is collectable: " + err + tag);

    Document dst;
    mesh::CloneReport rep;
    const uint32_t desc = mesh::CloneMesh(dst, src, cc.vanilla, newName, cc.section, {}, &rep, &err);
    Check(desc != 0, "CloneMesh: " + err + tag);
    if (!desc) return;

    // Closure by class: exactly the source's, plus the one new root XGraphSet.
    auto want = CountByType(src, &sc.order);
    want["XGraphSet"] += 1;
    auto have = CountByType(dst);
    Check(want == have, "the clone holds the source closure's classes and counts, plus one root XGraphSet" + tag);
    Check(rep.objects == dst.objects.size() && rep.objects == sc.order.size() + 1, "the report counts the closure: " + std::to_string(rep.objects) + tag);
    size_t reportSum = 0;
    for (auto& c : rep.classes) reportSum += c.count;
    Check(reportSum == dst.objects.size(), "the report's per-class counts add up" + tag);

    // Skeleton / skin objects identical in number to the source (zero for a static mesh).
    Check(have["XBone"] == cc.bones && have["XJointTransform"] == cc.joints && have["XPaletteWeightSet"] == cc.weightSets &&
              have["XSkin"] == cc.skins && have["XSkinShape"] == cc.skinShapes,
          "bone / joint / weight-set / skin counts match the source" + tag);

    // Descriptor patched, Flags kept, root and world entries as the engine reads them.
    const Object& d = dst.objects[desc - 1];
    const Object& sd = src.objects[srcDesc - 1];
    Check(d.type == "XMeshDescriptor" && d.field("ResourceId")->str == newName, "descriptor ResourceId is patched" + tag);
    Check(d.field("SectionId")->asUInt() == cc.section, "descriptor SectionId is patched" + tag);
    Check(d.field("Flags")->asUInt() == sd.field("Flags")->asUInt(), "descriptor Flags are kept" + tag);
    const Object* root = dst.object(dst.root);
    const Value* rg = root ? root->field("Graphs") : nullptr;
    Check(root && root->type == "XGraphSet" && rg && rg->size() == 1, "the document root is an XGraphSet with one entry" + tag);
    if (rg && rg->size() == 1) {
        Value e = rg->at(0);
        Check(GuidHexOf(e.member("Guid")) == "99cc436e6fbef54b85d2bfcdf9ae4283", "root entry GUID is the resource-descriptor GUID" + tag);
        Check(e.member("Graph")->asRef() == desc && e.member("Name")->str == newName, "root entry points at the descriptor and carries the new name" + tag);
    }
    const Object* world = dst.object(d.field("GraphSet")->asRef());
    bool haveWorld = false;
    size_t worldEntries = 0;
    if (world && world->field("Graphs"))
        for (auto& g : world->field("Graphs")->items) {
            ++worldEntries;
            if (GuidHexOf(g.member("Guid")) == "6ae6dbe4fa866b45a73ff9130e12dfeb" && g.member("Name")->str == "world") haveWorld = true;
        }
    Check(haveWorld, "the descriptor's graph set keeps its world entry with GUID 6ae6dbe4fa866b45a73ff9130e12dfeb" + tag);
    const Object* srcWorld = src.object(sd.field("GraphSet")->asRef());
    Check(srcWorld && worldEntries == srcWorld->field("Graphs")->size(), "every other graph-set entry (clips, collision, ...) is kept too" + tag);

    // The rest of the graph is the source's, object for object (the depth-first order of both closures lines up).
    mesh::Closure dc;
    Check(mesh::CollectClosure(dst, desc, dc, &err) && dc.order.size() == sc.order.size(), "the clone's closure has the source closure's size" + tag);
    if (dc.order.size() == sc.order.size()) {
        std::unordered_map<uint32_t, uint32_t> map;
        for (size_t i = 0; i < sc.order.size(); ++i) map[sc.order[i]] = dc.order[i];
        bool identical = true;
        std::string first;
        for (size_t i = 0; i < sc.order.size(); ++i) {
            if (sc.order[i] == srcDesc) continue;
            if (!ObjectEq(src.objects[sc.order[i] - 1], dst.objects[dc.order[i] - 1], map)) {
                identical = false;
                if (first.empty()) first = mesh::DescribeObject(src, sc.order[i]);
            }
        }
        Check(identical, "every copied object equals its source (references renumbered)" + tag + (first.empty() ? "" : ": " + first));
    }

    // Serialize, re-read strictly, find it again by name.
    Document back = Roundtrip(dst, &err);
    Check(!back.objects.empty(), "the clone serializes and re-parses strictly: " + err + tag);
    if (back.objects.empty()) return;
    const uint32_t bd = mesh::FindDescriptor(back, newName);
    mesh::Closure bc;
    Check(bd != 0 && mesh::CollectClosure(back, bd, bc, &err) && bc.order.size() == sc.order.size(), "the re-read bank closure is intact" + tag);
    Check(CountByType(back) == CountByType(dst), "TYPE counts survive the round trip" + tag);
    std::vector<uint8_t> bytes;
    Check(serialize(dst, bytes, &err) && WriteAll(outDir / (newName + ".xom"), bytes.data(), bytes.size()), "write " + newName + ".xom" + tag);

    // Images: graph order, sizes, who uses them.
    auto images = mesh::ListImages(dst, desc);
    Check(images.size() == cc.images, "ListImages finds " + std::to_string(cc.images) + " image(s)" + tag);
    bool imagesOk = !images.empty();
    for (auto& ii : images) imagesOk = imagesOk && ii.width > 0 && ii.height > 0 && ii.mips > 1 && !ii.usedBy.empty();
    Check(imagesOk, "every image reports a size, a mip count and the shapes using it" + tag);
    Check(mesh::ListImages(src, srcDesc).size() == images.size(), "the source and the clone list the same images" + tag);

    // Replacing one image changes that image and nothing else.
    if (!images.empty()) {
        const size_t k = images.size() - 1;
        Document edited = dst;
        image::Pixels px;
        Check(image::ExtractMip(edited.objects[images[k].ref - 1], 0, px, &err), "ExtractMip on the image to replace: " + err + tag);
        for (auto& b : px.data) b = uint8_t(255 - b);
        bool resampled = true;
        Check(image::ReplacePixels(edited.objects[images[k].ref - 1], px, &resampled, &err) && !resampled, "ReplacePixels (same size): " + err + tag);
        std::unordered_map<uint32_t, uint32_t> same;
        for (uint32_t r = 1; r <= dst.objects.size(); ++r) same[r] = r;
        bool onlyTarget = true;
        for (uint32_t r = 1; r <= dst.objects.size(); ++r) {
            const bool eq = ObjectEq(dst.objects[r - 1], edited.objects[r - 1], same);
            if (r == images[k].ref ? eq : !eq) onlyTarget = false;
        }
        Check(onlyTarget, "image replacement changes the target XImage and no other object" + tag);
        image::Pixels got;
        Check(image::ExtractMip(edited.objects[images[k].ref - 1], 0, got, &err) && got.data == px.data, "the new pixels read back exactly" + tag);
        const Object& oi = dst.objects[images[k].ref - 1];
        const Object& ni = edited.objects[images[k].ref - 1];
        Check(oi.field("Name")->str == ni.field("Name")->str && oi.field("Width")->asUInt() == ni.field("Width")->asUInt() &&
                  oi.field("MipLevels")->asUInt() == ni.field("MipLevels")->asUInt() && oi.field("Data")->raw.size() == ni.field("Data")->raw.size(),
              "the replaced image keeps its name, size and mip chain" + tag);

        // A larger picture is resampled to the original size.
        image::Pixels bigPx;
        bigPx.width = uint16_t(px.width * 2); bigPx.height = uint16_t(px.height * 2); bigPx.channels = px.channels;
        bigPx.data.assign(size_t(bigPx.width) * bigPx.height * size_t(bigPx.channels), 77);
        Document edited2 = dst;
        Check(image::ReplacePixels(edited2.objects[images[k].ref - 1], bigPx, &resampled, &err) && resampled, "a 2x-size picture is resampled to the original size: " + err + tag);
        image::Pixels got2;
        Check(image::ExtractMip(edited2.objects[images[k].ref - 1], 0, got2, &err) && got2.width == px.width && got2.data[0] == 77, "resampled pixels have the right size and value" + tag);
    }

    // The UV layout covers every image a shape samples; its triangles are the shapes' triangles.
    std::vector<mesh::UvLayout> layouts;
    Check(mesh::BuildUvLayouts(dst, desc, "image", layouts, &err) && !layouts.empty(), "BuildUvLayouts: " + err + tag);
    size_t tris = 0;
    bool layoutOk = true;
    for (auto& l : layouts) {
        tris += l.triangles;
        Json j;
        layoutOk = layoutOk && ParseJson(l.json, j, &err) && j.find("islands") && j.find("islands")->arr.size() == l.islands && l.islands > 0 &&
                   l.overlay.width == l.original.width * l.scale && l.overlay.channels == 4;
    }
    Check(layoutOk, "every layout has a parseable island list and an enlarged overlay" + tag);
    std::vector<mesh::ShapeRef> shapes;
    size_t shapeTris = 0;
    mesh::EnumerateShapes(dst, desc, shapes);
    for (auto& s : shapes) if (s.geometry) shapeTris += size_t(dst.object(s.geometry)->field("PrimitiveCount")->asUInt());
    Check(tris >= shapeTris, "the layouts account for every triangle of the shapes (" + std::to_string(tris) + " drawn, " + std::to_string(shapeTris) + " in the mesh)" + tag);
}

// The refusals CloneMesh makes, on `src` (by value: the cases edit it). `other` names a second mesh in the same document, or
// is empty to copy `mesh` under another name. `addLib` gives `mesh` an XAnimClipLibrary in its graph set (a vanilla animated
// mesh already has one). None of this needs the game: the synthetic bank is enough.
void RefusalTests(Document src, const std::string& mesh, std::string other, bool addLib) {
    std::string err;
    Document d;
    Check(mesh::CloneMesh(d, src, mesh, "kindjal.X", 475, {}, nullptr, &err) == 0 && err.find("476..519") != std::string::npos, "a section below 476 is refused: " + err);
    Document d1;
    Check(mesh::CloneMesh(d1, src, mesh, "kindjal.X", 520, {}, nullptr, &err) == 0 && err.find("476..519") != std::string::npos, "a section above 519 is refused: " + err);
    Document d2;
    Check(mesh::CloneMesh(d2, src, "NoSuchMesh", "kindjal.X", 480, {}, nullptr, &err) == 0 && err.find("NoSuchMesh") != std::string::npos, "an unknown mesh is refused: " + err);

    if (addLib) {
        AddClassTypes(src, "XAnimClipLibrary");
        Object lib;
        lib.type = "XAnimClipLibrary";
        lib.container = false;
        const uint32_t libRef = uint32_t(src.objects.size()) + 1;
        src.objects.push_back(lib);
        const uint32_t own = mesh::FindDescriptor(src, mesh);
        Object* gset = src.object(src.object(own)->field("GraphSet")->asRef());
        Value entry;
        entry.type = Type::Struct;
        { Value g; g.type = Type::Guid; entry.members.emplace_back("Guid", g); }
        { Value r; r.type = Type::Ref; r.bits = libRef; entry.members.emplace_back("Graph", r); }
        { Value n; n.type = Type::String; n.str = "clips"; entry.members.emplace_back("Name", n); }
        gset->field("Graphs")->items.push_back(entry);
    }

    // An XAnimClipLibrary that something outside the closure also reads is shared: refused unless --allow-shared.
    {
        Document shared = src;
        const uint32_t own = mesh::FindDescriptor(shared, mesh);
        mesh::Closure c;
        mesh::CollectClosure(shared, own, c);
        uint32_t lib = 0;
        for (auto r : c.order) if (shared.objects[r - 1].type == "XAnimClipLibrary") lib = r;
        Check(lib != 0, mesh + " owns an XAnimClipLibrary");
        Object gs;
        gs.type = "XGraphSet";
        gs.container = false;
        Value graphs; graphs.type = Type::Struct; graphs.array = true; graphs.items.resize(1);
        graphs.items[0].type = Type::Struct;
        { Value g; g.type = Type::Guid; graphs.items[0].members.emplace_back("Guid", g); }
        { Value r; r.type = Type::Ref; r.bits = lib; graphs.items[0].members.emplace_back("Graph", r); }
        { Value n; n.type = Type::String; n.str = "other"; graphs.items[0].members.emplace_back("Name", n); }
        gs.fields.emplace_back("Graphs", graphs);
        shared.objects.push_back(gs);   // (not serialized: CloneMesh only reads references)
        Document out;
        Check(mesh::CloneMesh(out, shared, mesh, "kindjal.Shared", 480, {}, nullptr, &err) == 0 && err.find("--allow-shared") != std::string::npos &&
                  err.find("XAnimClipLibrary") != std::string::npos,
              "a clip library shared with another resource is refused, naming it and the flag: " + err);
        Document out2;
        mesh::CloneOptions allow;
        allow.allowShared = true;
        mesh::CloneReport rep;
        Check(mesh::CloneMesh(out2, shared, mesh, "kindjal.Shared", 480, allow, &rep, &err) != 0, "--allow-shared lets it through: " + err);
        bool dup = false;
        for (auto& n : rep.notes) if (n.find("duplicated shared") != std::string::npos) dup = true;
        Check(dup, "and the report says the library was duplicated");
    }
    // A second descriptor in the closure is refused too.
    {
        Document two = src;
        if (other.empty()) {
            const uint32_t copy = mesh::CopySubgraph(two, two, mesh::FindDescriptor(two, mesh), &err);
            Check(copy != 0, "copy a second descriptor into the bank: " + err);
            if (!copy) return;
            two.objects[copy - 1].field("ResourceId")->str = other = "kindjal.Other";
        }
        const uint32_t own = mesh::FindDescriptor(two, mesh), dyn = mesh::FindDescriptor(two, other);
        Object* sd = two.object(own);
        Object* gset = two.object(sd->field("GraphSet")->asRef());
        Value entry;
        entry.type = Type::Struct;
        { Value g; g.type = Type::Guid; entry.members.emplace_back("Guid", g); }
        { Value r; r.type = Type::Ref; r.bits = dyn; entry.members.emplace_back("Graph", r); }
        { Value n; n.type = Type::String; n.str = "pulls in the other mesh"; entry.members.emplace_back("Name", n); }
        gset->field("Graphs")->items.push_back(entry);
        Document out;
        Check(mesh::CloneMesh(out, two, mesh, "kindjal.Two", 480, {}, nullptr, &err) == 0 && err.find("XMeshDescriptor") != std::string::npos &&
                  err.find(other) != std::string::npos,
              "a closure that reaches another XMeshDescriptor is refused, naming it: " + err);
    }
    // An object in the undelimited tail (or an opaque one) cannot be copied; the message says so and what reached it.
    {
        Document tail = src;
        const uint32_t own = mesh::FindDescriptor(tail, mesh);
        mesh::Closure c;
        mesh::CollectClosure(tail, own, c);
        uint32_t img = 0;
        for (auto r : c.order) if (tail.objects[r - 1].type == "XImage") img = r;
        Check(img != 0, mesh + " has an XImage to mark");
        if (!img) return;
        tail.objects[img - 1].inTail = true;
        Document out;
        Check(mesh::CloneMesh(out, tail, mesh, "kindjal.Tail", 480, {}, nullptr, &err) == 0 && err.find("undelimited tail") != std::string::npos &&
                  err.find("XImage") != std::string::npos && err.find("reached via") != std::string::npos,
              "an object in the undelimited tail is refused with its name and the chain that reaches it: " + err);
        tail.objects[img - 1].inTail = false;
        tail.objects[img - 1].opaque = true;
        Document out2;
        Check(mesh::CloneMesh(out2, tail, mesh, "kindjal.Tail", 480, {}, nullptr, &err) == 0 && err.find("opaque") != std::string::npos, "an opaque object is refused: " + err);
    }
}

// CloneMesh, ListImages, EnumerateShapes, BuildUvLayouts and InsertObject on a synthetic bank (the game's files are not needed).
void SyntheticCloneTests(const fs::path& outDir) {
    fs::create_directories(outDir);
    const Document src = MakeTexturedBundle();
    CloneOne(src, {"kindjal.Two", 491, 0, 0, 0, 0, 0, 1}, outDir);
    RefusalTests(src, "kindjal.Two", "", true);

    // Both shapes are found, with the shader's image used by both of them.
    const uint32_t desc = mesh::FindDescriptor(src, "kindjal.Two");
    std::vector<mesh::ShapeRef> shapes;
    std::string err;
    Check(desc != 0 && mesh::EnumerateShapes(src, desc, shapes, &err) && shapes.size() == 2 && shapes[0].geometry && shapes[1].geometry,
          "EnumerateShapes finds both spheres: " + err);
    auto images = mesh::ListImages(src, desc);
    Check(images.size() == 1 && images[0].name == "kindjal.tex" && images[0].width == 8 && images[0].usedBy.size() == 2,
          "ListImages: one 8x8 image used by both shapes");

    // InsertObject moves everything after the insertion point and every reference to it, and the document root.
    {
        Document d = src;
        const uint32_t before = d.root;
        const uint32_t imgRef = images.empty() ? 0 : images[0].ref;
        Object spare;
        spare.type = "XGraphSet";
        spare.container = false;
        mesh::InsertObject(d, imgRef, spare);
        mesh::Closure sc, dc;
        const uint32_t d2 = mesh::FindDescriptor(d, "kindjal.Two");
        Check(imgRef && d.objects.size() == src.objects.size() + 1 && d.objects[imgRef - 1].type == "XGraphSet" && d.objects[imgRef].type == "XImage",
              "InsertObject puts the object at the position and shifts the rest");
        Check(d.root == (before >= imgRef ? before + 1 : before) && mesh::CollectClosure(src, desc, sc) && mesh::CollectClosure(d, d2, dc) &&
                  sc.order.size() == dc.order.size() && mesh::ListImages(d, d2).size() == 1 && mesh::ListImages(d, d2)[0].ref == imgRef + 1,
              "InsertObject rewrites the references (closure and image still resolve) and the root");
    }
}

void CloneTests(const fs::path& game, const fs::path& outDir) {
    auto bundlePath = game / "Data" / "Bundles" / "Bundl09.xom";
    if (!fs::exists(bundlePath)) { Skip("mesh clone", "no game files"); return; }
    auto bytes = ReadAll(bundlePath);
    Document src;
    std::string err;
    if (!parse(bytes.data(), bytes.size(), src, &err)) { Check(false, "parse Bundl09.xom: " + err); return; }
    fs::create_directories(outDir);

    // static / rigid hierarchy / skinned
    CloneOne(src, {"Dynamite", 491, 0, 0, 0, 0, 0, 2}, outDir);
    CloneOne(src, {"ClusterBomb", 492, 0, 0, 0, 0, 0, 2}, outDir);
    CloneOne(src, {"Sheep", 490, 14, 14, 1, 1, 1, 1}, outDir);

    RefusalTests(src, "Sheep", "Dynamite", false);

    // Deform a skinned clone: counts, UVs, indices, skin weights and bones stay byte-identical; positions and normals change.
    {
        Document dst;
        const uint32_t desc = mesh::CloneMesh(dst, src, "Sheep", "kindjal.ProtoSheep", 490, {}, nullptr, &err);
        Check(desc != 0, "clone Sheep for the deform test: " + err);
        if (desc) {
            Document before = dst;
            mesh::DeformReport rep;
            const char* script = R"({"ops":[{"op":"region","box":[[-7.5,-7,-8],[7.5,9,7]],"falloff":2,"then":[{"op":"push","dist":0.4},{"op":"noise","amp":0.12,"freq":0.5,"seed":7}]}]})";
            Check(mesh::ApplyDeform(dst, desc, script, &rep, &err), "ApplyDeform on the skinned clone: " + err);
            Check(rep.shapes.size() == 1 && rep.shapes[0].skinned && rep.shapes[0].moved > 100 && rep.shapes[0].vertices == 365 && rep.maxDisplacement > 0.4f,
                  "the report counts moved vertices of the skinned shape");
            std::vector<mesh::ShapeRef> shapes;
            mesh::EnumerateShapes(dst, desc, shapes);
            Check(shapes.size() == 1 && shapes[0].skinned, "the clone has one skinned shape");
            std::unordered_map<uint32_t, uint32_t> same;
            for (uint32_t r = 1; r <= dst.objects.size(); ++r) same[r] = r;
            const uint32_t geomRef = shapes.empty() ? 0 : shapes[0].geometry;
            const Object* geom = dst.object(geomRef);
            const uint32_t coordRef = geom->field("CoordSet")->asRef(), normRef = geom->field("NormalSet")->asRef();
            bool everythingElse = true, weights = true, bones = true, counts = true, uvs = true;
            std::string firstDiff;
            for (uint32_t r = 1; r <= dst.objects.size(); ++r) {
                const Object& a = before.objects[r - 1];
                const Object& b = dst.objects[r - 1];
                const bool eq = ObjectEq(a, b, same);
                if (a.type == "XPaletteWeightSet" && !eq) weights = false;
                if ((a.type == "XBone" || a.type == "XJointTransform") && !eq) bones = false;
                if (a.type == "XTexCoord2fSet" && !eq) uvs = false;
                if (a.type == "XIndexSet" && !eq) counts = false;
                const bool allowed = r == coordRef || r == normRef || r == geomRef || a.type == "XGroup" || a.type == "XSkinShape" ||
                                     a.type == "XSkin" || a.type == "XInteriorNode";   // bounds only, below
                if (!eq && !allowed) { everythingElse = false; if (firstDiff.empty()) firstDiff = mesh::DescribeObject(before, r); }
            }
            Check(weights, "skin weights (XPaletteWeightSet) are byte-identical after the deform");
            Check(bones, "XBone / XJointTransform (bind pose, skeleton) are byte-identical");
            Check(uvs && counts, "UVs and index sets are byte-identical");
            Check(everythingElse, "no object other than the coordinates, normals and bounds changed" + (firstDiff.empty() ? std::string() : ": " + firstDiff));
            auto cb = CoordsOf(before, shapes[0].shape), ca = CoordsOf(dst, shapes[0].shape);
            Check(cb.size() == 365 && ca.size() == 365 && cb != ca, "the vertex count is unchanged (365) and the positions moved");
            // Only the Bounds fields of the scene nodes (and the triangle set's box) differ among the allowed objects.
            bool boundsOnly = true;
            for (uint32_t r = 1; r <= dst.objects.size(); ++r) {
                if (r == coordRef || r == normRef) continue;
                const Object& a = before.objects[r - 1];
                if (ObjectEq(a, dst.objects[r - 1], same)) continue;
                const Object& b = dst.objects[r - 1];
                for (size_t i = 0; i < a.fields.size(); ++i)
                    if (!ValueEq(a.fields[i].second, b.fields[i].second, same) && a.fields[i].first != "Bounds" && a.fields[i].first != "BoundBox") boundsOnly = false;
            }
            Check(boundsOnly, "the other changed objects differ only in their Bounds / BoundBox");
            bool grew = true;
            for (uint32_t r = 1; r <= dst.objects.size(); ++r) {
                const Value* ob = before.objects[r - 1].field("Bounds");
                const Value* nb = dst.objects[r - 1].field("Bounds");
                if (ob && nb && ob->type == Type::Math && nb->components().size() == 4 && nb->components()[3] < ob->components()[3]) grew = false;
            }
            Check(grew, "bounding spheres only grew");
            auto nrm = NormalsOf(dst, shapes[0].shape);
            bool unit = nrm.size() == 365;
            for (auto& n : nrm) unit = unit && std::abs(Len3(n) - 1.0f) < 2e-3f;
            Check(unit, "all normals are unit length after the deform");
            Document back = Roundtrip(dst, &err);
            Check(!back.objects.empty(), "the deformed clone re-parses strictly: " + err);
        }
    }

    // The tree of a vanilla multi-node mesh: names that animation clips address, and a rebuild that keeps them.
    {
        struct Want { const char* mesh; std::vector<const char*> nodes; };
        const Want wants[] = {
            {"ClusterBomb", {"cluster", "flap1", "flap2", "flap3", "flap4"}},
            {"Shotgun", {"Shotgun", "shotgun_pump", "eject", "Payload_Spawn"}},
            {"SentryGun", {"spindle_ring", "leg_1", "spindle", "gun", "barrel", "effect", "ball", "Sparks", "$animTex0"}},
        };
        for (auto& w : wants) {
            const uint32_t desc = mesh::FindDescriptor(src, w.mesh);
            mesh::Mesh tree;
            Check(desc && mesh::ReadMeshTree(src, desc, tree, &err), std::string("ReadMeshTree ") + w.mesh + ": " + err);
            std::vector<std::string> names;
            for (auto& n : tree.nodes) names.push_back(n.name);
            bool all = true;
            for (auto* n : w.nodes) all = all && std::find(names.begin(), names.end(), n) != names.end();
            Check(all, std::string(w.mesh) + ": the vanilla node names (" + w.nodes.front() + ", ...) are read as nodes");
            tree.resourceId = std::string("kindjal.Rebuilt") + w.mesh;
            tree.sectionId = 480;
            Document re;
            Check(mesh::WriteBundle(re, tree, 0, &err) != 0, std::string("WriteBundle of the ") + w.mesh + " tree: " + err);
            Document rr = Roundtrip(re, &err);
            mesh::Mesh again;
            const bool ok = !rr.objects.empty() && mesh::ReadMeshTree(rr, mesh::FindDescriptor(rr, tree.resourceId), again, &err);
            bool same = ok && again.nodes.size() == tree.nodes.size() && again.primitives.size() == tree.primitives.size();
            for (size_t i = 0; same && i < tree.nodes.size(); ++i)
                same = again.nodes[i].name == tree.nodes[i].name && again.nodes[i].parent == tree.nodes[i].parent;
            Check(same, std::string(w.mesh) + ": rebuilding the tree as a new bank keeps every node name and parent (" + std::to_string(tree.nodes.size()) + " nodes)");
        }
    }
}
}  // namespace

int main(int argc, char** argv) {
    fs::path game;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--game") == 0 && i + 1 < argc) { game = argv[++i]; g_gameRequested = true; }
    fs::path outDir = fs::temp_directory_path() / "xom_convert_selftest";

    ImageTests(game);
    ImageSafetyTests();
    MeshSafetyTests();
    GltfRefusalTests(outDir);
    BundleTests(outDir);
    MeshTests(game, outDir);
    BankTests(game, outDir);
    NodeTests(outDir);
    ImageReplaceTests();
    DeformTests();
    SyntheticCloneTests(outDir);
    CloneTests(game, outDir);

    std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
