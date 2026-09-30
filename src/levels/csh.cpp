#include "levels/csh.h"

#include <fstream>
#include <system_error>

#include "levels/roots.h"
#include "tools/hash.h"

namespace melange::levels::csh {
namespace fs = std::filesystem;
namespace {
std::string ReadSidecar(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::string s;
    if (f) std::getline(f, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

bool WriteSidecar(const fs::path& p, const std::string& sha) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << sha << "\n";
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
    return !ec;
}

template <class Pred>
uint32_t RemoveShadows(const std::string& stem, const std::vector<fs::path>& mapsDirs, Pred pred) {
    uint32_t n = 0;
    for (const auto& d : mapsDirs) {
        std::error_code ec;
        for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2) || e2) continue;
            const std::wstring wn = it->path().filename().wstring();
            std::string name;
            bool ascii = true;
            for (wchar_t c : wn) {
                if (c < 0x20 || c >= 0x7f) ascii = false;
                name.push_back(static_cast<char>(c));
            }
            if (!ascii || !roots::IsShadowOf(name, stem) || !pred(it->path())) continue;
            if (fs::remove(it->path(), e2)) ++n;
        }
    }
    return n;
}
}  // namespace

uint32_t Purge(const std::string& stem, const std::vector<fs::path>& mapsDirs) {
    return RemoveShadows(stem, mapsDirs, [](const fs::path&) { return true; });
}

Result Guard(const fs::path& xan, const std::string& stem, const fs::path& sidecarDir, const std::vector<fs::path>& mapsDirs) {
    Result r;
    r.sha = hashutil::Sha256HexFile(xan.wstring());
    if (r.sha.empty()) {
        r.error = "cannot read " + xan.filename().string();
        return r;
    }
    // The surround is terrain too: a .hmp beside the .xan joins the recorded hash and the age check.
    std::string key = r.sha;
    const fs::path hmp = xan.parent_path() / (stem + ".hmp");
    std::error_code hec;
    const bool hasHmp = fs::is_regular_file(hmp, hec);
    if (hasHmp) {
        const std::string h = hashutil::Sha256HexFile(hmp.wstring());
        if (h.empty()) {
            r.error = "cannot read " + hmp.filename().string();
            return r;
        }
        key += " " + h;
    }
    const fs::path side = sidecarDir / (stem + ".xan.sha");
    r.changed = ReadSidecar(side) != key;
    if (r.changed) {
        r.deleted = Purge(stem, mapsDirs);
        if (!WriteSidecar(side, key)) {
            r.error = "cannot write " + side.filename().string();
            return r;
        }
    } else {
        std::error_code ec;
        auto newest = fs::last_write_time(xan, ec);
        std::error_code e3;
        if (const auto ht = hasHmp ? fs::last_write_time(hmp, e3) : newest; !ec && !e3 && ht > newest) newest = ht;
        if (!ec)
            r.deleted = RemoveShadows(stem, mapsDirs, [&](const fs::path& p) {
                std::error_code e2;
                const auto t = fs::last_write_time(p, e2);
                return !e2 && t < newest;
            });
    }
    r.ok = true;
    return r;
}
}  // namespace melange::levels::csh
