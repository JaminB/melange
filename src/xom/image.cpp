// melange::xom::image - see image.h.
#include "image.h"

#include <algorithm>
#include <cstring>

namespace melange::xom::image {
namespace {

int BppForFormat(uint32_t fmt) { return fmt == 1 ? 4 : 3; }

// The layout formula, validated against every shipped XImage: levels are back-to-back, no padding. `levels` must
// already be bounded to at most 32 by the caller: a shift count of 32 or more on a 32-bit value is undefined
// behaviour, and 64-bit accumulation (rather than the on-disk field's own uint32_t) keeps `total` from wrapping
// back into a small, plausible-looking size for a caller-supplied MipLevels this formula didn't actually produce.
void ComputeLayout(uint16_t width, uint16_t height, int bpp, int levels, std::vector<uint64_t>& strides,
                    std::vector<uint64_t>& offsets, uint64_t& total) {
    strides.assign(size_t(levels), 0);
    offsets.assign(size_t(levels), 0);
    uint64_t off = 0;
    for (int i = 0; i < levels; ++i) {
        uint32_t w = std::max<uint32_t>(1, uint32_t(width) >> i);
        uint32_t h = std::max<uint32_t>(1, uint32_t(height) >> i);
        uint64_t stride = uint64_t(w) * uint64_t(bpp);
        strides[size_t(i)] = stride;
        offsets[size_t(i)] = off;
        off += stride * uint64_t(h);
    }
    total = off;
}

void FlipRows(const uint8_t* src, uint8_t* dst, uint32_t width, uint32_t height, int bpp) {
    uint32_t stride = width * uint32_t(bpp);
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(dst + size_t(y) * stride, src + size_t(height - 1 - y) * stride, stride);
}

std::vector<uint8_t> BoxDownsample(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, int bpp, uint32_t& outW,
                                    uint32_t& outH) {
    outW = std::max<uint32_t>(1, w / 2);
    outH = std::max<uint32_t>(1, h / 2);
    std::vector<uint8_t> out(size_t(outW) * outH * uint32_t(bpp), 0);
    for (uint32_t y = 0; y < outH; ++y) {
        for (uint32_t x = 0; x < outW; ++x) {
            for (int c = 0; c < bpp; ++c) {
                uint32_t total = 0, n = 0;
                for (uint32_t dy = 0; dy < 2; ++dy) {
                    uint32_t sy = y * 2 + dy;
                    if (sy >= h) continue;
                    for (uint32_t dx = 0; dx < 2; ++dx) {
                        uint32_t sx = x * 2 + dx;
                        if (sx >= w) continue;
                        total += src[(size_t(sy) * w + sx) * uint32_t(bpp) + uint32_t(c)];
                        ++n;
                    }
                }
                out[(size_t(y) * outW + x) * uint32_t(bpp) + uint32_t(c)] = uint8_t(total / std::max<uint32_t>(1, n));
            }
        }
    }
    return out;
}

std::vector<std::pair<std::string, Value>> FieldsFromPixelsBottomUp(const Pixels& px, bool generateMips,
                                                                     std::vector<uint8_t>& dataOut, uint32_t& mipsOut) {
    int bpp = px.channels;
    std::vector<std::vector<uint8_t>> levelsTopDown = {px.data};
    uint32_t lw = px.width, lh = px.height;
    while (generateMips && (lw > 1 || lh > 1)) {
        uint32_t nw, nh;
        levelsTopDown.push_back(BoxDownsample(levelsTopDown.back(), lw, lh, bpp, nw, nh));
        lw = nw;
        lh = nh;
    }
    uint32_t levels = uint32_t(levelsTopDown.size());
    std::vector<uint64_t> strides, offsets;
    uint64_t total;
    ComputeLayout(px.width, px.height, bpp, int(levels), strides, offsets, total);
    dataOut.assign(size_t(total), 0);
    for (uint32_t i = 0; i < levels; ++i) {
        uint32_t w = std::max<uint32_t>(1, uint32_t(px.width) >> i);
        uint32_t h = std::max<uint32_t>(1, uint32_t(px.height) >> i);
        FlipRows(levelsTopDown[i].data(), dataOut.data() + size_t(offsets[i]), w, h, bpp);
    }
    mipsOut = levels;

    std::vector<std::pair<std::string, Value>> out;
    auto pushStr = [&](const char* k, const std::string& s) { Value v; v.type = Type::String; v.str = s; out.emplace_back(k, v); };
    auto pushU16 = [&](const char* k, uint16_t n) { Value v; v.type = Type::U16; v.bits = n; out.emplace_back(k, v); };
    auto pushU32Arr = [&](const char* k, const std::vector<uint64_t>& a) {
        Value v; v.type = Type::U32; v.array = true;
        for (auto x64 : a) { uint32_t x = uint32_t(x64);
            v.raw.push_back(uint8_t(x)); v.raw.push_back(uint8_t(x >> 8)); v.raw.push_back(uint8_t(x >> 16)); v.raw.push_back(uint8_t(x >> 24)); }
        out.emplace_back(k, v);
    };
    (void)pushStr;
    pushU16("Width", px.width);
    pushU16("Height", px.height);
    pushU16("MipLevels", uint16_t(levels));
    pushU16("Flags", 0);
    pushU32Arr("Strides", strides);
    pushU32Arr("Offsets", offsets);
    { Value v; v.type = Type::Enum; v.bits = uint32_t(bpp == 4 ? 1 : 0); out.emplace_back("Format", v); }
    { Value v; v.type = Type::U8; v.array = true; v.raw = dataOut; out.emplace_back("Data", v); }
    { Value v; v.type = Type::Ref; v.bits = 0; out.emplace_back("Palette", v); }
    return out;
}

}  // namespace

int FullMipCount(uint16_t width, uint16_t height) {
    int n = 1;
    while ((width >> (n - 1)) > 1 || (height >> (n - 1)) > 1) ++n;
    return n;
}

bool ExtractMip(const Object& ximage, int level, Pixels& out, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    if (ximage.type != "XImage") return fail("not an XImage");
    const Value *wf = ximage.field("Width"), *hf = ximage.field("Height"), *ff = ximage.field("Format"),
                *mf = ximage.field("MipLevels"), *df = ximage.field("Data");
    if (!wf || !hf || !ff || !mf || !df) return fail("XImage is missing a field");
    uint16_t width = uint16_t(wf->asUInt()), height = uint16_t(hf->asUInt());
    uint32_t fmt = uint32_t(ff->asUInt());
    int mips = int(mf->asUInt());
    // MipLevels comes straight off the file. Above 32, `width >> i` (i is the loop index below) would shift a
    // 32-bit value by 32 or more, which is undefined behaviour; ComputeLayout also assumes its caller already
    // bounded `levels` this way.
    if (mips <= 0 || mips > 32) return fail("MipLevels out of range");
    if (level < 0 || level >= mips) return fail("mip level out of range");
    if (width == 0 || height == 0) return fail("zero-sized XImage");
    int bpp = BppForFormat(fmt);
    std::vector<uint64_t> strides, offsets;
    uint64_t total;
    ComputeLayout(width, height, bpp, mips, strides, offsets, total);
    if (total > (1ull << 32)) return fail("XImage.Data would be implausibly large for its Width/Height/MipLevels");
    if (!df->packed() || df->raw.size() != total)
        return fail("XImage.Data length does not match the Width/Height/Format/MipLevels formula");
    uint32_t w = std::max<uint32_t>(1, uint32_t(width) >> level), h = std::max<uint32_t>(1, uint32_t(height) >> level);
    out.width = uint16_t(w);
    out.height = uint16_t(h);
    out.channels = bpp;
    out.data.assign(size_t(w) * h * uint32_t(bpp), 0);
    FlipRows(df->raw.data() + size_t(offsets[size_t(level)]), out.data.data(), w, h, bpp);
    return true;
}

bool StoreFields(Object& ximage, const Pixels& px, bool generateMips, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    if (px.channels != 3 && px.channels != 4) return fail("pixels must be RGB (3) or RGBA (4)");
    if (px.width == 0 || px.height == 0) return fail("zero-sized image");
    std::vector<uint8_t> dataOut;
    uint32_t mips;
    auto built = FieldsFromPixelsBottomUp(px, generateMips, dataOut, mips);
    for (auto& [k, v] : built) {
        Value* slot = ximage.field(k);
        if (!slot) return fail("XImage template is missing field " + k);
        *slot = v;
    }
    return true;
}

Object MakeXImage(const std::string& name, const Pixels& px, bool generateMips) {
    Object o;
    o.type = "XImage";
    o.container = true;
    std::vector<uint8_t> dataOut;
    uint32_t mips;
    auto built = FieldsFromPixelsBottomUp(px, generateMips, dataOut, mips);
    Value nameV; nameV.type = Type::String; nameV.str = name;
    o.fields.emplace_back("Name", nameV);
    for (auto& kv : built) o.fields.push_back(kv);
    return o;
}


Pixels Resize(const Pixels& px, uint16_t width, uint16_t height) {
    Pixels out;
    out.width = width;
    out.height = height;
    out.channels = px.channels;
    const int ch = px.channels;
    out.data.assign(size_t(width) * height * size_t(ch), 0);
    if (px.width == 0 || px.height == 0 || width == 0 || height == 0 || ch <= 0) return out;
    const double sx = double(px.width) / width, sy = double(px.height) / height;
    const bool enlarge = width >= px.width && height >= px.height;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
            for (int c = 0; c < ch; ++c) {
                double v = 0;
                if (enlarge) {
                    // Bilinear at the pixel centre.
                    const double fx = std::min(std::max((x + 0.5) * sx - 0.5, 0.0), double(px.width - 1));
                    const double fy = std::min(std::max((y + 0.5) * sy - 0.5, 0.0), double(px.height - 1));
                    const uint32_t x0 = uint32_t(fx), y0 = uint32_t(fy);
                    const uint32_t x1 = std::min<uint32_t>(x0 + 1, px.width - 1), y1 = std::min<uint32_t>(y0 + 1, px.height - 1);
                    const double tx = fx - x0, ty = fy - y0;
                    auto at = [&](uint32_t xx, uint32_t yy) { return double(px.data[(size_t(yy) * px.width + xx) * size_t(ch) + size_t(c)]); };
                    v = (at(x0, y0) * (1 - tx) + at(x1, y0) * tx) * (1 - ty) + (at(x0, y1) * (1 - tx) + at(x1, y1) * tx) * ty;
                } else {
                    // Area average over the source rectangle this pixel covers, edges weighted by coverage.
                    const double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
                    double sum = 0, wsum = 0;
                    for (uint32_t yy = uint32_t(y0); yy < px.height && yy < y1; ++yy) {
                        const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                        for (uint32_t xx = uint32_t(x0); xx < px.width && xx < x1; ++xx) {
                            const double w = (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0)) * wy;
                            if (w <= 0) continue;
                            sum += w * px.data[(size_t(yy) * px.width + xx) * size_t(ch) + size_t(c)];
                            wsum += w;
                        }
                    }
                    v = wsum > 0 ? sum / wsum : 0;
                }
                out.data[(size_t(y) * width + x) * size_t(ch) + size_t(c)] = uint8_t(std::min(255.0, std::max(0.0, v + 0.5)));
            }
    return out;
}

Pixels WithChannels(const Pixels& px, int channels) {
    if (px.channels == channels || (channels != 3 && channels != 4) || (px.channels != 3 && px.channels != 4)) return px;
    Pixels out;
    out.width = px.width;
    out.height = px.height;
    out.channels = channels;
    const size_t n = size_t(px.width) * px.height;
    out.data.resize(n * size_t(channels));
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) out.data[i * size_t(channels) + size_t(c)] = px.data[i * size_t(px.channels) + size_t(c)];
        if (channels == 4) out.data[i * 4 + 3] = 255;
    }
    return out;
}

bool ReplacePixels(Object& ximage, const Pixels& px, bool* resampled, std::string* error) {
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    if (resampled) *resampled = false;
    if (ximage.type != "XImage") return fail("not an XImage");
    const Value *wf = ximage.field("Width"), *hf = ximage.field("Height"), *ff = ximage.field("Format"), *mf = ximage.field("MipLevels");
    if (!wf || !hf || !ff || !mf) return fail("XImage is missing a field");
    const uint16_t width = uint16_t(wf->asUInt()), height = uint16_t(hf->asUInt());
    if (px.channels != 3 && px.channels != 4) return fail("pixels must be RGB (3) or RGBA (4)");
    Pixels use = WithChannels(px, BppForFormat(uint32_t(ff->asUInt())));
    if (use.width != width || use.height != height) {
        use = Resize(use, width, height);
        if (resampled) *resampled = true;
    }
    // StoreFields rewrites every field a fresh image has, including Flags (0) and Palette (none). A vanilla image's Flags
    // is often 2 or 4 (168 and 17 of Bundl09's 338), which the engine may read, so a pixel swap keeps both as they were.
    const Value* oldFlags = ximage.field("Flags");
    const Value* oldPalette = ximage.field("Palette");
    const Value savedFlags = oldFlags ? *oldFlags : Value();
    const Value savedPalette = oldPalette ? *oldPalette : Value();
    const bool hadFlags = oldFlags != nullptr, hadPalette = oldPalette != nullptr;
    if (!StoreFields(ximage, use, mf->asUInt() > 1, error)) return false;
    if (hadFlags) *ximage.field("Flags") = savedFlags;
    if (hadPalette) *ximage.field("Palette") = savedPalette;
    return true;
}

}  // namespace melange::xom::image
