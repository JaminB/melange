#include <algorithm>
#include <cstring>
#include <fstream>

#include "assets/crcsafe.h"

namespace melange::assets::crcsafe {
namespace {
constexpr size_t kMaxImage = 64u << 20;

template <class T>
bool At(const std::vector<uint8_t>& b, size_t off, T* v) {
    if (off > b.size() || b.size() - off < sizeof(T)) return false;
    std::memcpy(v, b.data() + off, sizeof(T));
    return true;
}

struct Image {
    const std::vector<uint8_t>& bytes;
    uint32_t base = 0;
    struct Section { uint32_t va, vsize, raw, rawSize; };
    std::vector<Section> sections;

    bool Offset(uint32_t va, size_t* off) const {
        if (va < base) return false;
        const uint32_t rva = va - base;
        for (const auto& s : sections)
            if (rva >= s.va && rva - s.va < s.rawSize && rva - s.va < std::max(s.vsize, s.rawSize)) {
                *off = static_cast<size_t>(s.raw) + (rva - s.va);
                return *off < bytes.size();
            }
        return false;
    }
};
}  // namespace

bool ParseImage(const std::vector<uint8_t>& b, std::vector<Entry>* out) {
    out->clear();
    uint16_t mz = 0, nsec = 0, optSize = 0, magic = 0;
    uint32_t pe = 0, sig = 0;
    if (!At(b, 0, &mz) || mz != 0x5a4d || !At(b, 0x3c, &pe) || !At(b, pe, &sig) || sig != 0x4550) return false;
    if (!At(b, pe + 6, &nsec) || !At(b, pe + 20, &optSize) || !At(b, pe + 24, &magic) || magic != 0x10b) return false;
    Image img{b};
    if (!At(b, pe + 24 + 28, &img.base) || nsec > 96) return false;
    const size_t sec0 = pe + 24 + optSize;
    for (uint16_t i = 0; i < nsec; ++i) {
        Image::Section s{};
        const size_t h = sec0 + 40u * i;
        if (!At(b, h + 8, &s.vsize) || !At(b, h + 12, &s.va) || !At(b, h + 16, &s.rawSize) || !At(b, h + 20, &s.raw))
            return false;
        img.sections.push_back(s);
    }
    std::vector<Entry> entries;
    for (int i = 0; i <= kExpectedCount; ++i) {
        size_t off = 0;
        uint32_t path = 0, crc = 0;
        if (!img.Offset(kTableVa + 8u * i, &off) || !At(b, off, &path) || !At(b, off + 4, &crc)) return false;
        if (!path) break;
        size_t p = 0;
        if (!img.Offset(path, &p)) return false;
        std::string s;
        while (p < b.size() && b[p] && s.size() < 260) s.push_back(static_cast<char>(b[p++]));
        if (s.empty() || p >= b.size() || b[p]) return false;
        entries.push_back({std::move(s), crc});
    }
    if (static_cast<int>(entries.size()) != kExpectedCount || entries.front().crc != kFirstCrc) return false;
    *out = std::move(entries);
    return true;
}

bool ReadFromExe(const std::wstring& exePath, std::vector<Entry>* out) {
    std::ifstream f(exePath, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > kMaxImage) return false;
    std::vector<uint8_t> b(static_cast<size_t>(size));
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(b.data()), size)) return false;
    return ParseImage(b, out);
}
}  // namespace melange::assets::crcsafe
