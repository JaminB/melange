// Offline self-test of MirageTexTags' pure logic (render/mirage/textags_logic.h): the digest scheme (against the
// vectors tools/tag_digest.py prints with --self-test), the tag-table format and merge, and the name/binding tracker.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "render/mirage/textags_logic.h"

namespace {
namespace lg = melange::mirage::textags::logic;
int g_checks = 0, g_failed = 0;

void Check(bool ok, const char* what) {
    ++g_checks;
    if (ok) return;
    ++g_failed;
    std::printf("FAIL: %s\n", what);
}

// tools/tag_digest.py's vector: a 3x2 RGB image, rows top-down, value (x*40 + y*7 + c*3) & 255.
std::vector<uint8_t> VectorTopDown() {
    std::vector<uint8_t> px;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            for (int c = 0; c < 3; ++c) px.push_back(static_cast<uint8_t>((x * 40 + y * 7 + c * 3) & 255));
    return px;
}

// The engine uploads XImage rows bottom-up: GL row 0 = the image's bottom row.
std::vector<uint8_t> Flip(const std::vector<uint8_t>& px, int w, int h, int bpp) {
    std::vector<uint8_t> out;
    for (int y = h - 1; y >= 0; --y) out.insert(out.end(), px.begin() + y * w * bpp, px.begin() + (y + 1) * w * bpp);
    return out;
}

void TestDigest() {
    uint8_t hdr[20];
    lg::Header(hdr, 3, 2, lg::kRGB8, lg::kRGB, lg::kUByte);
    Check(hdr[0] == 3 && hdr[1] == 0 && hdr[4] == 2 && hdr[8] == 0x51 && hdr[9] == 0x80 && hdr[12] == 0x07 && hdr[13] == 0x19 &&
              hdr[16] == 0x01 && hdr[17] == 0x14 && hdr[19] == 0,
          "header: 5 little-endian uint32");

    std::vector<uint8_t> up = Flip(VectorTopDown(), 3, 2, 3);
    uint64_t d = 0;
    Check(lg::Digest(3, 2, lg::kRGB8, lg::kRGB, lg::kUByte, up.data(), 9, 9, &d) && lg::Hex64(d) == "a9daaeca15600965",
          "RGB 3x2 matches tools/tag_digest.py");

    // The same rows with GL_UNPACK_ALIGNMENT 4 padding (stride 12) hash the same: padding is not part of the digest.
    std::vector<uint8_t> padded(24, 0xEE);
    std::copy(up.begin(), up.begin() + 9, padded.begin());
    std::copy(up.begin() + 9, up.end(), padded.begin() + 12);
    uint64_t d2 = 0;
    Check(lg::SourceStride(3, 3, 4, 0) == 12 && lg::Digest(3, 2, lg::kRGB8, lg::kRGB, lg::kUByte, padded.data(), 9, 12, &d2) && d2 == d,
          "row padding is skipped");
    Check(lg::SourceStride(3, 3, 1, 0) == 9 && lg::SourceStride(3, 4, 4, 5) == 20 && lg::SourceStride(1, 2, 8, 0) == 8, "source stride");

    std::vector<uint8_t> rgba(16);
    for (int i = 0; i < 16; ++i) rgba[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
    std::vector<uint8_t> rgbaUp = Flip(rgba, 2, 2, 4);
    Check(lg::Digest(2, 2, lg::kRGBA8, lg::kRGBA, lg::kUByte, rgbaUp.data(), 8, 8, &d) && lg::Hex64(d) == "9a467ea8b3f1fb7e",
          "RGBA 2x2 matches tools/tag_digest.py");

    uint8_t dxt[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    Check(lg::DigestCompressed(4, 4, 0x83F1, dxt, 8, &d) && lg::Hex64(d) == "31374b8dfc3bf1ee", "compressed level: format = type = 0");

    // Every header field is part of the key.
    uint64_t a = 0, b = 0, c = 0;
    lg::Digest(3, 2, lg::kRGB8, lg::kRGB, lg::kUByte, up.data(), 9, 9, &a);
    lg::Digest(3, 2, lg::kRGBA8, lg::kRGB, lg::kUByte, up.data(), 9, 9, &b);
    lg::Digest(2, 3, lg::kRGB8, lg::kRGB, lg::kUByte, up.data(), 6, 6, &c);
    Check(a != b && a != c, "internal format and size change the digest");
    Check(!lg::Digest(0, 2, lg::kRGB8, lg::kRGB, lg::kUByte, up.data(), 9, 9, &d) &&
              !lg::Digest(3, 2, lg::kRGB8, lg::kRGB, lg::kUByte, nullptr, 9, 9, &d) &&
              !lg::Digest(3, 2, lg::kRGB8, lg::kRGB, lg::kUByte, up.data(), 9, 8, &d),
          "empty, null or overlapping rows are refused");

    Check(lg::BytesPerPixel(lg::kRGB, lg::kUByte) == 3 && lg::BytesPerPixel(lg::kRGBA, lg::kUByte) == 4 &&
              lg::BytesPerPixel(lg::kRGBA, 0x8366) == 2 && lg::BytesPerPixel(0x1906, lg::kUByte) == 1 &&
              lg::BytesPerPixel(lg::kRGB, 0x1406 /*FLOAT*/) == 0,
          "bytes per pixel");
    uint32_t f = 0, t = 0;
    Check(lg::ReadbackFormat(lg::kRGB8, &f, &t) && f == lg::kRGB && t == lg::kUByte && lg::ReadbackFormat(lg::kRGBA8, &f, &t) &&
              f == lg::kRGBA && !lg::ReadbackFormat(lg::kRGBA, &f, &t),
          "read-back formats");
}

void TestHex() {
    uint64_t v = 0;
    Check(lg::ParseHex64("a9daaeca15600965", &v) && v == 0xa9daaeca15600965ull && lg::Hex64(v) == "a9daaeca15600965", "hex round trip");
    Check(lg::ParseHex64("A9DAAECA15600965", &v) && v == 0xa9daaeca15600965ull, "upper case accepted");
    Check(!lg::ParseHex64("a9daaeca1560096", &v) && !lg::ParseHex64("a9daaeca156009650", &v) && !lg::ParseHex64("g9daaeca15600965", &v),
          "bad hex refused");
    Check(lg::Hex64(1) == "0000000000000001", "leading zeros kept");
}

std::string TableText(const std::string& tags, const std::string& digest = lg::kScheme, int version = 1) {
    return "{\"version\": " + std::to_string(version) + ", \"digest\": \"" + digest + "\", \"tags\": {" + tags + "}}";
}

void TestTable() {
    lg::Table t;
    std::string err;
    Check(lg::ParseTableText(TableText(R"("a9daaeca15600965": [1, 0.5, -2, 3, 4, 5, 6, 7], "0000000000000001": [9])"), &t, &err),
          "valid table parses");
    Check(t.tags.size() == 2 && t.index.count(0xa9daaeca15600965ull) && t.tags[t.index[0xa9daaeca15600965ull]][2] == -2.f &&
              t.tags[t.index[1]][0] == 9.f && t.tags[t.index[1]][7] == 0.f,
          "rows read, short rows padded with 0");
    struct Bad { std::string text; const char* what; };
    const Bad bad[] = {
        {TableText("", lg::kScheme, 2), "version 2"},
        {TableText("", "sha1-64"), "another digest scheme"},
        {TableText(R"("a9daaeca1560096": [1])"), "15-digit key"},
        {TableText(R"("a9daaeca15600965": [])"), "empty row"},
        {TableText(R"("a9daaeca15600965": [1,2,3,4,5,6,7,8,9])"), "9 numbers"},
        {TableText(R"("a9daaeca15600965": [1, "x"])"), "a string value"},
        {TableText(R"("a9daaeca15600965": 3)"), "a number row"},
        {TableText(R"("a9daaeca15600965": [1], "A9DAAECA15600965": [2])"), "the same digest twice"},
        {"{\"version\": 1, \"digest\": \"gl-level0-sha256-64\"}", "no tags"},
        {"[1]", "not an object"},
        {"{", "not JSON"},
    };
    for (const Bad& b : bad) {
        lg::Table x;
        err.clear();
        Check(!lg::ParseTableText(b.text, &x, &err) && !err.empty(), b.what);
    }

    lg::Table a, bt, merged;
    lg::ParseTableText(TableText(R"("0000000000000001": [1], "0000000000000002": [2])"), &a, &err);
    lg::ParseTableText(TableText(R"("0000000000000002": [20], "0000000000000003": [30])"), &bt, &err);
    size_t dupes = 9;
    Check(lg::Merge(&merged, a, "first", &dupes) == 2 && dupes == 0, "merge into empty");
    Check(lg::Merge(&merged, bt, "second", &dupes) == 1 && dupes == 1, "merge counts the duplicate");
    Check(merged.tags.size() == 3 && merged.tags[merged.index[2]][0] == 2.f && merged.tags[merged.index[3]][0] == 30.f,
          "the first declared table wins a digest");
    Check(merged.sources.size() == 2 && merged.sources[0] == "first", "sources in merge order");
}

void TestTracker() {
    lg::Table t;
    std::string err;
    lg::ParseTableText(TableText(R"("0000000000000011": [1, 2], "0000000000000022": [3])"), &t, &err);
    lg::Tracker tr;
    Check(tr.Bound(0) == lg::Tracker::kUnknown, "bindings unknown before the state is known");
    tr.ResetBindings(true);
    Check(tr.Bound(0) == 0 && tr.Bound(5) == 0 && tr.ActiveKnown(), "hooked from the start: all units have texture 0");
    tr.Bind(7);
    tr.ActiveTexture(3);
    tr.Bind(9);
    Check(tr.Bound(0) == 7 && tr.Bound(3) == 9 && tr.Active() == 3, "binds land on the active unit");
    tr.ActiveTexture(99);
    tr.Bind(4);
    Check(!tr.ActiveKnown() && tr.Bound(0) == lg::Tracker::kUnknown && tr.Bound(3) == lg::Tracker::kUnknown,
          "a bind on an untracked unit forgets the bindings instead of landing on the last tracked unit");
    tr.ResetBindings(true);
    tr.Bind(7);
    tr.ActiveTexture(3);
    tr.Bind(9);

    uint32_t g0 = tr.Gen();
    tr.Hashed(7, 0x11, 64, 32, t);
    tr.Hashed(9, 0x99, 8, 8, t);
    const lg::Tracker::Entry* e7 = tr.Find(7);
    const lg::Tracker::Entry* e9 = tr.Find(9);
    Check(e7 && e7->hashed && e7->row >= 0 && t.tags[static_cast<size_t>(e7->row)][1] == 2.f && e7->w == 64 && e7->h == 32,
          "a hashed name gets its row");
    Check(e9 && e9->hashed && e9->row == -1, "a digest not in the table stays untagged");
    Check(tr.Gen() != g0, "hashing bumps the generation");

    tr.Dynamic(7);
    Check(tr.Find(7) && tr.Find(7)->dynamic && tr.Find(7)->row == -1 && !tr.Find(7)->hashed, "re-specified outside the loader: untagged");
    tr.Hashed(7, 0x11, 64, 32, t);

    tr.ActiveTexture(0);
    tr.Bind(7);
    tr.ActiveTexture(2);
    tr.Bind(7);
    uint32_t g1 = tr.Gen();
    tr.Delete(7);
    Check(!tr.Find(7) && tr.Bound(0) == 0 && tr.Bound(2) == 0 && tr.Gen() != g1, "glDeleteTextures drops the name and its bindings");
    tr.Bind(7);  // the name comes back from glGenTextures
    Check(!tr.Find(7) && tr.Bound(2) == 7, "a reused name starts unknown");

    tr.Hashed(7, 0x22, 4, 4, t);
    tr.Forget(7);
    Check(!tr.Find(7) && tr.Bound(2) == 7, "Forget keeps the binding");

    // A new table re-keys every hashed name.
    tr.Hashed(9, 0x99, 8, 8, t);
    lg::Table t2;
    lg::ParseTableText(TableText(R"("0000000000000099": [5])"), &t2, &err);
    uint32_t g2 = tr.Gen();
    tr.Retag(t2);
    Check(tr.Find(9)->row == 0 && tr.Gen() != g2, "Retag picks up a new table");
    tr.Retag(lg::Table{});
    Check(tr.Find(9)->row == -1, "an empty table untags everything");

    lg::Tracker live;
    live.ResetBindings(false);
    live.Bind(5);
    Check(live.Bound(0) == lg::Tracker::kUnknown, "with the active unit unknown, a bind leaves every unit unknown");
    live.ActiveTexture(1);
    live.Bind(5);
    Check(live.Bound(1) == 5 && live.Bound(0) == lg::Tracker::kUnknown, "once the active unit is known, binds are tracked");
    live.Hashed(0, 0x11, 1, 1, t);
    Check(live.Size() == 0, "texture 0 is never recorded");
}

// Not a pass/fail check: what hashing costs at level load (the inventory's 712 textures are about 35 MB).
void Timing() {
    std::vector<uint8_t> big(35u << 20);
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 2654435761u >> 24);
    auto t0 = std::chrono::steady_clock::now();
    uint64_t d = 0;
    bool ok = true;
    // As 560 textures of 256x256 RGB (192 KB each).
    const size_t one = 256 * 256 * 3;
    for (size_t off = 0; off + one <= big.size(); off += one) ok &= lg::Digest(256, 256, lg::kRGB8, lg::kRGB, lg::kUByte, big.data() + off, 768, 768, &d);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Check(ok, "timing run hashed");
    std::printf("textags: hashing 35 MB as 256x256 RGB textures took %.1f ms\n", ms);
}
}  // namespace

int main() {
    TestDigest();
    TestHex();
    TestTable();
    TestTracker();
    Timing();
    std::printf("textags: %d/%d checks passed\n", g_checks - g_failed, g_checks);
    return g_failed ? 1 : 0;
}
