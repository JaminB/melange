// Offline self-test for the mod mesh-bank loader's pure half (no game): InspectBank on a synthetic bundle-shaped
// document and on a real bank when given one, plus the naming / section rules. Exit code 0 = all passed.
//
// Links src/assets/meshbank_inspect.cpp and melange_xom only. Usage: meshbank_selftest [--bank <file.xom> --mod <modId>]
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "assets/meshbank.h"
#include "xom/xom.h"

namespace meshes = melange::assets::meshes;
namespace xom = melange::xom;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

xom::Value Str(const std::string& s) {
    xom::Value v;
    v.type = xom::Type::String;
    v.str = s;
    return v;
}
xom::Value U16(uint16_t n) {
    xom::Value v;
    v.type = xom::Type::U16;
    v.bits = n;
    return v;
}
xom::Value Ref(uint32_t r) {
    xom::Value v;
    v.type = xom::Type::Ref;
    v.bits = r;
    return v;
}

void AddType(xom::Document& doc, const char* cls, const char* guidHex) {
    xom::TypeEntry t;
    t.name = cls;
    for (size_t i = 0; i < 16; ++i) t.guid[i] = static_cast<uint8_t>(std::stoul(std::string(guidHex + 2 * i, 2), nullptr, 16));
    std::string padded = cls;
    padded.resize(32, static_cast<char>(0));
    std::memcpy(t.rawName.data(), padded.data(), 32);
    doc.types.push_back(t);
}

// A minimal bundle-shaped document: root XGraphSet -> N XMeshDescriptors -> each an (empty) XGraphSet. Object order
// keeps every class contiguous (what the writer requires): graph sets first, then descriptors. GUIDs as in
// src/xom/mesh.cpp.
std::vector<uint8_t> Bundle(const std::vector<std::pair<std::string, uint16_t>>& meshes, bool badRoot = false) {
    xom::Document doc;
    AddType(doc, "XGraphSet", "0b3dbf644139bb40b1798f882d14449b");
    AddType(doc, "XMeshDescriptor", "dbb2e8a8c30af04ba47696f47cf924d2");
    const uint32_t n = static_cast<uint32_t>(meshes.size());
    // mesh graph sets: #1..#n ; root graph set: #n+1 ; descriptors: #n+2..#2n+1
    for (uint32_t i = 0; i < n; ++i) {
        xom::Object gs;
        gs.type = "XGraphSet";
        gs.container = false;
        xom::Value graphs;
        graphs.type = xom::Type::Struct;
        graphs.array = true;
        gs.fields.emplace_back("Graphs", graphs);
        doc.objects.push_back(gs);
    }
    {
        xom::Object root;
        root.type = badRoot ? "XMeshDescriptor" : "XGraphSet";
        root.container = false;
        xom::Value graphs;
        graphs.type = xom::Type::Struct;
        graphs.array = true;
        graphs.items.resize(n);
        for (uint32_t i = 0; i < n; ++i) {
            graphs.items[i].type = xom::Type::Struct;
            graphs.items[i].members.emplace_back("Guid", xom::Value{});
            graphs.items[i].members.emplace_back("Graph", Ref(n + 2 + i));
            graphs.items[i].members.emplace_back("Name", Str(meshes[i].first));
        }
        if (badRoot) {
            root.fields.emplace_back("ResourceId", Str("x"));
            root.fields.emplace_back("SectionId", U16(1));
            root.fields.emplace_back("GraphSet", Ref(1));
            root.fields.emplace_back("Flags", U16(8));
        } else {
            root.fields.emplace_back("Graphs", graphs);
        }
        doc.objects.push_back(root);
    }
    for (uint32_t i = 0; i < n; ++i) {
        xom::Object d;
        d.type = "XMeshDescriptor";
        d.container = false;
        d.fields.emplace_back("ResourceId", Str(meshes[i].first));
        d.fields.emplace_back("SectionId", U16(meshes[i].second));
        d.fields.emplace_back("GraphSet", Ref(i + 1));
        d.fields.emplace_back("Flags", U16(8));
        doc.objects.push_back(d);
    }
    doc.root = n + 1;
    std::vector<uint8_t> bytes;
    std::string err;
    if (!xom::serialize(doc, bytes, &err)) printf("serialize failed: %s\n", err.c_str());
    return bytes;
}

void SyntheticTests() {
    std::vector<meshes::BankEntry> e;
    uint16_t sec = 0;
    std::string err;
    Expect(meshes::InspectBank(Bundle({{"kindjal.NailBat", uint16_t(480)}, {"kindjal.Shell", uint16_t(480)}}), &e, &sec, &err), "two-mesh bank parses: " + err);
    Expect(e.size() == 2 && e[0].name == "kindjal.NailBat" && e[1].name == "kindjal.Shell" && e[0].flags == 8, "entries in root order");
    Expect(sec == 480, "section read");
    Expect(meshes::CheckEntries("kindjal", e, sec, &err), "naming rule accepts kindjal.*: " + err);
    Expect(!meshes::CheckEntries("other", e, sec, &err), "naming rule refuses another mod's prefix");
    Expect(!meshes::CheckEntries("kindjal", e, 9, &err) && err.find("outside") != std::string::npos, "section 9 (vanilla) refused");
    Expect(!meshes::CheckEntries("kindjal", e, 475, &err), "section 475 (kSectionCount sentinel) refused");
    Expect(meshes::CheckEntries("kindjal", e, 476, &err) && meshes::CheckEntries("kindjal", e, 519, &err), "476 and 519 accepted");
    Expect(!meshes::CheckEntries("kindjal", e, 520, &err), "520 (past the engine's 520-entry section arrays) refused");

    e.clear();
    Expect(!meshes::InspectBank(Bundle({{"kindjal.A", uint16_t(480)}, {"kindjal.B", uint16_t(481)}}), &e, &sec, &err) && err.find("SectionId") != std::string::npos,
           "mixed sections refused: " + err);
    e.clear();
    Expect(!meshes::InspectBank(Bundle({{"kindjal.A", uint16_t(480)}}, true), &e, &sec, &err), "non-XGraphSet root refused: " + err);
    std::vector<meshes::BankEntry> dup = {{"kindjal.A", 8}, {"kindjal.A", 8}};
    Expect(!meshes::CheckEntries("kindjal", dup, 480, &err) && err.find("twice") != std::string::npos, "duplicate name refused");
    std::vector<meshes::BankEntry> bare = {{"kindjal.", 8}};
    Expect(!meshes::CheckEntries("kindjal", bare, 480, &err), "empty name after the prefix refused");
    {
        // RelocateBank: the section is rewritten, nothing else is (names, order, flags), and the result is a valid bank.
        std::vector<uint8_t> moved;
        const auto src = Bundle({{"kindjal.NailBat", uint16_t(9)}, {"kindjal.Shell", uint16_t(9)}});
        Expect(meshes::RelocateBank(src, 477, &moved, &err), "RelocateBank: " + err);
        std::vector<meshes::BankEntry> m;
        uint16_t msec = 0;
        Expect(meshes::InspectBank(moved, &m, &msec, &err), "relocated bank parses: " + err);
        Expect(msec == 477 && m.size() == 2 && m[0].name == "kindjal.NailBat" && m[1].name == "kindjal.Shell" && m[0].flags == 8,
               "relocated bank: section 477, names/order/flags kept");
        Expect(meshes::CheckEntries("kindjal", m, msec, &err), "relocated bank passes the naming/section rule");
        std::vector<uint8_t> again;
        Expect(meshes::RelocateBank(moved, 9, &again, &err) && again == src, "relocating back reproduces the source bytes");
        Expect(!meshes::RelocateBank(std::vector<uint8_t>{1, 2, 3}, 477, &moved, &err), "RelocateBank refuses junk");
    }
    std::vector<uint8_t> junk = {1, 2, 3, 4};
    Expect(!meshes::InspectBank(junk, &e, &sec, &err), "junk bytes refused");
}

void RealBank(const char* path, const char* mod) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<meshes::BankEntry> e;
    uint16_t sec = 0;
    std::string err;
    Expect(!bytes.empty(), std::string("read ") + path);
    const bool ok = meshes::InspectBank(bytes, &e, &sec, &err);
    Expect(ok, std::string("InspectBank(") + path + "): " + err);
    if (ok) {
        printf("%s: section %u, %zu mesh(es)\n", path, sec, e.size());
        for (auto& x : e) printf("  %s (Flags %u)\n", x.name.c_str(), x.flags);
        Expect(meshes::CheckEntries(mod, e, sec, &err), std::string("CheckEntries(") + mod + "): " + err);
    }
}
}  // namespace

int main(int argc, char** argv) {
    const char* bank = nullptr;
    const char* mod = "kindjal";
    for (int i = 1; i + 1 < argc; ++i) {
        if (!strcmp(argv[i], "--bank")) bank = argv[++i];
        else if (!strcmp(argv[i], "--mod")) mod = argv[++i];
    }
    SyntheticTests();
    if (bank) RealBank(bank, mod);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
