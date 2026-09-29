// melange::xom::gltf - a minimal glTF 2.0 reader/writer for static mesh geometry only
// (positions/normals/UV0/indices and node transforms). No materials, images, skins or
// animation: XShape.Shader material handling is mesh.h's --material-from, not glTF's own
// material JSON. C++17, standard library only.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mesh.h"

namespace melange::xom::gltf {

// Reads every triangle-list mesh primitive drawn by the glTF's default scene (or, absent one,
// every root node), each becoming one mesh::Primitive with its node's transform (matrix, or
// TRS composed into one) already applied to Primitive::matrix - ancestor transforms are
// composed in too. `isGlb` selects a self-contained .glb (one JSON chunk, one BIN chunk); a
// plain .gltf's buffer is resolved as a file named by its "uri", read from `binDir`.
bool ReadGltf(const std::vector<uint8_t>& fileBytes, bool isGlb, const std::string& binDir,
              std::vector<mesh::Primitive>& out, std::string* error = nullptr);

// Writes a minimal glTF 2.0 document: one mesh per primitive, one node per primitive (with its
// own `matrix`) under the scene root, and one external buffer. `binFileName` is the name written
// into the JSON's buffer `uri` (a sibling file); `out.bin` is that buffer's bytes.
struct Output { std::string json; std::vector<uint8_t> bin; };
Output WriteGltf(const std::vector<mesh::Primitive>& primitives, const std::string& binFileName);

}  // namespace melange::xom::gltf
