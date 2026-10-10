#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Panel icons: a mod's PNG goes into one of atlas 3's free sub-icons (9..15), written at every upload of
// "Weapon Panel Icons3". The atlas is stored bottom-up, RGB8: sub-icon k is file row 3-k/4, column k%4, 64x64
// each in a 256x256 image.
namespace melange::assets::icons {
constexpr int kSize = 64, kFirstSub = 9, kLastSub = 15, kSubCount = kLastSub - kFirstSub + 1;
constexpr int kSubTotal = 16;                      // sub-icons per atlas (4x4)
constexpr size_t kSubBytes = kSize * kSize * 3;    // one sub-icon of the RGB8 atlas

// Box-filters a w*h RGBA8 image down to 64x64 RGBA8. w must equal h, be a multiple of 64, and be at most 4096
// (the mod-parser size cap). Pure.
bool Downscale(const uint8_t* rgba, int w, int h, uint8_t out[kSize * kSize * 4], std::string* err);

// Alpha-blends a 64x64 RGBA8 icon into sub-icon `sub` (0..15) of a 256x256 RGB8 atlas buffer (196608 bytes),
// flipping the icon's row order so it lands upright in the bottom-up buffer. Pure; touches only that sub-icon's
// 64x64 region, leaving the rest of atlasRgb256 untouched.
bool WriteSubIcon(uint8_t* atlasRgb256, size_t atlasSize, int sub, const uint8_t rgba64[kSize * kSize * 4]);

// A sub-icon's current 64x64 RGB8 pixels (the same placement as WriteSubIcon, a plain copy with no row flip), and the
// reverse. Pure; used to keep a vanilla icon aside before it is replaced and to put it back.
bool ReadSubIcon(const uint8_t* atlasRgb256, size_t atlasSize, int sub, uint8_t out[kSubBytes]);
bool RestoreSubIcon(uint8_t* atlasRgb256, size_t atlasSize, int sub, const uint8_t in[kSubBytes]);

// Replacing a VANILLA weapon's panel icon (spice.json "weaponIcons"): the PNG goes into the sub-icon that iconCode
// names (atlas 1..3, sub 0..15; iconCode = atlas | sub << 8) at every upload of "Weapon Panel Icons<atlas>" while
// the patch is registered. Before the first write to a sub-icon its vanilla pixels are copied aside; every later
// upload of that atlas restores them first, so ClearVanilla() brings the vanilla icon back at the next upload of
// the atlas. A second patch for the same sub-icon replaces the first. Needs the game's upload hook.
bool PatchVanilla(const std::string& modId, const std::wstring& assetsDir, const std::string& relPng, uint32_t iconCode,
                  std::string* err);
void ClearVanilla();      // forget the patches (the atlas is restored when it is next uploaded)
uint32_t VanillaCount();  // vanilla patches registered now

// Decodes relPng under assetsDir (a PNG, RGBA, <= 4096^2, box-filtered to 64x64 if larger), and reserves the next
// free sub-icon (9..15, first come) for the (modId, relPng) pair; a repeat call for the same pair returns the same
// iconCode without using another sub-icon. Registers the "Weapon Panel Icons3" upload patcher on first use.
// iconCode = 0x0903 .. 0x0f03 (atlas 3 | sub << 8).
bool Reserve(const std::string& modId, const std::wstring& assetsDir, const std::string& relPng, uint32_t* iconCode,
             std::string* err);

// Decodes and caches the icon ahead of Reserve (start-up), so a match's clone creation does not decode PNGs.
void Preload(const std::string& modId, const std::wstring& assetsDir, const std::string& relPng);

// Patching follows the clones: on in a match where they are live, off otherwise (the reservations stay).
void Activate(bool on);

uint32_t Count();  // icons reserved this session
}  // namespace melange::assets::icons

namespace melange::assets {
void PreloadPanelIcon(const char* modId, const char* relPng);
// Resolves the mod's assets root through Thumper and calls icons::PatchVanilla; false (with err) if [Assets] is off.
bool PatchVanillaPanelIcon(const char* modId, const char* relPng, uint32_t iconCode, char* err, size_t errLen);
void ClearVanillaPanelIcons();
}
