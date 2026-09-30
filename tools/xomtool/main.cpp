// xomtool - the Sieve CLI: unpack/pack/inspect/diff/convert/bank/report/level on melange::xom.
// See docs/xomtool.md. Exit codes: 0 ok, 1 usage, 2 input error, 3 write refused.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
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

int CmdLevel(int argc, char** argv);  // level.cpp

namespace {

bool ReadFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}
bool ReadTextFile(const std::string& path, std::string& out) {
    std::vector<uint8_t> b;
    if (!ReadFile(path, b)) return false;
    out.assign(reinterpret_cast<const char*>(b.data()), b.size());
    return true;
}
bool WriteFile(const std::string& path, const void* data, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), std::streamsize(n));
    return bool(f);
}
bool WriteTextFile(const std::string& path, const std::string& s) { return WriteFile(path, s.data(), s.size()); }

std::string Ext(const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return "";
    std::string e = path.substr(pos + 1);
    for (auto& c : e) c = char(std::tolower(uint8_t(c)));
    return e;
}
std::string DirOf(const std::string& path) {
    auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? "." : path.substr(0, pos);
}
std::string BaseNoExt(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    auto dot = b.find_last_of('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

int Usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  xomtool unpack <in.xom|.xan> [-o <out.json>] [--split <dir>]\n"
        "  xomtool pack <in.json|dir> <out.xom>\n"
        "  xomtool inspect <in.xom> [--object <NAME|#N>] [--type <TypeName>]\n"
        "  xomtool diff <a.xom> <b.xom>\n"
        "  xomtool convert <texture.png> --into <file.xom> --as <Name> [--section N] [--mips=0|1] [-o <out.xom>]\n"
        "  xomtool convert <Name> --from <file.xom> --out <texture.png> [--mip N]\n"
        "  xomtool convert <mesh.gltf|.glb> --into <file.xom> --as <Name> [--section N]\n"
        "                  [--material-from <Name>] [--material-file <file.xom>] [--texture <png>] [-o <out.xom>]\n"
        "  xomtool convert <Name> --from <file.xom> --out <mesh.gltf>\n"
        "  xomtool bank --from <src.xom> --object <BaseName> --as <NewName> [--set Field=value ...] --out <out.xom>\n"
        "  xomtool report <in.xom> -o <out.md>\n"
        "  xomtool level unpack|build|diff ...  (xomtool level for details)\n");
    return 1;
}

bool LoadDoc(const std::string& path, Document& doc, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) { if (error) *error = "cannot read " + path; return false; }
    return parse(bytes.data(), bytes.size(), doc, error);
}
bool SaveDoc(const std::string& path, const Document& doc, std::string* error) {
    std::vector<uint8_t> out;
    if (!serialize(doc, out, error)) return false;
    if (!WriteFile(path, out.data(), out.size())) { if (error) *error = "cannot write " + path; return false; }
    return true;
}

// ---------------------------------------------------------------- argument helpers

struct Args {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> flags;  // --name value (or --name=value)
    std::vector<std::string> setPairs;                        // repeatable --set Field=value

    bool has(const std::string& name) const {
        for (auto& [k, v] : flags) { (void)v; if (k == name) return true; }
        return false;
    }
    std::string get(const std::string& name, const std::string& def = "") const {
        for (auto& [k, v] : flags) if (k == name) return v;
        return def;
    }
};

Args ParseArgs(int argc, char** argv, int start) {
    Args a;
    for (int i = start; i < argc; ++i) {
        std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            std::string name = s.substr(2);
            auto eq = name.find('=');
            if (eq != std::string::npos) {
                a.flags.emplace_back(name.substr(0, eq), name.substr(eq + 1));
                continue;
            }
            if (name == "set" && i + 1 < argc) { a.setPairs.push_back(argv[++i]); continue; }
            if (i + 1 < argc && argv[i + 1][0] != '-') { a.flags.emplace_back(name, argv[++i]); continue; }
            a.flags.emplace_back(name, "1");
        } else if (s == "-o" && i + 1 < argc) {
            a.flags.emplace_back("o", argv[++i]);
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

// ---------------------------------------------------------------- unpack / pack

int CmdUnpack(const Args& a) {
    if (a.positional.empty()) return Usage();
    Document doc;
    std::string err;
    if (!LoadDoc(a.positional[0], doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    std::string json = DocumentToJson(doc);
    if (a.has("split")) {
        std::string dir = a.get("split");
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::create_directories(dir + "/objects", ec);
        Json full;
        ParseJson(json, full, &err);
        Json index = Json::Obj();
        for (auto& kv : full.obj) {
            if (kv.first == "objects") continue;
            index.set(kv.first, kv.second);
        }
        Json objIndex = Json::Arr();
        const Json* objects = full.find("objects");
        if (objects)
            for (size_t i = 0; i < objects->arr.size(); ++i) {
                char name[32];
                std::snprintf(name, sizeof(name), "%06zu.json", i);
                std::string file = std::string("objects/") + name;
                if (!WriteTextFile(dir + "/" + file, WriteJson(objects->arr[i]))) {
                    std::fprintf(stderr, "xomtool: cannot write %s\n", file.c_str());
                    return 3;
                }
                Json entry = Json::Obj();
                const Json* t = objects->arr[i].find("type");
                entry.set("type", t ? *t : Json::Str(""));
                entry.set("file", Json::Str(file));
                objIndex.arr.push_back(std::move(entry));
            }
        index.set("objects", std::move(objIndex));
        if (!WriteTextFile(dir + "/index.json", WriteJson(index))) {
            std::fprintf(stderr, "xomtool: cannot write %s/index.json\n", dir.c_str());
            return 3;
        }
        std::printf("wrote %s/index.json and %zu object file(s)\n", dir.c_str(), objects ? objects->arr.size() : size_t(0));
        return 0;
    }
    if (a.has("o")) {
        if (!WriteTextFile(a.get("o"), json)) { std::fprintf(stderr, "xomtool: cannot write %s\n", a.get("o").c_str()); return 3; }
    } else {
        std::fwrite(json.data(), 1, json.size(), stdout);
    }
    return 0;
}

int CmdPack(const Args& a) {
    if (a.positional.size() < 2) return Usage();
    std::string src = a.positional[0], out = a.positional[1];
    std::string json, err;
    std::error_code ec;
    if (std::filesystem::is_directory(src, ec)) {
        if (!ReadTextFile(src + "/index.json", json)) { std::fprintf(stderr, "xomtool: cannot read %s/index.json\n", src.c_str()); return 2; }
        Json index;
        if (!ParseJson(json, index, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
        const Json* objIdx = index.find("objects");
        if (!objIdx) { std::fprintf(stderr, "xomtool: index.json has no objects\n"); return 2; }
        Json objects = Json::Arr();
        for (auto& entry : objIdx->arr) {
            const Json* file = entry.find("file");
            if (!file) { std::fprintf(stderr, "xomtool: index.json entry missing \"file\"\n"); return 2; }
            std::string objJson;
            if (!ReadTextFile(src + "/" + file->str, objJson)) {
                std::fprintf(stderr, "xomtool: cannot read %s/%s\n", src.c_str(), file->str.c_str());
                return 2;
            }
            Json obj;
            if (!ParseJson(objJson, obj, &err)) { std::fprintf(stderr, "xomtool: %s: %s\n", file->str.c_str(), err.c_str()); return 2; }
            objects.arr.push_back(std::move(obj));
        }
        index.set("objects", std::move(objects));
        json = WriteJson(index);
    } else {
        if (!ReadTextFile(src, json)) { std::fprintf(stderr, "xomtool: cannot read %s\n", src.c_str()); return 2; }
    }
    Document doc;
    if (!JsonToDocument(json, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    if (!SaveDoc(out, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 3; }
    std::printf("wrote %s\n", out.c_str());
    return 0;
}

// ---------------------------------------------------------------- inspect

std::string FieldSummary(const Object& o) {
    if (o.type == "XImage") {
        const Value *w = o.field("Width"), *h = o.field("Height"), *f = o.field("Format"), *m = o.field("MipLevels");
        if (w && h) {
            std::ostringstream ss;
            ss << w->asUInt() << "x" << h->asUInt() << " format=" << (f ? f->asUInt() : 0) << " mips=" << (m ? m->asUInt() : 1);
            return ss.str();
        }
    }
    if (o.type == "XIndexedTriangleSet") {
        const Value* pc = o.field("PrimitiveCount");
        if (pc) return std::to_string(pc->asUInt()) + " triangles";
    }
    if (o.type == "XMeshDescriptor" || o.type == "XBaseResourceDescriptor" || o.type == "XBitmapDescriptor") {
        const Value* rid = o.field("ResourceId");
        if (rid) return "ResourceId=" + rid->str;
    }
    return {};
}

int CmdInspect(const Args& a) {
    if (a.positional.empty()) return Usage();
    Document doc;
    std::string err;
    if (!LoadDoc(a.positional[0], doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    std::string wantType = a.get("type");
    std::string wantObject = a.get("object");
    if (wantObject.empty() && wantType.empty()) {
        std::printf("types: ");
        for (size_t i = 0; i < doc.types.size(); ++i)
            std::printf("%s%s v%u x%u", i ? ", " : "", doc.types[i].className().c_str(), doc.types[i].version, doc.types[i].count);
        std::printf("\nroot: #%u  strings: %zu  objects: %zu\n", doc.root, doc.strings.size(), doc.objects.size());
        return 0;
    }
    for (size_t i = 0; i < doc.objects.size(); ++i) {
        const Object& o = doc.objects[i];
        bool match = true;
        if (!wantType.empty()) match = o.type == wantType;
        if (match && !wantObject.empty()) {
            if (wantObject[0] == '#') {
                match = (std::to_string(i + 1) == wantObject.substr(1));
            } else {
                const Value* n = o.field("Name");
                const Value* rid = o.field("ResourceId");
                match = (n && n->str == wantObject) || (rid && rid->str == wantObject);
            }
        }
        if (!match) continue;
        std::string sum = FieldSummary(o);
        std::printf("#%zu %s%s%s\n", i + 1, o.type.c_str(), sum.empty() ? "" : "  ", sum.c_str());
        if (!wantObject.empty() && !o.opaque && !o.inTail) {
            // Reuse the melange-xom/1 field encoding (hex arrays, {"ref":n}, non-finite floats,
            // ...) by wrapping this one object in a throwaway single-object document.
            Document one;
            one.types = doc.types;
            one.strings = doc.strings;
            one.guidRec = doc.guidRec;
            one.schmRec = doc.schmRec;
            one.objects.push_back(o);
            one.root = 1;
            Json j;
            std::string perr;
            if (ParseJson(DocumentToJson(one), j, &perr)) {
                const Json* objs = j.find("objects");
                const Json* fields = (objs && !objs->arr.empty()) ? objs->arr[0].find("fields") : nullptr;
                if (fields)
                    for (auto& kv : fields->obj) std::printf("    %s = %s\n", kv.first.c_str(), WriteJson(kv.second).c_str());
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------- diff

void JsonDiff(const std::string& path, const Json& a, const Json& b, std::vector<std::string>& out) {
    if (a.kind != b.kind) {
        out.push_back(path + ": " + WriteJson(a) + " -> " + WriteJson(b));
        return;
    }
    switch (a.kind) {
        case Json::Kind::Object: {
            std::vector<std::string> keys;
            for (auto& kv : a.obj) keys.push_back(kv.first);
            for (auto& kv : b.obj) if (std::find(keys.begin(), keys.end(), kv.first) == keys.end()) keys.push_back(kv.first);
            for (auto& k : keys) {
                const Json* av = a.find(k);
                const Json* bv = b.find(k);
                std::string p = path.empty() ? k : path + "." + k;
                if (!av) out.push_back(p + ": (absent) -> " + WriteJson(*bv));
                else if (!bv) out.push_back(p + ": " + WriteJson(*av) + " -> (absent)");
                else JsonDiff(p, *av, *bv, out);
            }
            break;
        }
        case Json::Kind::Array: {
            size_t n = std::max(a.arr.size(), b.arr.size());
            for (size_t i = 0; i < n; ++i) {
                std::string p = path + "[" + std::to_string(i) + "]";
                if (i >= a.arr.size()) out.push_back(p + ": (absent) -> " + WriteJson(b.arr[i]));
                else if (i >= b.arr.size()) out.push_back(p + ": " + WriteJson(a.arr[i]) + " -> (absent)");
                else JsonDiff(p, a.arr[i], b.arr[i], out);
            }
            break;
        }
        default: {
            std::string as = WriteJson(a), bs = WriteJson(b);
            if (as != bs) out.push_back(path + ": " + as + " -> " + bs);
        }
    }
}

int CmdDiff(const Args& a) {
    if (a.positional.size() < 2) return Usage();
    Document da, db;
    std::string err;
    if (!LoadDoc(a.positional[0], da, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    if (!LoadDoc(a.positional[1], db, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    Json ja, jb;
    ParseJson(DocumentToJson(da), ja, &err);
    ParseJson(DocumentToJson(db), jb, &err);
    const Json *oa = ja.find("objects"), *ob = jb.find("objects");
    size_t n = std::max(oa->arr.size(), ob->arr.size());
    int diffs = 0;
    for (size_t i = 0; i < n; ++i) {
        std::string label = "#" + std::to_string(i + 1);
        if (i < oa->arr.size()) {
            const Json* fields = oa->arr[i].find("fields");
            const Json* rid = fields ? fields->find("ResourceId") : nullptr;
            const Json* nm = fields ? fields->find("Name") : nullptr;
            if (rid && rid->kind == Json::Kind::String) label += " " + rid->str;
            else if (nm && nm->kind == Json::Kind::String) label += " " + nm->str;
        }
        if (i >= oa->arr.size()) { std::printf("%s: only in b\n", label.c_str()); ++diffs; continue; }
        if (i >= ob->arr.size()) { std::printf("%s: only in a\n", label.c_str()); ++diffs; continue; }
        std::vector<std::string> lines;
        JsonDiff("", oa->arr[i], ob->arr[i], lines);
        for (auto& l : lines) { std::printf("%s %s\n", label.c_str(), l.c_str()); ++diffs; }
    }
    std::printf("%d difference(s)\n", diffs);
    return 0;
}

// ---------------------------------------------------------------- image helpers (stb)

bool LoadPng(const std::string& path, image::Pixels& px, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) { if (error) *error = "cannot read " + path; return false; }
    int w, h, ch;
    unsigned char* data = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &ch, 0);
    if (!data) { if (error) *error = std::string("cannot decode PNG: ") + stbi_failure_reason(); return false; }
    if (ch != 3 && ch != 4) {
        stbi_image_free(data);
        data = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &ch, 4);
        ch = 4;
        if (!data) { if (error) *error = "unsupported PNG pixel format"; return false; }
    }
    px.width = uint16_t(w);
    px.height = uint16_t(h);
    px.channels = ch;
    px.data.assign(data, data + size_t(w) * h * ch);
    stbi_image_free(data);
    return true;
}
bool SavePng(const std::string& path, const image::Pixels& px, std::string* error) {
    int ok = stbi_write_png(path.c_str(), px.width, px.height, px.channels, px.data.data(), px.width * px.channels);
    if (!ok && error) *error = "cannot write PNG: " + path;
    return ok != 0;
}

const Object* FindImageByName(const Document& doc, const std::string& name) {
    for (auto& o : doc.objects)
        if (o.type == "XImage" && !o.opaque && !o.inTail) {
            const Value* n = o.field("Name");
            if (n && n->str == name) return &o;
        }
    return nullptr;
}

// ---------------------------------------------------------------- convert

int CmdConvertTextureIn(const Args& a, const std::string& pngPath) {
    std::string into = a.get("into"), as = a.get("as");
    if (into.empty() || as.empty()) { std::fprintf(stderr, "xomtool: convert <png> needs --into and --as\n"); return 1; }
    image::Pixels px;
    std::string err;
    if (!LoadPng(pngPath, px, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    Document doc;
    if (!LoadDoc(into, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    bool mips = a.get("mips", "1") != "0";
    Object img = image::MakeXImage(as, px, mips);
    int xImagePos = -1;
    for (size_t i = 0; i < doc.types.size(); ++i) if (doc.types[i].className() == "XImage") xImagePos = int(i);
    if (xImagePos < 0) {
        const ClassDef* cd = findClass("XImage");
        std::array<uint8_t, 16> g{};
        for (int i = 0; i < 16; ++i) {
            auto hv = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
            g[size_t(i)] = uint8_t((hv(cd->guid[2 * i]) << 4) | hv(cd->guid[2 * i + 1]));
        }
        TypeEntry t;
        t.name = "XImage";
        t.version = 0;
        t.guid = g;
        std::string padded = "XImage";
        padded.resize(32, '\0');
        std::memcpy(t.rawName.data(), padded.data(), 32);
        doc.types.push_back(t);
        doc.objects.push_back(std::move(img));
    } else {
        size_t insertAt = doc.objects.size();
        for (size_t i = 0; i < doc.objects.size(); ++i) {
            int p = -1;
            for (size_t k = 0; k < doc.types.size(); ++k) if (doc.types[k].className() == doc.objects[i].type) p = int(k);
            if (p > xImagePos) { insertAt = i; break; }
        }
        doc.objects.insert(doc.objects.begin() + long(insertAt), std::move(img));
    }
    std::string out = a.has("o") ? a.get("o") : into;
    if (!SaveDoc(out, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 3; }
    std::printf("wrote %s (added XImage \"%s\")\n", out.c_str(), as.c_str());
    return 0;
}

int CmdConvertTextureOut(const Args& a, const std::string& name) {
    std::string from = a.get("from"), out = a.get("out");
    if (from.empty() || out.empty()) { std::fprintf(stderr, "xomtool: convert <Name> --from needs --out\n"); return 1; }
    Document doc;
    std::string err;
    if (!LoadDoc(from, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    const Object* img = FindImageByName(doc, name);
    if (!img) { std::fprintf(stderr, "xomtool: no XImage named \"%s\" in %s\n", name.c_str(), from.c_str()); return 2; }
    int mip = a.has("mip") ? std::atoi(a.get("mip").c_str()) : 0;
    image::Pixels px;
    if (!image::ExtractMip(*img, mip, px, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    if (!SavePng(out, px, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 3; }
    std::printf("wrote %s (%ux%u, %d channels)\n", out.c_str(), px.width, px.height, px.channels);
    return 0;
}

bool ReadGltfFile(const std::string& path, std::vector<mesh::Primitive>& prims, std::string* error) {
    std::vector<uint8_t> bytes;
    if (!ReadFile(path, bytes)) { if (error) *error = "cannot read " + path; return false; }
    bool isGlb = Ext(path) == "glb";
    return gltf::ReadGltf(bytes, isGlb, DirOf(path), prims, error);
}

int CmdConvertMeshIn(const Args& a, const std::string& gltfPath) {
    std::string into = a.get("into"), as = a.get("as");
    if (into.empty() || as.empty()) { std::fprintf(stderr, "xomtool: convert <mesh> needs --into and --as\n"); return 1; }
    std::string err;
    std::vector<mesh::Primitive> prims;
    if (!ReadGltfFile(gltfPath, prims, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    Document doc;
    if (!LoadDoc(into, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    uint32_t shaderRef = 0;
    if (a.has("material-from")) {
        // The template mesh usually lives in a different file (a vanilla bundle) than the one
        // being written into; --material-file names it, defaulting to --into for the case where
        // both happen to be the same file.
        Document matDoc;
        const Document* matSrc = &doc;
        if (a.has("material-file")) {
            if (!LoadDoc(a.get("material-file"), matDoc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
            matSrc = &matDoc;
        }
        uint32_t srcShaderRef = mesh::FindMeshShader(*matSrc, a.get("material-from"), &err);
        if (!srcShaderRef) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
        shaderRef = mesh::CopySubgraph(doc, *matSrc, srcShaderRef, &err);
        if (!shaderRef) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
        if (a.has("texture")) {
            uint32_t texRef = mesh::FindShaderTexture(doc, shaderRef, &err);
            if (!texRef) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
            image::Pixels px;
            if (!LoadPng(a.get("texture"), px, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
            if (!image::StoreFields(doc.objects[texRef - 1], px, true, &err)) {
                std::fprintf(stderr, "xomtool: %s\n", err.c_str());
                return 2;
            }
        }
    }
    mesh::Mesh m;
    m.resourceId = as;
    m.sectionId = a.has("section") ? uint16_t(std::atoi(a.get("section").c_str())) : 0;
    m.primitives = prims;
    uint32_t descRef = mesh::WriteMesh(doc, m, shaderRef, &err);
    if (!descRef) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 3; }
    std::string out = a.has("o") ? a.get("o") : into;
    if (!SaveDoc(out, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 3; }
    std::printf("wrote %s (added XMeshDescriptor \"%s\", %zu primitive(s))\n", out.c_str(), as.c_str(), m.primitives.size());
    return 0;
}

int CmdConvertMeshOut(const Args& a, const std::string& name) {
    std::string from = a.get("from"), out = a.get("out");
    if (from.empty() || out.empty()) { std::fprintf(stderr, "xomtool: convert <Name> --from needs --out\n"); return 1; }
    Document doc;
    std::string err;
    if (!LoadDoc(from, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    mesh::Mesh m;
    if (!mesh::ReadMesh(doc, name, m, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    auto res = gltf::WriteGltf(m.primitives, BaseNoExt(out) + ".bin");
    if (!WriteTextFile(out, res.json)) { std::fprintf(stderr, "xomtool: cannot write %s\n", out.c_str()); return 3; }
    std::string binPath = DirOf(out) + "/" + BaseNoExt(out) + ".bin";
    if (!WriteFile(binPath, res.bin.data(), res.bin.size())) {
        std::fprintf(stderr, "xomtool: cannot write %s\n", binPath.c_str());
        return 3;
    }
    std::printf("wrote %s and %s (%zu primitive(s))\n", out.c_str(), binPath.c_str(), m.primitives.size());
    return 0;
}

int CmdConvert(const Args& a) {
    if (a.positional.empty()) return Usage();
    std::string first = a.positional[0];
    std::string ext = Ext(first);
    if (ext == "png") {
        if (a.has("into")) return CmdConvertTextureIn(a, first);
        return CmdConvertTextureOut(a, first);
    }
    if (ext == "gltf" || ext == "glb") {
        if (a.has("into")) return CmdConvertMeshIn(a, first);
        std::fprintf(stderr, "xomtool: convert <mesh> --into is the only mesh-import form\n");
        return 1;
    }
    if (a.has("out")) {
        std::string outExt = Ext(a.get("out"));
        if (outExt == "png") return CmdConvertTextureOut(a, first);
        if (outExt == "gltf") return CmdConvertMeshOut(a, first);
    }
    std::fprintf(stderr, "xomtool: convert: cannot tell texture from mesh here; name a .png/.gltf/.glb file\n");
    return 1;
}

// ---------------------------------------------------------------- bank

int CmdBank(const Args& a) {
    std::string from = a.get("from"), object = a.get("object"), as = a.get("as"), out = a.get("out");
    if (from.empty() || object.empty() || as.empty() || out.empty()) {
        std::fprintf(stderr, "xomtool: bank needs --from --object --as --out\n");
        return 1;
    }
    Document doc;
    std::string err;
    if (!LoadDoc(from, doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    const Object* bank = nullptr;
    for (auto& o : doc.objects) if (o.type == "XDataBank") { bank = &o; break; }
    if (!bank) { std::fprintf(stderr, "xomtool: %s has no XDataBank\n", from.c_str()); return 2; }
    const Value* cr = bank->field("ContainerResources");
    uint32_t baseRef = 0;
    const Object* templateDetail = nullptr;
    if (cr)
        for (size_t i = 0; i < cr->size(); ++i) {
            const Object* det = doc.object(cr->at(i).asRef());
            if (!det || !det->field("Name") || det->field("Name")->str != object) continue;
            const Value* valueF = det->field("Value");
            if (!valueF) {
                std::fprintf(stderr, "xomtool: \"%s\"'s resource-details entry has no Value field in %s\n", object.c_str(),
                             from.c_str());
                return 2;
            }
            baseRef = valueF->asRef();
            templateDetail = det;  // keep *this* entry's own Flags, not some other resource's
            break;
        }
    if (!baseRef || !templateDetail) {
        std::fprintf(stderr, "xomtool: no container resource named \"%s\" in %s\n", object.c_str(), from.c_str());
        return 2;
    }
    const Object* baseObj = doc.object(baseRef);
    if (!baseObj) {
        std::fprintf(stderr, "xomtool: \"%s\"'s Value ref does not point to a real object in %s\n", object.c_str(), from.c_str());
        return 2;
    }
    std::string cls = baseObj->type;

    Object cont = *baseObj;
    for (auto& kv : a.setPairs) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) { std::fprintf(stderr, "xomtool: --set needs Field=value (got \"%s\")\n", kv.c_str()); return 1; }
        std::string field = kv.substr(0, eq), val = kv.substr(eq + 1);
        Value* fv = cont.field(field);
        if (!fv) { std::fprintf(stderr, "xomtool: %s has no field \"%s\"\n", cls.c_str(), field.c_str()); return 2; }
        switch (fv->type) {
            case Type::F32: case Type::F64: fv->setFloat(std::strtod(val.c_str(), nullptr)); break;
            case Type::Bool: fv->bits = (val == "1" || val == "true") ? 1 : 0; break;
            case Type::String: fv->str = val; break;
            case Type::I8: case Type::I16: case Type::I32: case Type::I64: fv->setInt(std::strtoll(val.c_str(), nullptr, 10)); break;
            case Type::U8: case Type::U16: case Type::U32: case Type::U64: case Type::Enum:
                fv->bits = std::strtoull(val.c_str(), nullptr, 10);
                break;
            default:
                std::fprintf(stderr, "xomtool: field \"%s\" has a type --set cannot write\n", field.c_str());
                return 2;
        }
    }

    Document out_;
    out_.version = doc.version;
    out_.reserved08 = doc.reserved08;
    out_.reserved24 = doc.reserved24;
    out_.guidRec = doc.guidRec;
    out_.schmRec = doc.schmRec;
    // Keep every original TYPE entry, in place: the format doesn't mind
    // an unused, zero-count entry, and a class version (e.g. an abstract ancestor's) can matter
    // for field gating even with no instances - dropping entries the bank does not need would
    // still be a valid file, but not a byte-identical one. Counts are recomputed below from the
    // objects actually written.
    out_.types = doc.types;
    Object detail = *templateDetail;
    { Value* nameF = detail.field("Name"); Value* valueF = detail.field("Value");
      if (!nameF || !valueF) { std::fprintf(stderr, "xomtool: internal error: bad resource-details template\n"); return 2; }
      nameF->str = as; valueF->bits = 3; }

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
    out_.objects.push_back(std::move(detail));
    out_.objects.push_back(std::move(bankCopy));
    out_.objects.push_back(std::move(cont));
    for (auto& t : out_.types) t.count = 0;
    for (auto& o : out_.objects)
        for (auto& t : out_.types)
            if (t.className() == o.type) ++t.count;
    out_.root = 2;
    out_.strings = doc.strings;

    if (!SaveDoc(out, out_, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 3; }
    Document check;
    if (!LoadDoc(out, check, &err)) { std::fprintf(stderr, "xomtool: wrote an unreadable bank: %s\n", err.c_str()); return 3; }
    std::printf("wrote %s: %s/%s/%s, root #%u\n", out.c_str(), check.objects[0].type.c_str(), check.objects[1].type.c_str(),
                check.objects[2].type.c_str(), check.root);
    return 0;
}

// ---------------------------------------------------------------- report

int CmdReport(const Args& a) {
    if (a.positional.empty() || !a.has("o")) { std::fprintf(stderr, "xomtool: report <in.xom> -o <out.md>\n"); return 1; }
    Document doc;
    std::string err;
    if (!LoadDoc(a.positional[0], doc, &err)) { std::fprintf(stderr, "xomtool: %s\n", err.c_str()); return 2; }
    const Object* bank = doc.object(doc.root);
    std::ostringstream md;
    md << "# " << a.positional[0] << "\n\nGenerated by `xomtool report`.\n\n";
    if (bank && bank->type == "XDataBank") {
        const Value* cr = bank->field("ContainerResources");
        md << "| Name | Class | Fields set |\n|---|---|---|\n";
        if (cr)
            for (size_t i = 0; i < cr->size(); ++i) {
                const Object* det = doc.object(cr->at(i).asRef());
                if (!det) continue;
                const Value* nameF = det->field("Name");
                const Value* valueF = det->field("Value");
                const Object* cont = valueF ? doc.object(valueF->asRef()) : nullptr;
                if (!nameF || !cont) continue;
                md << "| `" << nameF->str << "` | " << cont->type << " | " << cont->fields.size() << " |\n";
            }
    } else {
        md << "Object types: ";
        for (size_t i = 0; i < doc.types.size(); ++i) md << (i ? ", " : "") << doc.types[i].className() << " x" << doc.types[i].count;
        md << "\n";
    }
    if (!WriteTextFile(a.get("o"), md.str())) { std::fprintf(stderr, "xomtool: cannot write %s\n", a.get("o").c_str()); return 3; }
    std::printf("wrote %s\n", a.get("o").c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return Usage();
    std::string cmd = argv[1];
    if (cmd == "level") return CmdLevel(argc, argv);
    Args a = ParseArgs(argc, argv, 2);
    if (cmd == "unpack") return CmdUnpack(a);
    if (cmd == "pack") return CmdPack(a);
    if (cmd == "inspect") return CmdInspect(a);
    if (cmd == "diff") return CmdDiff(a);
    if (cmd == "convert") return CmdConvert(a);
    if (cmd == "bank") return CmdBank(a);
    if (cmd == "report") return CmdReport(a);
    return Usage();
}
