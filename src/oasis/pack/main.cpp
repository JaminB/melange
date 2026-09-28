// oasis_pack <dir> <out.zip>: packs the web bundle deterministically (sorted names, fixed timestamps) and adds a
// "<name>.gz" copy of every compressible file over 1 KB, which the server sends as-is to gzip-capable clients.
#include <miniz.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
bool Compressible(const std::string& name) {
    for (const char* e : {".js", ".mjs", ".css", ".html", ".json", ".svg", ".txt", ".map", ".md"})
        if (name.size() > strlen(e) && name.compare(name.size() - strlen(e), strlen(e), e) == 0) return true;
    return false;
}

std::string Gzip(const std::string& in) {
    size_t n = 0;
    const int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(10, -15, MZ_DEFAULT_STRATEGY));
    void* d = tdefl_compress_mem_to_heap(in.data(), in.size(), &n, flags);
    if (!d) return {};
    std::string out("\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff", 10);
    out.append(static_cast<const char*>(d), n);
    mz_free(d);
    const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(in.data()), in.size());
    for (int i = 0; i < 4; ++i) out += static_cast<char>(crc >> (8 * i) & 0xff);
    const uint32_t size = static_cast<uint32_t>(in.size());
    for (int i = 0; i < 4; ++i) out += static_cast<char>(size >> (8 * i) & 0xff);
    return out;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: oasis_pack <dir> <out.zip>\n");
        return 2;
    }
    const fs::path root = argv[1];
    std::vector<std::string> names;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file()) names.push_back(fs::relative(it->path(), root).generic_string());
    if (ec || names.empty()) {
        fprintf(stderr, "oasis_pack: nothing to pack in %s\n", argv[1]);
        return 1;
    }
    std::sort(names.begin(), names.end());

    // 1980-01-01 00:00 local time maps back to the same DOS timestamp in every time zone.
    std::tm tm{};
    tm.tm_year = 80;
    tm.tm_mday = 1;
    tm.tm_isdst = 0;
    MZ_TIME_T stamp = mktime(&tm);

    const std::string tmp = std::string(argv[2]) + ".tmp";
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_file(&zip, tmp.c_str(), 0)) {
        fprintf(stderr, "oasis_pack: cannot write %s\n", tmp.c_str());
        return 1;
    }
    size_t raw = 0;
    for (const auto& name : names) {
        std::ifstream f(root / fs::path(name), std::ios::binary);
        const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        raw += data.size();
        bool ok = mz_zip_writer_add_mem_ex_v2(&zip, name.c_str(), data.data(), data.size(), nullptr, 0, 9, 0, 0, &stamp,
                                              nullptr, 0, nullptr, 0);
        if (ok && Compressible(name) && data.size() > 1024) {
            const std::string gz = Gzip(data);
            ok = !gz.empty() && mz_zip_writer_add_mem_ex_v2(&zip, (name + ".gz").c_str(), gz.data(), gz.size(), nullptr, 0,
                                                            MZ_NO_COMPRESSION, 0, 0, &stamp, nullptr, 0, nullptr, 0);
        }
        if (!ok) {
            fprintf(stderr, "oasis_pack: failed to add %s\n", name.c_str());
            mz_zip_writer_end(&zip);
            return 1;
        }
    }
    const bool ok = mz_zip_writer_finalize_archive(&zip) && mz_zip_writer_end(&zip);
    if (!ok) return 1;
    fs::rename(tmp, argv[2], ec);
    if (ec) {
        fs::remove(argv[2], ec);
        fs::rename(tmp, argv[2], ec);
    }
    printf("oasis_pack: %zu files, %zu bytes -> %s\n", names.size(), raw, argv[2]);
    return ec ? 1 : 0;
}
