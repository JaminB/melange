// melange::xom::mesh - static mesh object graphs <-> a small portable Mesh struct.
// XMeshDescriptor -> XGraphSet -> XInteriorNode -> XGroup ->
// XShape -> XIndexedTriangleSet{XCoord3fSet,XNormal3fSet,XTexCoord2fSet,XIndexSet}. One
// primitive per XShape; u16 indices only. C++17, standard library only, no file I/O (glTF
// text/binary framing is gltf.h; PNG is stb_image in tools/xomtool).
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "xom.h"

namespace melange::xom::mesh {

struct Vec3 { float x = 0, y = 0, z = 0; };
struct Vec2 { float u = 0, v = 0; };

// A column-major 4x4 matrix, glTF convention (same layout as XTransform's own cached `Matrix`
// field: column 0/1/2 are the transformed local X/Y/Z basis vectors, column 3 is translation).
using Mat4 = std::array<float, 16>;
Mat4 Identity();
Mat4 Multiply(const Mat4& a, const Mat4& b);  // a * b (apply b, then a)

struct Primitive {
    std::string name;
    std::vector<Vec3> positions, normals;
    std::vector<Vec2> uvs;
    std::vector<uint32_t> indices;  // triangle list, length a multiple of 3
    Mat4 matrix = Identity();       // this primitive's own node transform (already composed)
};

struct Mesh {
    std::string resourceId;
    uint16_t sectionId = 0;
    std::vector<Primitive> primitives;
};

// Reads every XShape reachable from `resourceId`'s "world" graph entry (or its first entry, if
// none is named "world"), each becoming one Primitive with the transform accumulated from its
// ancestor XGroup.Core chain.
bool ReadMesh(const Document& doc, const std::string& resourceId, Mesh& out, std::string* error = nullptr);

// Appends a new mesh graph to `doc` (adding TYPE entries as needed) and returns the new
// XMeshDescriptor's object index (1-based), or 0 with *error set. Every primitive's XShape.Shader
// is `materialFromShaderRef` (an existing ref already in `doc`, or 0 for no shader). Refuses a
// primitive with more than 65535 vertices (u16 indices only).
uint32_t WriteMesh(Document& doc, const Mesh& mesh, uint32_t materialFromShaderRef, std::string* error = nullptr);

// WriteMesh into a document that holds nothing else (plus the shader subgraph, if any), made a loadable
// mesh bank, the shape of Data/Bundles/BundlNN.xom: the document root is a new XGraphSet whose one entry is
// {resource-descriptor GUID, Graph -> the XMeshDescriptor, Name = resourceId}. One mesh per bank. Returns the
// descriptor's new 1-based index, or 0 with *error set.
uint32_t WriteBundle(Document& doc, const Mesh& mesh, uint32_t materialFromShaderRef, std::string* error = nullptr);

// Deep-copies the object subgraph reachable from `srcRef` in `src` (following every Ref field
// transitively) into `dst`, appending the copies grouped by TYPE-table order and adding TYPE
// entries as needed (`src` and `dst` may be the same document). Returns the new ref to the
// copied root within `dst`, or 0 with *error set.
uint32_t CopySubgraph(Document& dst, const Document& src, uint32_t srcRef, std::string* error = nullptr);

// Finds the mesh's XShape.Shader ref for a resource (the first primitive's), for
// --material-from. Returns 0 with *error set if the resource or its shader is not found.
uint32_t FindMeshShader(const Document& doc, const std::string& resourceId, std::string* error = nullptr);

// Walks a (possibly just-copied) Shader subgraph for the one XImage reachable via
// XSimpleShader.TextureStages[*].Texture. Returns 0 (with *error set) unless exactly one is
// found - the well-defined case this tool supports for `--texture`.
uint32_t FindShaderTexture(const Document& doc, uint32_t shaderRef, std::string* error = nullptr);

}  // namespace melange::xom::mesh
