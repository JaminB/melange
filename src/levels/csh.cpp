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
    const fs::path side = sidecarDir / (stem + ".xan.sha");
    r.changed = ReadSidecar(side) != r.sha;
    if (r.changed) {
        r.deleted = Purge(stem, mapsDirs);
        if (!WriteSidecar(side, r.sha)) {
            r.error = "cannot write " + side.filename().string();
            return r;
        }
    } else {
        std::error_code ec;
        const auto xanTime = fs::last_write_time(xan, ec);
        if (!ec)
            r.deleted = RemoveShadows(stem, mapsDirs, [&](const fs::path& p) {
                std::error_code e2;
                const auto t = fs::last_write_time(p, e2);
                return !e2 && t < xanTime;
            });
    }
    r.ok = true;
    return r;
}
}  // namespace melange::levels::csh
