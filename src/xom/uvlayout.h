// melange::xom::mesh - UV layout sheets for texture painters: `xomtool clone --uv-layout <dir>`.
//
// For every XImage a mesh uses: the original texture with the triangles of the shapes that sample it drawn as outlines
// (one colour per UV island), and a JSON description of those islands, so a painter can work in the layout and the
// result can go straight back in with `--texture k=<png>`. An island is a set of triangles connected by shared vertices
// within one shape. C++17, standard library only (PNG encoding is the caller's).
#pragma once

#include <string>
#include <vector>

#include "clone.h"

namespace melange::xom::mesh {

struct UvLayout {
    int imageIndex = 0;          // ListImages index (what `--texture k=` takes)
    std::string imageName;
    image::Pixels original;      // mip 0 at its own size, top-down, as the PNG a painter starts from
    image::Pixels overlay;       // `original` enlarged by `scale` (nearest) with the island outlines drawn on it, RGBA
    int scale = 1;
    std::string json;            // the island description (melange-uvlayout/1); names the two PNG files below
    std::string originalFile, overlayFile;
    size_t triangles = 0, islands = 0;
};

// One layout per image that some shape's shader reaches (images no shape samples are skipped). File names are
// "<prefix><k>.original.png", "<prefix><k>.layout.png" and "<prefix><k>.json".
bool BuildUvLayouts(const Document& doc, uint32_t descRef, const std::string& prefix, std::vector<UvLayout>& out,
                    std::string* error = nullptr);

}  // namespace melange::xom::mesh
