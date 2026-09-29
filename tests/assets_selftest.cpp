// Offline self-test for Component C's runtime enforcement layer (no game): the CRC-collision check, the loose-root
// naming rule, the bank path safety and size cap, and the panel-icon pixel math (downscale + atlas placement).
// Exit code 0 = all passed.
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "assets/banks.h"
#include "assets/crcsafe.h"
#include "assets/icons.h"
#include "assets/roots.h"

namespace crcsafe = melange::assets::crcsafe;
namespace roots = melange::assets::roots;
namespace banks = melange::assets::banks;
namespace icons = melange::assets::icons;

namespace {
int g_fail = 0, g_pass = 0;
std::wstring g_root;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

std::wstring TempDir(const wchar_t* name) {
    std::wstring d = g_root + L"\\" + name;
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

void WriteFile_(const std::wstring& path, const void* data, size_t n) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

std::vector<crcsafe::Entry> FakeCrcTable() {
    return {{"Tweak/WEAPTWK.XOM", 0x11111111}, {"lang/english.lub", 0x22222222}};
}

// ---------------------------------------------------------------------------------------------
// crcsafe: bare-filename, case-insensitive collision.
// ---------------------------------------------------------------------------------------------
void TestCrcsafe() {
    const auto table = FakeCrcTable();
    Expect(crcsafe::Collides(table, "weaptwk.xom"), "crcsafe: matches the listed name case-insensitively");
    Expect(crcsafe::Collides(table, "WEAPTWK.XOM"), "crcsafe: matches the listed name exactly");
    Expect(!crcsafe::Collides(table, "megabazooka.xom"), "crcsafe: an unrelated name does not collide");
    Expect(crcsafe::Collides(table, "some/dir/WeapTwk.xom"), "crcsafe: compares the file name, not the full path");
}

// ---------------------------------------------------------------------------------------------
// roots: the "<modId>.*" naming rule and the CRC-collision refusal.
// ---------------------------------------------------------------------------------------------
void TestRootsNaming() {
    const auto table = FakeCrcTable();
    std::string err;
    Expect(roots::CheckNames("mega-bazooka", {"mega-bazooka.hud.tga", "mega-bazooka.tex.tga"}, table, &err),
           "roots: every file named '<modId>.*' is accepted");

    err.clear();
    Expect(!roots::CheckNames("mega-bazooka", {"bazooka.tga"}, table, &err) &&
               err.find("is not named") != std::string::npos,
           "roots: a file not named '<modId>.*' is refused");

    err.clear();
    std::vector<crcsafe::Entry> tricky = {{"mega-bazooka.evil.tga", 1}};
    Expect(!roots::CheckNames("mega-bazooka", {"mega-bazooka.evil.tga"}, tricky, &err) &&
               err.find("collides") != std::string::npos,
           "roots: a correctly named file can still collide with a listed one");
}

// roots::Add and banks::Load (the engine-touching glue in roots.cpp/banks_load.cpp) are exercised only in the
// game, by the private accept.ps1 script: off the known build, crcsafe::Available() is false and both fail closed
// by inspection (the `if (!crcsafe::Available())` guard ahead of every other check in each function).

// ---------------------------------------------------------------------------------------------
// banks: path safety and the size cap.
// ---------------------------------------------------------------------------------------------
void TestBanksCheckPath() {
    const auto table = FakeCrcTable();
    const std::wstring dir = TempDir(L"data");
    const std::string small(64, 'a');
    WriteFile_(dir + L"\\one.xom", small.data(), small.size());
    std::string err;
    Expect(banks::CheckPath(dir, "one.xom", table, &err), "banks: a real, small .xom under data/ is accepted");

    err.clear();
    Expect(!banks::CheckPath(dir, "../Tweak/WEAPTWK.XOM", table, &err) && err.find("..") != std::string::npos,
           "banks: '..' is refused");

    err.clear();
    Expect(!banks::CheckPath(dir, "C:\\Windows\\one.xom", table, &err), "banks: an absolute path is refused");

    err.clear();
    Expect(!banks::CheckPath(dir, "one.txt", table, &err), "banks: a non-.xom extension is refused");

    err.clear();
    Expect(!banks::CheckPath(dir, "missing.xom", table, &err), "banks: a file that doesn't exist is refused");

    err.clear();
    Expect(!banks::CheckPath(dir, "one.xom", {{"one.xom", 1}}, &err) && err.find("collides") != std::string::npos,
           "banks: a CRC-listed file name is refused even if it exists and is small");
}

void TestBanksSizeCap() {
    const std::wstring dir = TempDir(L"data-big");
    const std::wstring path = dir + L"\\big.xom";
    FILE* f = _wfopen(path.c_str(), L"wb");
    Expect(f != nullptr, "banks: could create the oversized fixture");
    if (f) {
        // Sparse-seek to 64 MiB + 1 byte rather than writing it, to keep the self-test fast.
        _fseeki64(f, (64LL << 20), SEEK_SET);
        fputc('a', f);
        fclose(f);
    }
    std::string err;
    Expect(!banks::CheckPath(dir, "big.xom", {}, &err) && err.find("64 MiB") != std::string::npos,
           "banks: a file over 64 MiB is refused");
}

// ---------------------------------------------------------------------------------------------
// icons: the box filter and the bottom-up, flipped, alpha-blended atlas write.
// ---------------------------------------------------------------------------------------------
void TestIconsDownscale() {
    // A 128x128 image, left half opaque red, right half opaque blue: each 64x64 quadrant of the downscale should
    // average to its own half's colour exactly (uniform blocks), and each output pixel keeps alpha 255.
    std::vector<uint8_t> src(128 * 128 * 4);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x) {
            uint8_t* p = &src[(y * 128 + x) * 4];
            if (x < 64) {
                p[0] = 255;
                p[1] = 0;
                p[2] = 0;
            } else {
                p[0] = 0;
                p[1] = 0;
                p[2] = 255;
            }
            p[3] = 255;
        }
    uint8_t out[64 * 64 * 4];
    std::string err;
    Expect(icons::Downscale(src.data(), 128, 128, out, &err), "icons: downscale of a valid 128x128 image succeeds");
    const uint8_t* left = &out[(10 * 64 + 5) * 4];
    const uint8_t* right = &out[(10 * 64 + 60) * 4];
    Expect(left[0] == 255 && left[2] == 0, "icons: the left half downscales to pure red");
    Expect(right[0] == 0 && right[2] == 255, "icons: the right half downscales to pure blue");
    Expect(left[3] == 255 && right[3] == 255, "icons: alpha is preserved");

    err.clear();
    Expect(!icons::Downscale(src.data(), 100, 100, out, &err), "icons: a side that isn't a multiple of 64 is refused");
    Expect(!icons::Downscale(src.data(), 64, 128, out, &err), "icons: a non-square image is refused");
}

void TestIconsWriteSubIcon() {
    std::vector<uint8_t> atlas(256 * 256 * 3, 0x40);  // a mid-grey atlas, as if already uploaded once
    std::vector<uint8_t> icon(64 * 64 * 4, 0);
    // Opaque white in the icon's top-left corner (row 0, col 0 in PNG/top-down order) only.
    for (int c = 0; c < 3; ++c) icon[c] = 255;
    icon[3] = 255;

    Expect(icons::WriteSubIcon(atlas.data(), atlas.size(), 12, icon.data()), "icons: WriteSubIcon accepts sub-icon 12");
    // Sub-icon 12 -> row 3-12/4=0, col 0: the icon's top-left pixel must land at the TOP of that cell (row-flipped),
    // i.e. atlas row 63, col 0 -- not atlas row 0 (which would be the icon's bottom-left, unflipped).
    const uint8_t* top = &atlas[(63 * 256 + 0) * 3];
    const uint8_t* bottom = &atlas[(0 * 256 + 0) * 3];
    Expect(top[0] == 255 && top[1] == 255 && top[2] == 255, "icons: the icon's top-left corner lands row-flipped, at the cell's top");
    Expect(bottom[0] == 0x40, "icons: the cell's bottom-left corner is untouched (icon's own bottom-left is black)");

    // Nothing outside sub-icon 12's 64x64 region (row band 0, column band 0) changed.
    bool untouched = true;
    for (int y = 0; y < 256 && untouched; ++y)
        for (int x = 0; x < 256 && untouched; ++x) {
            if (y < 64 && x < 64) continue;  // sub-icon 12's own cell
            const uint8_t* p = &atlas[(y * 256 + x) * 3];
            if (p[0] != 0x40 || p[1] != 0x40 || p[2] != 0x40) untouched = false;
        }
    Expect(untouched, "icons: WriteSubIcon touches only its own 64x64 cell");

    Expect(!icons::WriteSubIcon(atlas.data(), atlas.size(), 8, icon.data()), "icons: sub-icon 8 (in use by vanilla) is refused");
    Expect(!icons::WriteSubIcon(atlas.data(), 100, 12, icon.data()), "icons: a wrong-sized atlas buffer is refused");
}

void TestIconsAlphaBlend() {
    std::vector<uint8_t> atlas(256 * 256 * 3, 0);
    std::vector<uint8_t> icon(64 * 64 * 4, 0);
    for (auto& p : icon) p = 0;
    // Half-alpha white everywhere.
    for (int i = 0; i < 64 * 64; ++i) {
        icon[i * 4 + 0] = 255;
        icon[i * 4 + 1] = 255;
        icon[i * 4 + 2] = 255;
        icon[i * 4 + 3] = 128;
    }
    icons::WriteSubIcon(atlas.data(), atlas.size(), 9, icon.data());
    const uint8_t* p = &atlas[(64 * 256 + 64) * 3];  // sub-icon 9's cell (row 1, col 1)
    Expect(p[0] > 100 && p[0] < 155, "icons: a half-alpha icon blends toward its colour, not a flat overwrite");
}
}  // namespace

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t dir[MAX_PATH];
    swprintf(dir, MAX_PATH, L"%lsassets_selftest_%lu", tmp, GetTickCount());
    CreateDirectoryW(dir, nullptr);
    g_root = dir;

    TestCrcsafe();
    TestRootsNaming();
    TestBanksCheckPath();
    TestBanksSizeCap();
    TestIconsDownscale();
    TestIconsWriteSubIcon();
    TestIconsAlphaBlend();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
