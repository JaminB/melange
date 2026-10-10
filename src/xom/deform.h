// melange::xom::mesh - vertex-only deformation of a mesh in a Document: `xomtool clone --deform script.json`.
//
// The edit moves XCoord3fSet positions and nothing else that a vertex count could depend on: vertex count and order,
// the index sets, UVs, XPaletteWeightSet skin weights and every XBone are left byte for byte as they were. That is what
// keeps a deformed copy a drop-in for the original: the same animation clips, the same skin, the same texture layout.
// Normals are recomputed for the vertices the edit reached, and bounds are refreshed so culling keeps working.
//
// Script (JSON): either an array of ops, or {"select": "<glob>", "ops": [...]}. Ops run in order, each on the shapes
// its "select" names (a glob, or an array of globs, over the shape's name and the names of the nodes above it; "*" =
// every shape; default = the script's "select", else "*"). Positions are the shape's own stored coordinates (bind pose
// for a skinned shape, node-local for a rigid one).
//
//   {"op":"scale",     "s":[x,y,z] | k, "about":[x,y,z]}            about defaults to the selection's bounds centre
//   {"op":"translate", "t":[x,y,z]}
//   {"op":"bend",      "axis":"y", "dir":"z", "amount":deg, "length":L, "about":[x,y,z]}
//        bends the selection about `about` (default: the bounds minimum along `axis`, centred on the other axes) into an
//        arc that turns `amount` degrees over `length` (default: up to the bounds maximum) toward +dir; beyond the arc
//        the shape continues straight. Vertices before `about` do not move.
//   {"op":"push",      "dist":d}                                    along the vertex normal (welded across UV seams)
//   {"op":"noise",     "amp":a, "freq":f, "seed":n, "mode":"normal"|"vector"}
//        value noise of the position, deterministic: the same script always gives the same mesh. Vertices at the same
//        position get the same offset, so seams do not open.
//   {"op":"region",    "box":[[x0,y0,z0],[x1,y1,z1]], "falloff":r, "then":[ops...]}
//        runs `then` only on vertices inside the box, fading out over `falloff` outside it. Inside a region, default
//        pivots (`about`) are the box centre.
//
// C++17, standard library only.
#pragma once

#include <string>
#include <vector>

#include "clone.h"

namespace melange::xom::mesh {

struct DeformReport {
    struct Shape {
        std::string name;
        bool skinned = false;
        size_t vertices = 0;
        size_t moved = 0;        // vertices whose position changed
        size_t renormalised = 0; // vertices whose normal was recomputed
        float maxMove = 0;
    };
    std::vector<Shape> shapes;
    float maxDisplacement = 0;   // over all shapes, in the shapes' own units
    std::vector<std::string> notes;
};

// Applies the script to the mesh `descRef` describes. On failure nothing in `doc` has changed (the script is parsed and
// every selector resolved before the first vertex moves; a geometry shared with an unselected shape is an error).
bool ApplyDeform(Document& doc, uint32_t descRef, const std::string& scriptJson, DeformReport* report,
                 std::string* error = nullptr);

// Glob match used by "select": '*' any run, '?' one character, case-insensitive.
bool GlobMatch(const std::string& pattern, const std::string& text);

}  // namespace melange::xom::mesh
