// melange::xom::image - see image.h.
#include "image.h"

#include <algorithm>
#include <cstring>

namespace melange::xom::image {
namespace {

int BppForFormat(uint32_t fmt) { return fmt == 1 ? 4 : 3; }

// The layout formula, validated against every shipped XImage: levels are back-to-back, no padding.
void ComputeLayout(uint16_t width, uint16_t height, int bpp, int levels, std::vector<uint32_t>& strides,
                    std::vector<uint32_t>& offsets, uint32_t& total) {
    strides.assign(size_t(levels), 0);
    offsets.assign(size_t(levels), 0);
    uint32_t off = 0;
    for (int i = 0; i < levels; ++i) {
        uint32_t w = std::max<uint32_t>(1, uint32_t(width) >> i);
        uint32_t h = std::max<uint32_t>(1, uint32_t(height) >> i);
        uint32_t stride = w * uint32_t(bpp);
        strides[size_t(i)] = stride;
        offsets[size_t(i)] = off;
        off += stride * h;
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
    std::vector<uint32_t> strides, offsets;
    uint32_t total;
    ComputeLayout(px.width, px.height, bpp, int(levels), strides, offsets, total);
    dataOut.assign(total, 0);
    for (uint32_t i = 0; i < levels; ++i) {
        uint32_t w = std::max<uint32_t>(1, uint32_t(px.width) >> i);
        uint32_t h = std::max<uint32_t>(1, uint32_t(px.height) >> i);
        FlipRows(levelsTopDown[i].data(), dataOut.data() + offsets[i], w, h, bpp);
    }
    mipsOut = levels;

    std::vector<std::pair<std::string, Value>> out;
    auto pushStr = [&](const char* k, const std::string& s) { Value v; v.type = Type::String; v.str = s; out.emplace_back(k, v); };
    auto pushU16 = [&](const char* k, uint16_t n) { Value v; v.type = Type::U16; v.bits = n; out.emplace_back(k, v); };
    auto pushU32Arr = [&](const char* k, const std::vector<uint32_t>& a) {
        Value v; v.type = Type::U32; v.array = true;
        for (auto x : a) { v.raw.push_back(uint8_t(x)); v.raw.push_back(uint8_t(x >> 8)); v.raw.push_back(uint8_t(x >> 16)); v.raw.push_back(uint8_t(x >> 24)); }
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
    if (level < 0 || level >= mips) return fail("mip level out of range");
    if (width == 0 || height == 0) return fail("zero-sized XImage");
    int bpp = BppForFormat(fmt);
    std::vector<uint32_t> strides, offsets;
    uint32_t total;
    ComputeLayout(width, height, bpp, mips, strides, offsets, total);
    if (!df->packed() || df->raw.size() != total)
        return fail("XImage.Data length does not match the Width/Height/Format/MipLevels formula");
    uint32_t w = std::max<uint32_t>(1, uint32_t(width) >> level), h = std::max<uint32_t>(1, uint32_t(height) >> level);
    out.width = uint16_t(w);
    out.height = uint16_t(h);
    out.channels = bpp;
    out.data.assign(size_t(w) * h * uint32_t(bpp), 0);
    FlipRows(df->raw.data() + offsets[size_t(level)], out.data.data(), w, h, bpp);
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

}  // namespace melange::xom::image
