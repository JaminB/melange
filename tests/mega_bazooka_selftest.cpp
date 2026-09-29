// Offline self-test for the shipped sample: dist/Mods/mega-bazooka. No game needed.
//  1. spice.json parses as a content mod with exactly the clone the docs describe (weapons/manifest.cpp,
//     the same code Thumper runs at rescan).
//  2. sim/main.lua compiles under a real Lua 5.0.1 (the dialect the match VM runs); a syntax mistake such as
//     "#" or "%" would fail here before it ever reached the game.
//  3. The shipped panel icon decodes as a 64x64 RGBA PNG, and the HUD icon as a 64x64 32bpp TGA, matching the
//     manifest's own size rules; no assets/data/*.xom bank ships (vanilla meshes only).
extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include <windows.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"

#include "mods/spice.h"
#include "weapons/fields.h"
#include "weapons/manifest.h"

namespace wm = melange::weapons::manifest;
using melange::weapons::FieldType;

namespace {
int g_fail = 0, g_pass = 0;

void Expect(bool ok, const std::string& what) {
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
        printf("FAIL: %s\n", what.c_str());
    }
}

std::wstring W(const char* s) {
    std::wstring out;
    while (*s) out.push_back(static_cast<wchar_t>(*s++));
    return out;
}

std::vector<uint8_t> ReadBytes(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

const wm::SetValue* Find(const wm::CloneDecl& d, const char* field) {
    for (auto& s : d.set)
        if (s.field == field) return &s;
    return nullptr;
}
}  // namespace

int main() {
    const std::wstring modDir = W(MELANGE_SOURCE_DIR) + L"\\dist\\Mods\\mega-bazooka";

    // 1. spice.json + the weapons array.
    melange::spice::Manifest m;
    std::vector<melange::spice::Error> perrs;
    const bool parsed = melange::spice::Parse(modDir, &m, &perrs);
    Expect(parsed, "spice.json parses");
    Expect(m.content, "kind: content");
    Expect(m.defaultEnabled == false, "ships disabled");
    Expect(!m.entrySim.empty(), "has entry.sim");
    Expect(m.weapons.size() == 1, "one weapon declared");

    if (parsed) {
        std::vector<wm::Error> merrs;
        auto decls = wm::Parse(m, &merrs);
        Expect(decls.size() == 1, "the manifest parser accepts it");
        if (decls.size() == 1) {
            const auto& d = decls[0];
            Expect(d.name == "kWeaponMegaBazooka", "clone name");
            Expect(d.baseId == 1 && d.base == "kWeaponBazooka", "base is the proven Bazooka (id 1)");
            Expect(d.cell == 29, "cell 29 (a free panel cell)");
            Expect(d.bank.empty(), "no mesh bank shipped");
            Expect(d.text.name == "Mega Bazooka" && !d.text.help.empty(), "panel text");
            Expect(d.panelIcon == "icons/megabazooka.png", "panelIcon path");
            Expect(d.hudIcon == "mega-bazooka.hud.tga", "hudIcon path, named <id>.*");

            const auto* mesh = Find(d, "PayloadGraphicsResourceID");
            Expect(mesh && mesh->type == FieldType::String && mesh->string == "Grenade.Payload",
                   "PayloadGraphicsResourceID names a real vanilla mesh");
            const auto* scale = Find(d, "Scale");
            Expect(scale && scale->type == FieldType::F32 && scale->number == 10, "Scale is 10");
            const auto* dmg = Find(d, "WormDamageMagnitude");
            Expect(dmg && dmg->type == FieldType::F32 && dmg->number == 120, "damage is set (schema-typed F32)");
            const auto* sfx = Find(d, "LaunchSfx");
            Expect(sfx && sfx->type == FieldType::String && sfx->string == "weapons/SheepBaa",
                   "LaunchSfx is set");
        }
    }

    // No custom mesh bank ships: banks cannot carry meshes.
    Expect(!FileExists(modDir + L"\\assets\\data\\megabazooka.xom"), "no assets/data mesh bank is shipped");

    // 2. The sim script compiles under the real Lua 5.0 dialect the match VM uses.
    const auto lua = ReadBytes(modDir + L"\\sim\\main.lua");
    Expect(!lua.empty(), "sim/main.lua is not empty");
    lua_State* L = lua_open();
    const int rc = luaL_loadbuffer(L, reinterpret_cast<const char*>(lua.data()), lua.size(), "main.lua");
    if (rc != 0) printf("  lua error: %s\n", lua_tostring(L, -1));
    Expect(rc == 0, "sim/main.lua compiles under Lua 5.0.1 (float numbers)");
    lua_close(L);

    // 3. Panel icon: 64x64 RGBA PNG.
    const auto png = ReadBytes(modDir + L"\\assets\\icons\\megabazooka.png");
    Expect(!png.empty(), "panel icon file exists");
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &ch, 4);
    Expect(pixels != nullptr, "panel icon decodes");
    Expect(w == 64 && h == 64, "panel icon is 64x64");
    if (pixels) stbi_image_free(pixels);

    // HUD icon: 64x64 32bpp uncompressed TGA (the 18-byte header, per the naming and format rule).
    const auto tga = ReadBytes(modDir + L"\\assets\\loose\\mega-bazooka.hud.tga");
    Expect(tga.size() > 18, "HUD icon file exists");
    if (tga.size() > 18) {
        const int tw = tga[12] | (tga[13] << 8);
        const int th = tga[14] | (tga[15] << 8);
        Expect(tga[2] == 2, "HUD icon is uncompressed truecolor");
        Expect(tw == 64 && th == 64, "HUD icon is 64x64");
        Expect(tga[16] == 32, "HUD icon is 32bpp");
        Expect(tga.size() == 18u + 64u * 64u * 4u, "HUD icon pixel data is the exact expected size");
    }

    printf("mega_bazooka_selftest: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
