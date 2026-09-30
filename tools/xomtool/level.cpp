// xomtool level: the Erg level service's scene and build code on the command line.
//   level unpack <file.xan> [--xom <level.XOM>] [--hmp <file.hmp>] [--key <Multi.X>] [-o <scene.json>] [--blobs <dir>]
//   level build --patch <p.ergpatch.json> --game <game dir> --out <dir> [--stem <stem>]
//   level diff <a.scene.json> <b.scene.json> [-o <patch.json>]
#include <cstdio>
#include <string>
#include <vector>

#include "erg/build.h"
#include "erg/install.h"
#include "erg/load.h"
#include "erg/patch.h"
#include "erg/scene.h"
#include "erg/service.h"

namespace erg = melange::erg;

namespace {
struct LevelArgs {
    std::vector<std::string> positional;
    std::vector<std::pair<std::string, std::string>> flags;
    std::string get(const std::string& k, const std::string& def = "") const {
        for (auto& [n, v] : flags)
            if (n == k) return v;
        return def;
    }
};

LevelArgs Parse(int argc, char** argv, int start) {
    LevelArgs a;
    for (int i = start; i < argc; ++i) {
        std::string s = argv[i];
        if ((s.rfind("--", 0) == 0 || s == "-o") && i + 1 < argc) a.flags.emplace_back(s == "-o" ? "o" : s.substr(2), argv[++i]);
        else a.positional.push_back(s);
    }
    return a;
}

int Usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  xomtool level unpack <file.xan> [--xom <level.XOM>] [--hmp <file.hmp>] [--key <Multi.X>] [-o <scene.json>]\n"
                 "                       [--blobs <dir>]\n"
                 "  xomtool level build --patch <p.ergpatch.json> --game <game dir> --out <dir> [--stem <stem>]\n"
                 "  xomtool level diff <a.scene.json> <b.scene.json> [-o <patch.json>]\n");
    return 1;
}

bool Read(const std::string& path, size_t max, std::vector<uint8_t>* out) {
    std::string err;
    if (erg::install::ReadFile(erg::install::Widen(path), max, out, &err)) return true;
    std::fprintf(stderr, "xomtool level: %s\n", err.c_str());
    return false;
}

bool Write(const std::string& path, const void* data, size_t n) {
    FILE* f = _wfopen(erg::install::Widen(path).c_str(), L"wb");
    const bool ok = f && std::fwrite(data, 1, n, f) == n;
    if (f) std::fclose(f);
    if (!ok) std::fprintf(stderr, "xomtool level: cannot write %s\n", path.c_str());
    return ok;
}

bool Emit(const std::string& text, const std::string& out) {
    if (out.empty()) {
        std::fwrite(text.data(), 1, text.size(), stdout);
        std::fputc('\n', stdout);
        return true;
    }
    return Write(out, text.data(), text.size());
}

std::string Stem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string b = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = b.find('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

std::string Dir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

int Unpack(const LevelArgs& a) {
    if (a.positional.empty()) return Usage();
    const std::string xan = a.positional[0], stem = Stem(xan);
    erg::load::BaseFiles f;
    f.file = stem;
    f.key = a.get("key", "Multi." + stem);
    f.title = stem.size() <= 40 ? stem : stem.substr(0, 40);
    std::string xom = a.get("xom");
    if (xom.empty()) xom = Dir(Dir(xan)) + "\\" + stem + ".XOM";
    std::string hmp = a.get("hmp");
    if (hmp.empty() && erg::install::Exists(erg::install::Widen(Dir(xan) + "\\" + stem + ".hmp"))) hmp = Dir(xan) + "\\" + stem + ".hmp";
    if (!Read(xan, erg::load::kMaxXanBytes, &f.xan) || !Read(xom, erg::load::kMaxXomBytes, &f.xom)) return 2;
    if (!hmp.empty()) {
        std::vector<uint8_t> h;
        if (!Read(hmp, erg::load::kHmpBytes, &h)) return 2;
        f.hmp = std::move(h);
    }
    erg::load::Loaded L;
    std::string err;
    if (!erg::load::LoadScene(f, &L, &err)) {
        std::fprintf(stderr, "xomtool level: %s\n", err.c_str());
        return 2;
    }
    if (!Emit(erg::WriteScene(L.scene), a.get("o"))) return 3;
    const std::string blobs = a.get("blobs");
    if (!blobs.empty()) {
        if (!erg::install::MakeDirs(erg::install::Widen(blobs))) return 3;
        for (const auto& [ref, bytes] : L.blobs)
            if (!Write(blobs + "\\" + std::to_string(ref) + ".bin", bytes.data(), bytes.size())) return 3;
    }
    std::fprintf(stderr, "%zu frames, %zu details, %zu blobs\n", L.scene.frames.size(), L.scene.details.size(), L.scene.blobs.size());
    return 0;
}

int BuildCmd(const LevelArgs& a) {
    const std::string patchPath = a.get("patch"), game = a.get("game"), out = a.get("out");
    if (patchPath.empty() || game.empty() || out.empty()) return Usage();
    const std::wstring gameDir = erg::install::Widen(game), outDir = erg::install::Widen(out);
    if (erg::install::Inside(outDir, erg::install::DataDir(gameDir)) ||
        erg::install::Inside(outDir + L"\\x", erg::install::DataDir(gameDir))) {
        std::fprintf(stderr, "xomtool level: refusing to write under the game's Data folder\n");
        return 3;
    }
    std::vector<uint8_t> bytes;
    if (!Read(patchPath, erg::kMaxPatchBytes, &bytes)) return 2;
    erg::Patch p;
    std::string err;
    if (!erg::ParsePatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &p, &err)) {
        std::fprintf(stderr, "xomtool level: %s: %s\n", patchPath.c_str(), err.c_str());
        return 2;
    }
    std::vector<erg::build::File> files;
    int code = 0;
    if (!erg::service::BuildPatch(gameDir, erg::install::ScanPacks(gameDir), p, a.get("stem"), &files, &code, &err)) {
        std::fprintf(stderr, "xomtool level: %s\n", err.c_str());
        return 2;
    }
    for (const auto& f : files) {
        std::string rel = f.rel;
        for (auto& c : rel)
            if (c == '/') c = '\\';
        const std::wstring path = outDir + L"\\" + erg::install::Widen(rel);
        if (!erg::install::MakeDirs(path.substr(0, path.find_last_of(L'\\'))) ||
            !erg::install::WriteAtomic(path, f.bytes.data(), f.bytes.size(), &err)) {
            std::fprintf(stderr, "xomtool level: %s\n", err.empty() ? "cannot write" : err.c_str());
            return 3;
        }
        std::printf("%s (%zu bytes)\n", f.rel.c_str(), f.bytes.size());
    }
    return 0;
}

int Diff(const LevelArgs& a) {
    if (a.positional.size() < 2) return Usage();
    erg::Scene s[2];
    for (int i = 0; i < 2; ++i) {
        std::vector<uint8_t> b;
        std::string err;
        if (!Read(a.positional[static_cast<size_t>(i)], 16u << 20, &b)) return 2;
        if (!erg::ParseScene(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()), &s[i], &err)) {
            std::fprintf(stderr, "xomtool level: %s: %s\n", a.positional[static_cast<size_t>(i)].c_str(), err.c_str());
            return 2;
        }
    }
    return Emit(erg::WritePatch(erg::build::Diff(s[0], s[1])), a.get("o")) ? 0 : 3;
}
}  // namespace

int CmdLevel(int argc, char** argv) {
    if (argc < 3) return Usage();
    const std::string sub = argv[2];
    const LevelArgs a = Parse(argc, argv, 3);
    if (sub == "unpack") return Unpack(a);
    if (sub == "build") return BuildCmd(a);
    if (sub == "diff") return Diff(a);
    return Usage();
}
