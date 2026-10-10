// Offline self-test for mod-asset safety (no game): the CRC-collision check, the loose-root naming rule, the bank
// path safety and size cap, and the panel-icon pixel math (downscale + atlas placement). Exit code 0 = all passed.
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "assets/banks.h"
#include "assets/crcsafe.h"
#include "assets/icons.h"
#include "assets/roots.h"
#include "xom/xom.h"

namespace crcsafe = melange::assets::crcsafe;
namespace roots = melange::assets::roots;
namespace banks = melange::assets::banks;
namespace icons = melange::assets::icons;
namespace xom = melange::xom;

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

std::vector<uint8_t> FakeImage(int entries, uint32_t firstCrc) {
    constexpr uint32_t kBase = 0x400000, kSecRva = 0x522000, kRaw = 0x400, kStrRva = 0x523000;
    std::vector<uint8_t> b(kRaw + 0x4000, 0);
    auto put32 = [&](size_t off, uint32_t v) { std::memcpy(b.data() + off, &v, 4); };
    auto put16 = [&](size_t off, uint16_t v) { std::memcpy(b.data() + off, &v, 2); };
    put16(0, 0x5a4d);
    put32(0x3c, 0x80);
    put32(0x80, 0x4550);
    put16(0x80 + 6, 1);
    put16(0x80 + 20, 0xe0);
    put16(0x80 + 24, 0x10b);
    put32(0x80 + 24 + 28, kBase);
    const size_t sec = 0x80 + 24 + 0xe0;
    put32(sec + 8, 0x4000);
    put32(sec + 12, kSecRva);
    put32(sec + 16, 0x4000);
    put32(sec + 20, kRaw);
    size_t str = kRaw + (kStrRva - kSecRva);
    for (int i = 0; i < entries; ++i) {
        const std::string name = "Data/Tweak/file" + std::to_string(i) + ".xom";
        const size_t at = kRaw + (crcsafe::kTableVa - kBase - kSecRva) + 8u * i;
        put32(at, static_cast<uint32_t>(kBase + kSecRva + (str - kRaw)));
        put32(at + 4, i ? 0x1000u + i : firstCrc);
        std::memcpy(b.data() + str, name.c_str(), name.size() + 1);
        str += name.size() + 1;
    }
    return b;
}

void TestCrcsafeImage() {
    std::vector<crcsafe::Entry> t;
    Expect(crcsafe::ParseImage(FakeImage(crcsafe::kExpectedCount, crcsafe::kFirstCrc), &t) &&
               t.size() == static_cast<size_t>(crcsafe::kExpectedCount) && t[3].path == "Data/Tweak/file3.xom" &&
               crcsafe::Collides(t, "FILE88.XOM"),
           "crcsafe: the table reads from an exe image");
    Expect(!crcsafe::ParseImage(FakeImage(crcsafe::kExpectedCount, 0x1234), &t) && t.empty(),
           "crcsafe: an image whose first entry differs is refused");
    Expect(!crcsafe::ParseImage(FakeImage(crcsafe::kExpectedCount - 1, crcsafe::kFirstCrc), &t),
           "crcsafe: an image with another entry count is refused");
    std::vector<uint8_t> junk(64, 0x5a);
    Expect(!crcsafe::ParseImage(junk, &t), "crcsafe: a file that is not a PE image is refused");
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
// banks: the names an XDataBank declares (BankResourceNames), which banks_load.cpp checks against the live
// engine before ever calling LoadBank. A from-scratch fixture, built by hand from the field lists in
// src/xom/xom_schema.inc (XDataBank: Section then the 8 *Resources lists; XContainerResourceDetails' own Value
// first, most-derived first, then XResourceDetails' Name and Flags), mirroring tools/xom/xomtool.py's reader for
// the same classes.
// ---------------------------------------------------------------------------------------------
std::array<uint8_t, 16> HexGuid(const char* hex) {
    std::array<uint8_t, 16> g{};
    for (int i = 0; i < 16; ++i) {
        auto v = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        g[size_t(i)] = uint8_t((v(hex[2 * i]) << 4) | v(hex[2 * i + 1]));
    }
    return g;
}
xom::TypeEntry MakeType(const char* cls) {
    xom::TypeEntry t;
    t.name = cls;
    t.guid = HexGuid(xom::findClass(cls)->guid);
    std::string padded = t.name;
    padded.resize(32, '\0');
    std::memcpy(t.rawName.data(), padded.data(), 32);
    return t;
}
xom::Value RefArray(std::initializer_list<uint32_t> refs = {}) {
    xom::Value v;
    v.type = xom::Type::Ref;
    v.array = true;
    for (auto r : refs) {
        xom::Value item;
        item.type = xom::Type::Ref;
        item.bits = r;
        v.items.push_back(item);
    }
    return v;
}
xom::Object MakeDetails(const std::string& name, uint32_t valueRef) {
    xom::Object o;
    o.type = "XContainerResourceDetails";
    xom::Value nameV;
    nameV.type = xom::Type::String;
    nameV.str = name;
    xom::Value flagsV;
    flagsV.type = xom::Type::U32;
    xom::Value valueV;
    valueV.type = xom::Type::Ref;
    valueV.bits = valueRef;
    o.fields = {{"Value", valueV}, {"Name", nameV}, {"Flags", flagsV}};
    return o;
}
xom::Object MakeBank(std::initializer_list<uint32_t> containerRefs) {
    xom::Object o;
    o.type = "XDataBank";
    xom::Value section;
    section.type = xom::Type::U8;
    o.fields = {{"Section", section},
                {"IntResources", RefArray()},          {"UintResources", RefArray()},
                {"StringResources", RefArray()},        {"FloatResources", RefArray()},
                {"VectorResources", RefArray()},        {"ContainerResources", RefArray(containerRefs)},
                {"StringTableResources", RefArray()},   {"ColorResources", RefArray()}};
    return o;
}
std::vector<uint8_t> BuildBank(std::vector<xom::Object> objects) {
    xom::Document doc;
    doc.types = {MakeType("XContainerResourceDetails"), MakeType("XDataBank")};
    doc.objects = std::move(objects);
    doc.root = uint32_t(doc.objects.size());
    for (auto& o : doc.objects)
        for (auto& t : doc.types) t.count += (t.className() == o.type);
    std::vector<uint8_t> bytes;
    std::string err;
    if (!xom::serialize(doc, bytes, &err)) printf("FAIL: BuildBank fixture: %s\n", err.c_str());
    return bytes;
}

void TestBankResourceNames() {
    std::string err;
    std::vector<std::string> names;
    Expect(!banks::BankResourceNames({}, &names, &err), "bank names: empty bytes are refused");

    // A valid document (two details entries referencing each other, since the target class is unused here) with no
    // XDataBank at all.
    {
        auto bytes = BuildBank({MakeDetails("kWeaponSomething", 1)});
        names.clear();
        Expect(!bytes.empty() && !banks::BankResourceNames(bytes, &names, &err) && err.find("XDataBank") != std::string::npos,
               "bank names: a file with no XDataBank is refused (" + err + ")");
    }
    {
        auto d1 = MakeDetails("kWeaponMegaBazooka", 0);
        auto d2 = MakeDetails("kWeaponBazooka", 0);
        auto bank = MakeBank({1, 2});
        auto bytes = BuildBank({d1, d2, bank});
        names.clear();
        Expect(!bytes.empty() && banks::BankResourceNames(bytes, &names, &err) && names.size() == 2 &&
                   names[0] == "kWeaponMegaBazooka" && names[1] == "kWeaponBazooka",
               "bank names: every ContainerResources entry's Name, in order (" + err + ")");
    }
    {
        // A bank with no resource entries at all: an empty, valid result.
        auto bank = MakeBank({});
        auto bytes = BuildBank({bank});
        names.clear();
        Expect(!bytes.empty() && banks::BankResourceNames(bytes, &names, &err) && names.empty(),
               "bank names: an empty bank names nothing");
    }
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

    Expect(!icons::WriteSubIcon(atlas.data(), atlas.size(), 16, icon.data()) && !icons::WriteSubIcon(atlas.data(), atlas.size(), -1, icon.data()),
           "icons: sub-icons outside 0..15 are refused");
    Expect(!icons::WriteSubIcon(atlas.data(), 100, 12, icon.data()), "icons: a wrong-sized atlas buffer is refused");
}

// Replacing a vanilla weapon's own sub-icon (spice.json "weaponIcons"): any of the 16 sub-icons, snapshot and restore.
void TestIconsVanillaSubIcon() {
    std::vector<uint8_t> atlas(256 * 256 * 3);
    for (size_t i = 0; i < atlas.size(); ++i) atlas[i] = static_cast<uint8_t>(i * 7 + 3);  // a recognisable "vanilla" atlas
    const std::vector<uint8_t> vanilla = atlas;
    std::vector<uint8_t> icon(64 * 64 * 4, 0);
    for (int i = 0; i < 64 * 64; ++i) {
        icon[i * 4] = 200;
        icon[i * 4 + 1] = 100;
        icon[i * 4 + 2] = 50;
        icon[i * 4 + 3] = 255;
    }
    // Atlas 1 sub 3 -> row 3-3/4=3, column 3: the top-right cell of the bottom-up buffer.
    Expect(icons::WriteSubIcon(atlas.data(), atlas.size(), 3, icon.data()), "icons: WriteSubIcon accepts sub-icon 3 of atlas 1");
    const uint8_t* p = &atlas[((3 * 64 + 10) * 256 + (3 * 64 + 10)) * 3];
    Expect(p[0] == 200 && p[1] == 100 && p[2] == 50, "icons: an opaque icon replaces sub-icon 3's pixels");
    bool others = true;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            if (y >= 192 && x >= 192) continue;  // sub-icon 3's own cell
            for (int c = 0; c < 3; ++c) others &= atlas[(y * 256 + x) * 3 + c] == vanilla[(y * 256 + x) * 3 + c];
        }
    Expect(others, "icons: the other 15 sub-icons keep their vanilla pixels");
    Expect(icons::WriteSubIcon(atlas.data(), atlas.size(), 0, icon.data()) && icons::WriteSubIcon(atlas.data(), atlas.size(), 15, icon.data()),
           "icons: the first and last sub-icons are in range");

    // Snapshot of the vanilla pixels, then put back.
    std::vector<uint8_t> keep(icons::kSubBytes);
    std::vector<uint8_t> fresh = vanilla;
    Expect(icons::ReadSubIcon(fresh.data(), fresh.size(), 3, keep.data()), "icons: ReadSubIcon takes a sub-icon");
    Expect(icons::WriteSubIcon(fresh.data(), fresh.size(), 3, icon.data()) && fresh != vanilla, "icons: the patch changes the atlas");
    Expect(icons::RestoreSubIcon(fresh.data(), fresh.size(), 3, keep.data()) && fresh == vanilla,
           "icons: RestoreSubIcon brings the vanilla pixels back exactly");
    // Restoring over an atlas that was never patched changes nothing (a freshly built atlas on the next upload).
    std::vector<uint8_t> again = vanilla;
    icons::RestoreSubIcon(again.data(), again.size(), 3, keep.data());
    Expect(again == vanilla, "icons: restoring over a pristine atlas is a no-op");
    // Bounds.
    Expect(!icons::ReadSubIcon(fresh.data(), fresh.size(), 16, keep.data()) && !icons::ReadSubIcon(fresh.data(), fresh.size(), -1, keep.data()) &&
               !icons::ReadSubIcon(fresh.data(), 100, 3, keep.data()) && !icons::ReadSubIcon(nullptr, fresh.size(), 3, keep.data()),
           "icons: ReadSubIcon refuses a bad sub-icon, size or buffer");
    Expect(!icons::RestoreSubIcon(fresh.data(), fresh.size(), 16, keep.data()) && !icons::RestoreSubIcon(fresh.data(), 100, 3, keep.data()) &&
               !icons::RestoreSubIcon(fresh.data(), fresh.size(), 3, nullptr),
           "icons: RestoreSubIcon refuses a bad sub-icon, size or buffer");
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
    TestCrcsafeImage();
    TestRootsNaming();
    TestBanksCheckPath();
    TestBanksSizeCap();
    TestBankResourceNames();
    TestIconsDownscale();
    TestIconsVanillaSubIcon();
    TestIconsWriteSubIcon();
    TestIconsAlphaBlend();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
