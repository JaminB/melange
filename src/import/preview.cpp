#include "import/preview.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#pragma warning(push, 0)
#include "stb_image.h"
#include "stb_image_write.h"
#pragma warning(pop)

namespace melange::import {
namespace {
void Append(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* b = static_cast<const uint8_t*>(data);
    out->insert(out->end(), b, b + size);
}
}  // namespace

bool TgaToPng(const std::vector<uint8_t>& tga, int width, std::vector<uint8_t>* png) {
    png->clear();
    if (tga.empty() || tga.size() > (8u << 20) || width < 16) return false;
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(tga.data(), static_cast<int>(tga.size()), &w, &h, &n) || w <= 0 || h <= 0 || w > 4096 || h > 4096)
        return false;
    stbi_uc* px = stbi_load_from_memory(tga.data(), static_cast<int>(tga.size()), &w, &h, &n, 3);
    if (!px) return false;
    const int ow = w < width ? w : width;
    const int oh = (h * ow + w / 2) / w > 0 ? (h * ow + w / 2) / w : 1;
    std::vector<uint8_t> out(static_cast<size_t>(ow) * oh * 3);
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = (y + 1) * h / oh > y0 ? (y + 1) * h / oh : y0 + 1;
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = (x + 1) * w / ow > x0 ? (x + 1) * w / ow : x0 + 1;
            unsigned sum[3] = {0, 0, 0}, cnt = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const stbi_uc* p = px + (static_cast<size_t>(yy) * w + xx) * 3;
                    sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
                    ++cnt;
                }
            uint8_t* o = out.data() + (static_cast<size_t>(y) * ow + x) * 3;
            for (int c = 0; c < 3; ++c) o[c] = static_cast<uint8_t>(sum[c] / cnt);
        }
    }
    stbi_image_free(px);
    return stbi_write_png_to_func(&Append, png, ow, oh, 3, out.data(), ow * 3) != 0 && !png->empty();
}
}  // namespace melange::import
