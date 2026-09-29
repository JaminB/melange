// Downscale and WriteSubIcon: pure pixel math, kept apart from icons.cpp (which needs stb_image, the upload hook
// and the game) so an offline self-test can link this file alone.
#include "assets/icons.h"

namespace melange::assets::icons {
namespace {
constexpr int kAtlasDim = 256, kAtlasBytes = kAtlasDim * kAtlasDim * 3;
}  // namespace

bool Downscale(const uint8_t* rgba, int w, int h, uint8_t out[kSize * kSize * 4], std::string* err) {
    if (!rgba || w <= 0 || h <= 0 || w != h || w % kSize != 0 || w > 4096) {
        if (err) *err = "must be a square PNG whose side is a multiple of 64, up to 4096";
        return false;
    }
    const int scale = w / kSize;
    for (int oy = 0; oy < kSize; ++oy) {
        for (int ox = 0; ox < kSize; ++ox) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < scale; ++sy) {
                const int py = oy * scale + sy;
                for (int sx = 0; sx < scale; ++sx) {
                    const int px = ox * scale + sx;
                    const uint8_t* p = rgba + (static_cast<size_t>(py) * w + px) * 4;
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            }
            const uint32_t n = static_cast<uint32_t>(scale) * static_cast<uint32_t>(scale);
            uint8_t* o = out + (static_cast<size_t>(oy) * kSize + ox) * 4;
            for (int c = 0; c < 4; ++c) o[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
    return true;
}

bool WriteSubIcon(uint8_t* atlasRgb256, size_t atlasSize, int sub, const uint8_t rgba64[kSize * kSize * 4]) {
    if (!atlasRgb256 || !rgba64 || atlasSize != static_cast<size_t>(kAtlasBytes) || sub < kFirstSub || sub > kLastSub)
        return false;
    const int row = 3 - sub / 4, col = sub % 4;
    // The atlas is bottom-up but the PNG (after Downscale) is top-down, so row 0 of the icon (its visual top) must
    // land at the top of its cell, i.e. the highest buffer row within the band: flip locally (§1.9 item 6).
    for (int pngRow = 0; pngRow < kSize; ++pngRow) {
        const int destRow = row * kSize + (kSize - 1 - pngRow);
        for (int pngCol = 0; pngCol < kSize; ++pngCol) {
            const int destCol = col * kSize + pngCol;
            const uint8_t* src = rgba64 + (static_cast<size_t>(pngRow) * kSize + pngCol) * 4;
            uint8_t* dst = atlasRgb256 + (static_cast<size_t>(destRow) * kAtlasDim + destCol) * 3;
            const uint32_t a = src[3];
            for (int c = 0; c < 3; ++c) dst[c] = static_cast<uint8_t>((src[c] * a + dst[c] * (255 - a) + 127) / 255);
        }
    }
    return true;
}
}  // namespace melange::assets::icons
