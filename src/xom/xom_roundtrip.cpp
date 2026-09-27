// xom_roundtrip - byte-identical round-trip check for the C++ XOM library
// (same file set and report shape as tools/xom/roundtrip_test.py).
//
//   xom_roundtrip [--game <WormsXHD dir>] [--bundles] [--maps] [-v] [files...]
#include "xom.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace wumfix::xom;

static bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

static std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static void collect(const fs::path& dir, const char* ext, std::vector<fs::path>& out) {
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && lower(e.path().extension().string()) == ext) out.push_back(e.path());
}

int main(int argc, char** argv) {
    fs::path game = "C:/Program Files (x86)/Steam/steamapps/common/WormsXHD";
    bool bundles = false, maps = false, verbose = false;
    std::vector<fs::path> files;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "--bundles") bundles = true;
        else if (a == "--maps") maps = true;
        else if (a == "-v") verbose = true;
        else files.push_back(a);
    }
    if (files.empty()) {
        fs::path data = game / "Data";
        collect(data / "Tweak", ".xom", files);
        collect(data, ".xom", files);
        if (bundles) collect(data / "Bundles", ".xom", files);
        if (maps) {
            collect(data / "Maps", ".xan", files);
            collect(data / "Maps", ".xom", files);
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return lower(a.string()) < lower(b.string());
        });
    }
    size_t pass = 0, fail = 0, err = 0, objs = 0, opaque = 0, fully = 0;
    std::map<std::string, size_t> opaqueTypes;
    for (auto& f : files) {
        std::vector<uint8_t> in, out;
        if (!readFile(f, in)) {
            ++err;
            std::printf("ERROR  %s: cannot read\n", f.string().c_str());
            continue;
        }
        Document doc;
        std::string e;
        if (!parse(in.data(), in.size(), doc, &e)) {
            ++err;
            std::printf("ERROR  %s: %s\n", f.string().c_str(), e.c_str());
            continue;
        }
        size_t op = 0;
        for (auto& o : doc.objects)
            if (o.opaque || o.inTail) {
                if (verbose && !o.error.empty())
                    std::printf("opaque %s: %s: %s\n", f.string().c_str(), o.type.c_str(), o.error.c_str());
                ++op;
                opaqueTypes[o.type]++;
            }
        objs += doc.objects.size();
        opaque += op;
        if (!op) ++fully;
        if (!serialize(doc, out, &e)) {
            ++err;
            std::printf("ERROR  %s: serialize: %s\n", f.string().c_str(), e.c_str());
            continue;
        }
        if (out == in) {
            ++pass;
            if (verbose) std::printf("ok     %s (%zu objects)\n", f.string().c_str(), doc.objects.size());
        } else {
            ++fail;
            size_t k = 0;
            while (k < in.size() && k < out.size() && in[k] == out[k]) ++k;
            std::printf("FAIL   %s: first difference at 0x%zx (sizes %zu vs %zu)\n", f.string().c_str(), k,
                        in.size(), out.size());
        }
    }
    std::printf("\nbyte-identical round trip: %zu/%zu files  (%zu mismatched, %zu errors)\n", pass, files.size(),
                fail, err);
    std::printf("fully decoded (no opaque objects): %zu/%zu files, %zu objects total, %zu opaque\n", fully,
                files.size() - err, objs, opaque);
    for (auto& [t, c] : opaqueTypes) std::printf("  opaque: %-40s %zu\n", t.c_str(), c);
    return pass == files.size() ? 0 : 1;
}
