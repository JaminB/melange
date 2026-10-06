// Offline self-test for the .wsr container (src/wormsign/format.cpp): round trips, truncated files, bad CRCs and a
// timed mutation run on the reader. Usage: wormsign_format_selftest [mutationSeconds=60] [sample.wsr]; the optional
// path receives a sample file for other readers. Exit code 0 = all passed.
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "wormsign/format.h"

using namespace melange::wormsign::wsr;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what);
    }
}

struct Src {
    uint32_t type;
    std::vector<uint8_t> bytes;
    bool deflate;
};

std::vector<Src> Sample(std::mt19937& rng, int chunks) {
    std::vector<Src> v;
    const std::string head = R"({"format":1,"engineHash":1,"exeBuild":"1077","tickMs":20})";
    v.push_back({kHEAD, std::vector<uint8_t>(head.begin(), head.end()), true});
    const uint32_t types[] = {kSEED, kPDRW, kSETP, kINPT, kTICK, kNOTE, kDETL};
    for (int i = 0; i < chunks; ++i) {
        Src s{types[rng() % 7], {}, (rng() & 1) != 0};
        const size_t n = rng() % 3 == 0 ? rng() % 16 : rng() % 20000;
        const bool compressible = rng() & 1;
        for (size_t k = 0; k < n; ++k) s.bytes.push_back(static_cast<uint8_t>(compressible ? (k / 7) & 0x0f : rng()));
        v.push_back(std::move(s));
    }
    return v;
}

std::vector<uint8_t> Write(const std::vector<Src>& src, bool close) {
    std::vector<uint8_t> out;
    Writer w;
    w.OpenMemory(&out);
    uint32_t tick = 1;
    for (const Src& s : src) {
        w.Chunk(s.type, s.bytes.data(), s.bytes.size(), s.deflate, tick, tick + 499);
        tick += 500;
    }
    if (close) Expect(w.Close(), "Close");
    else w.Abandon();
    return out;
}

bool SameChunks(const Reader& r, const std::vector<Src>& src, size_t n) {
    size_t i = 0;
    bool ok = true;
    r.ForEach(0, [&](const ChunkRef& c, const std::vector<uint8_t>& p) {
        if (i >= src.size() || c.type != src[i].type || p != src[i].bytes) ok = false;
        ++i;
    });
    return ok && i == n;
}

void RoundTrip(std::mt19937& rng) {
    for (int round = 0; round < 40; ++round) {
        const auto src = Sample(rng, round % 12);
        const auto bytes = Write(src, true);
        Reader r;
        std::string err;
        const bool ok = r.OpenMemory(bytes, &err);
        if (!ok) printf("  open: %s\n", err.c_str());
        Expect(ok, "round trip opens");
        Expect(r.Complete(), "round trip is complete");
        Expect(r.Format() == 1 && r.EngineHash() == 1, "HEAD format and engineHash");
        Expect(SameChunks(r, src, src.size()), "round trip payloads");
        Expect(r.Index().size() == src.size(), "index size");
        bool idx = r.Index().size() == src.size();
        for (size_t i = 0; idx && i < src.size(); ++i)
            idx = r.Index()[i].type == src[i].type && r.Index()[i].tickFrom == 1 + 500 * i && r.Index()[i].tickTo == 500 * (i + 1);
        Expect(idx, "index entries");
    }
}

void Deflates() {
    std::vector<uint8_t> big(100000, 7);
    const auto c = EncodeChunk(kTICK, big.data(), big.size(), true);
    Expect(c.size() < 2000, "compressible payload is deflated");
    const auto raw = EncodeChunk(kTICK, big.data(), big.size(), false);
    Expect(raw.size() == big.size() + kChunkHeaderBytes, "deflate off stores raw");
    std::vector<uint8_t> noise(3000);
    std::mt19937 rng(7);
    for (auto& b : noise) b = static_cast<uint8_t>(rng());
    const auto n = EncodeChunk(kTICK, noise.data(), noise.size(), true);
    Expect(n.size() == noise.size() + kChunkHeaderBytes, "incompressible payload is stored raw");
}

void Truncated(std::mt19937& rng) {
    const auto src = Sample(rng, 6);
    const auto full = Write(src, true);
    std::vector<uint64_t> ends;
    {
        Reader r;
        r.OpenMemory(full, nullptr);
        for (const ChunkRef& c : r.Chunks()) ends.push_back(c.offset + kChunkHeaderBytes + c.storedLen);
    }
    bool allOk = true;
    for (size_t cut = 0; cut < full.size(); ++cut) {
        std::vector<uint8_t> t(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
        Reader r;
        const bool ok = r.OpenMemory(std::move(t), nullptr);
        size_t whole = 0;
        while (whole < ends.size() - 1 && ends[whole] <= cut) ++whole;
        const bool expectOpen = whole >= 1;
        if (ok != expectOpen || (ok && (r.Complete() || !SameChunks(r, src, whole)))) {
            if (allOk) printf("  cut at %zu: open=%d complete=%d chunks=%zu want %zu\n", cut, ok, r.Complete(), r.Chunks().size(), whole);
            allOk = false;
        }
    }
    Expect(allOk, "every truncation reads the whole chunks before the cut, incomplete");
    const auto open = Write(src, false);
    Reader r;
    Expect(r.OpenMemory(open, nullptr) && !r.Complete() && SameChunks(r, src, src.size()), "abandoned file reads fully, incomplete");
}

// What a crash leaves, by how far the recording got: nothing, the magic, a HEAD cut short, a HEAD and a cut chunk.
void CrashCut(std::mt19937& rng) {
    const auto src = Sample(rng, 2);
    const auto full = Write(src, false);
    std::string err;
    Reader r;
    Expect(!r.OpenMemory({}, &err) && err == "not a .wsr file", "a 0-byte file is not a .wsr file");
    Expect(!r.OpenMemory(std::vector<uint8_t>(full.begin(), full.begin() + 4), &err) && err == "no HEAD chunk",
           "the magic alone: no HEAD chunk");
    Expect(!r.OpenMemory(std::vector<uint8_t>(full.begin(), full.begin() + 4 + kChunkHeaderBytes + 1), &err) &&
               err == "no HEAD chunk",
           "a HEAD chunk cut short: no HEAD chunk");
    Reader first;
    first.OpenMemory(full, nullptr);
    const size_t secondEnd = static_cast<size_t>(first.Chunks()[1].offset) + kChunkHeaderBytes + 1;
    Expect(r.OpenMemory(std::vector<uint8_t>(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(secondEnd)), &err) &&
               !r.Complete() && r.Chunks().size() == 1 && r.Index().empty() && SameChunks(r, src, 1),
           "a HEAD and a cut chunk: opens with the HEAD, incomplete, no index");
}

void BadCrc(std::mt19937& rng) {
    const auto src = Sample(rng, 5);
    const auto full = Write(src, true);
    Reader good;
    good.OpenMemory(full, nullptr);
    const auto chunks = good.Chunks();
    bool allOk = true;
    for (size_t k = 1; k + 1 < chunks.size(); ++k) {
        if (!chunks[k].storedLen) continue;
        auto bad = full;
        bad[static_cast<size_t>(chunks[k].offset + kChunkHeaderBytes + chunks[k].storedLen / 2)] ^= 0x5a;
        Reader r;
        const bool ok = r.OpenMemory(std::move(bad), nullptr);
        if (!ok || r.Complete() || r.Chunks().size() != k || !SameChunks(r, src, k)) allOk = false;
    }
    Expect(allOk, "a bad CRC stops the walk at that chunk, incomplete");
    auto head = full;
    head[kMagicBytes + kChunkHeaderBytes] ^= 1;
    Reader r;
    Expect(!r.OpenMemory(std::move(head), nullptr), "a bad HEAD CRC refuses the file");
    auto trailer = full;
    trailer[trailer.size() - 1] = 'X';
    Reader t;
    Expect(t.OpenMemory(std::move(trailer), nullptr) && !t.Complete(), "a bad trailer reads incomplete");
}

void Versions() {
    auto one = [](const std::string& head, std::string* err) {
        std::vector<uint8_t> out;
        Writer w;
        w.OpenMemory(&out);
        w.Chunk(kHEAD, head.data(), head.size());
        w.Close();
        Reader r;
        return r.OpenMemory(std::move(out), err);
    };
    std::string err;
    Expect(!one(R"({"format":2,"engineHash":1})", &err) && err.find("newer") != std::string::npos, "format 2 is refused");
    Expect(!one(R"({"engineHash":1})", &err), "no format is refused");
    Expect(!one("not json", &err), "a non-JSON HEAD is refused");
    Expect(one(R"({"format":1,"engineHash":2})", &err), "a newer engineHash still opens");
    std::vector<uint8_t> noHead;
    Writer w;
    w.OpenMemory(&noHead);
    w.Chunk(kNOTE, "x", 1);
    w.Close();
    Reader r;
    Expect(!r.OpenMemory(noHead, nullptr), "a file not starting with HEAD is refused");
    Expect(!r.OpenMemory({'W', 'S', 'R'}, nullptr), "a short file is refused");
    Expect(!r.OpenMemory({'W', 'S', 'R', '2'}, nullptr), "a wrong magic is refused");
}

void FileIo() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"wsr", 0, path);
    Writer w;
    Expect(w.Open(path), "open a file");
    const std::string head = R"({"format":1,"engineHash":1})";
    w.Chunk(kHEAD, head.data(), head.size());
    std::vector<uint8_t> ticks(76 * 500, 3);
    w.Chunk(kTICK, ticks.data(), ticks.size(), true, 1, 500);
    Expect(w.Flush(), "flush");
    Expect(w.Close(), "close a file");
    Reader r;
    std::string err;
    Expect(r.OpenFile(path, &err) && r.Complete() && r.Chunks().size() == 3, "read the file back");
    DeleteFileW(path);
}

void Mutation(std::mt19937& rng, double seconds) {
    std::vector<std::vector<uint8_t>> seeds;
    for (int i = 0; i < 8; ++i) seeds.push_back(Write(Sample(rng, 1 + i), i % 3 != 0));
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000);
    uint64_t runs = 0, opened = 0, complete = 0, decoded = 0;
    while (GetTickCount64() < end) {
        for (int batch = 0; batch < 64; ++batch, ++runs) {
            auto d = seeds[rng() % seeds.size()];
            const int edits = 1 + static_cast<int>(rng() % 8);
            for (int e = 0; e < edits && !d.empty(); ++e) {
                const size_t at = rng() % d.size();
                switch (rng() % 6) {
                    case 0: d[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
                    case 1: d[at] = static_cast<uint8_t>(rng()); break;
                    case 2: d.resize(at); break;
                    case 3: d.insert(d.begin() + static_cast<std::ptrdiff_t>(at), static_cast<uint8_t>(rng())); break;
                    case 4: d.erase(d.begin() + static_cast<std::ptrdiff_t>(at)); break;
                    default: {
                        const uint32_t v = rng() % 4 ? static_cast<uint32_t>(rng()) : 0xffffffffu;
                        for (int k = 0; k < 4 && at + k < d.size(); ++k) d[at + k] = static_cast<uint8_t>(v >> (8 * k));
                    }
                }
            }
            Reader r;
            if (r.OpenMemory(std::move(d), nullptr)) {
                ++opened;
                complete += r.Complete();
                if (r.ForEach(0, [](const ChunkRef&, const std::vector<uint8_t>&) {})) ++decoded;
            }
        }
    }
    printf("  mutation: %llu runs in %.0f s, %llu opened, %llu complete, %llu fully decoded\n", runs, seconds, opened,
           complete, decoded);
    Expect(runs > 0, "mutation run");
}
}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? atof(argv[1]) : 60.0;
    std::mt19937 rng(1077);
    RoundTrip(rng);
    Deflates();
    Truncated(rng);
    CrashCut(rng);
    BadCrc(rng);
    Versions();
    FileIo();
    Mutation(rng, seconds);
    if (argc > 2) {
        FILE* f = fopen(argv[2], "wb");
        const auto sample = Write(Sample(rng, 6), true);
        if (f) fwrite(sample.data(), 1, sample.size(), f), fclose(f);
        Expect(f != nullptr, "write the sample file");
    }
    printf("wormsign_format_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
