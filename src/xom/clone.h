// melange::xom::mesh - cloning a vanilla mesh (static, rigid-hierarchy or skinned) into a loadable mesh bank, and the
// read-only views of its object graph that cloning, texture replacement, deformation and the UV layout all share.
//
// A vanilla mesh is not just geometry: XMeshDescriptor -> XGraphSet -> {"world" XInteriorNode tree, an
// XAnimClipLibrary, XExpandedAnimInfo, collision data, ...}, and skinned meshes add XSkin / XSkinShape / XBone /
// XPaletteWeightSet. CloneMesh copies the whole reference closure of the descriptor, so nothing that tree points at is
// left behind (a skinned clone has the same bones, weights and clips as its source). C++17, standard library only.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "image.h"
#include "mesh.h"

namespace melange::xom::mesh {

constexpr uint16_t kFirstModSection = 476;  // docs/meshes.md "Free sections"
constexpr uint16_t kLastModSection = 519;

// Packed float arrays (XCoord3fSet.Coord, XNormal3fSet.Normal, XTexCoord2fSet.TexCoord): all components, flat. Empty
// when `v` is not a packed Math array of floats. WriteFloatArray keeps the element type and replaces the data.
std::vector<float> ReadFloatArray(const Value& v);
void WriteFloatArray(Value& v, const std::vector<float>& f);

// The 1-based index of the XMeshDescriptor whose ResourceId is `resourceId`, or 0.
uint32_t FindDescriptor(const Document& doc, const std::string& resourceId);

// The "world" graph root the engine renders: the XGraphSet entry with the geometry GUID
// 6ae6dbe4fa866b45a73ff9130e12dfeb (what XMeshDescriptor::AdoptFrom fetches), else the entry named "world", else the
// first. 0 if the descriptor has none.
uint32_t WorldRoot(const Document& doc, uint32_t descRef);

// Everything reachable from `root` through Ref fields, in depth-first pre-order following each object's fields in
// stream order (so the order is stable and follows the scene: shape 0's shader and texture before shape 1's).
// parent[x] is the object x was first reached from (0 for the root). Fails, naming the object and the chain that
// reaches it, on an object xomtool cannot decode (opaque payload or the undelimited tail): its references cannot be
// followed or rewritten, so it cannot be copied.
struct Closure {
    std::vector<uint32_t> order;
    std::unordered_map<uint32_t, uint32_t> parent;
};
bool CollectClosure(const Document& doc, uint32_t root, Closure& out, std::string* error = nullptr);

// "#12 XShape \"body\"" - an object for a message.
std::string DescribeObject(const Document& doc, uint32_t ref);
// "#79 XMeshDescriptor \"Sheep\" -> #577 XGraphSet (Graphs) -> ... -> #438 XAnimClipLibrary": how `ref` was reached.
std::string ReachChain(const Document& doc, const Closure& c, uint32_t ref);

struct CloneOptions {
    // A closure that reaches another XMeshDescriptor, or an XAnimClipLibrary that objects outside the closure also use,
    // is refused (the copy would silently duplicate a resource other meshes depend on) unless this is set.
    bool allowShared = false;
};
struct ClassCount { std::string cls; size_t count = 0; };
struct CloneReport {
    std::vector<ClassCount> classes;  // closure size by class, in the new document's TYPE order (root entry included)
    size_t objects = 0;
    uint32_t srcDescriptor = 0;       // in the source document
    uint32_t descriptor = 0;          // in the new document
    uint32_t root = 0;                // the new root XGraphSet
    std::vector<std::string> notes;   // what the clone carries that is worth knowing (owned clip libraries, ...)
};

// Builds the one-mesh bank for `vanillaName` in `dst` (an empty Document): the file header is the source's, the
// descriptor's whole closure is copied, the copy's ResourceId/SectionId are patched (Flags are kept) and the root
// XGraphSet entry {99cc436e6fbef54b85d2bfcdf9ae4283, Graph -> descriptor, Name = newName} is added exactly as
// WriteBundle writes it. The descriptor's own XGraphSet keeps its "world" entry and every other entry unchanged.
// Returns the new descriptor's index, or 0 with *error set. `section` must be 476..519.
uint32_t CloneMesh(Document& dst, const Document& src, const std::string& vanillaName, const std::string& newName,
                   uint16_t section, const CloneOptions& opt, CloneReport* report, std::string* error = nullptr);

// Inserts `obj` so it becomes object `pos` (1-based), moving every later object one place on and rewriting every
// reference and the document root to match. The caller keeps TYPE-table grouping (`pos` must lie inside or at the end
// of the run of `obj.type`).
void InsertObject(Document& doc, uint32_t pos, Object obj);

// ---------------------------------------------------------------- images

// One XImage reachable from a descriptor, in graph order (ListImages order is the index `--texture k=` uses).
struct ImageInfo {
    int index = 0;
    uint32_t ref = 0;
    std::string name;
    uint32_t width = 0, height = 0, format = 0, mips = 0;
    std::vector<std::string> usedBy;  // names of the shapes whose shader reaches it
};
std::vector<ImageInfo> ListImages(const Document& doc, uint32_t descRef);

// ---------------------------------------------------------------- shapes

// A drawable in the "world" graph: an XShape (static or rigid) or an XSkinShape (skinned, bind pose).
struct ShapeRef {
    uint32_t shape = 0;      // the XShape / XSkinShape
    uint32_t geometry = 0;   // its XIndexedTriangleSet (0 if the shape draws something else)
    uint32_t shader = 0;
    std::string name;        // XShape.Name
    std::vector<std::string> path;  // names of the nodes above it, outermost first (empty names skipped)
    Mat4 matrix = Identity();       // composed XGroup transforms above it
    bool skinned = false;
};
bool EnumerateShapes(const Document& doc, uint32_t descRef, std::vector<ShapeRef>& out, std::string* error = nullptr);

// The geometry of one shape as a mesh::Primitive (positions, normals, UVs, indices, name, composed matrix).
bool ReadShapePrimitive(const Document& doc, const ShapeRef& s, Primitive& out, std::string* error = nullptr);

// The "world" graph as a hierarchical Mesh (mesh::Mesh::nodes): one Node per XGroup / XSkin with its name and local
// transform, locators included, each "<node>Shape" holder folded into its node, shapes in their node's space. This is
// what WriteMesh turns back into the vanilla layout, so an export -> edit -> `convert --bundle` round trip keeps the
// names that animation clips address. Skinned shapes come out in bind pose, without skin.
bool ReadMeshTree(const Document& doc, uint32_t descRef, Mesh& out, std::string* error = nullptr);

// An indented dump of the "world" graph (node class, name, transform; shapes with vertex/triangle counts, shader and
// images) plus a skin's skeleton, for `xomtool clone --tree`.
std::string TreeText(const Document& doc, uint32_t descRef);

}  // namespace melange::xom::mesh
