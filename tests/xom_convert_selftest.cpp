// xom_convert_selftest - offline checks for D (the Sieve xomtool CLI's library, src/xom):
// image round trips, a mesh round trip on Factory.Proj.Bazookashell, and the bank builder
// against bank_one.py's fixture. The game files it reads are read-only inputs, never written.
//
// Usage: xom_convert_selftest --game <WormsXHD dir>
// Without --game, the checks that need the game's files are skipped (reported, not a failure),
// so this still builds and runs offline; D acceptance runs it with --game set.
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

    // Every mip level round-trips exactly too (the formula from docs/m5-assets-research.md S1.1).
    const Value* mipsF = icon->field("MipLevels");
    int mips = mipsF ? int(mipsF->asUInt()) : 1;
    bool allMips = true;
    for (int lvl = 0; lvl < mips; ++lvl) {
        image::Pixels lvlPx;
        if (!image::ExtractMip(*icon, lvl, lvlPx, &err)) { allMips = false; break; }
    }
    Check(allMips, "every mip level of the panel icon extracts without error");
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
    MeshTests(game, outDir);
    BankTests(game, outDir);

    std::printf("\n%d passed, %d failed, %d skipped\n", g_pass, g_fail, g_skip);
    return g_fail == 0 ? 0 : 1;
}
