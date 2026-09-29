// melange::xom::image - XImage <-> raw pixel buffer, with the row flip
// (XImage rows are bottom-up, RGB channel order kept).
// No file I/O and no PNG codec here: src/xom stays platform-free (PNG is stb_image in
// tools/xomtool). C++17, standard library only.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xom.h"

namespace melange::xom::image {

// Decoded pixels, top-down (row 0 first), tightly packed, `channels` bytes per pixel -
// i.e. exactly the convention a PNG codec wants.
struct Pixels {
    uint16_t width = 0, height = 0;
    int channels = 0;  // 3 (Format 0, RGB8) or 4 (Format 1, RGBA8)
    std::vector<uint8_t> data;
};

// How many mip levels a full chain has for a given size (halving to 1x1).
int FullMipCount(uint16_t width, uint16_t height);

// Extracts one mip level of an XImage object (an Object already decoded from a Document,
// e.g. via Document::object()) into top-down pixels. Recomputes the stride/offset formula
// itself (proven exhaustively over every shipped XImage) rather than trusting the object's
// own Strides/Offsets fields, and cross-checks the result against the object's Data length.
bool ExtractMip(const Object& ximage, int level, Pixels& out, std::string* error = nullptr);

// Overwrites Width/Height/MipLevels/Strides/Offsets/Format/Data on an existing XImage
// object's fields in place, from top-down pixels, regenerating the mip chain with a 2x2 box
// filter down to 1x1 unless generateMips is false (the lossless leg of the round trip: one
// level, no resampling). Name/Flags/Palette and the object's iflags/uflags/dxcount are left
// untouched, so this also works to swap the texture inside a copied shader subgraph in place.
bool StoreFields(Object& ximage, const Pixels& px, bool generateMips, std::string* error = nullptr);

// Builds a brand new, fully-populated XImage object (Name = the given name; Palette = null;
// Flags = 0) from top-down pixels. Used by `xomtool convert <png> --into ... --as <Name>`.
Object MakeXImage(const std::string& name, const Pixels& px, bool generateMips);

}  // namespace melange::xom::image
