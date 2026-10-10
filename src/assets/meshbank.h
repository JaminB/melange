#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Mod mesh banks: an xomtool-built XOM whose root is an XGraphSet of XMeshDescriptors (the same shape as
// Data/Bundles/BundlNN.xom), loaded through the engine's own graphical resource manager (XGraphicalResourceManager,
// "GRM") so a weapon container's WeaponGraphicsResourceID / PayloadGraphicsResourceID / AttachedMesh can name a mod
// mesh. See docs/meshes.md for the engine side (addresses, the stub-then-bundle protocol, what is unverified).
//
// The engine only accepts bundle contents whose resource names it already knows (a stub XMeshDescriptor registered
// from a static table at startup), so loading a mod bank is two steps: register one stub per descriptor in the bank
// (name, section, scene bin) and then load the file as the bundle of that section. A bank picks its own section id in
// [kSectionMin, kSectionMax] (xomtool convert --section N) - the vanilla table stops at 474 and 475 is the engine's
// "no section" sentinel - and every descriptor in it must carry that same section.
namespace melange::assets::meshes {
constexpr uint16_t kSectionMin = 476, kSectionMax = 519;
constexpr uint8_t kSceneBinWeapons = 8;  // the bin every vanilla weapon mesh stub uses (Bazooka.Weapon, BaseballBat, ...)

struct BankEntry {
    std::string name;  // XMeshDescriptor.ResourceId
    uint16_t flags;    // XMeshDescriptor.Flags (vanilla weapon meshes: 8)
};

// Pure parsing (melange::xom), no engine dependency: the bank's root must be an XGraphSet whose Graphs entries
// reference XMeshDescriptors (xomtool's `convert <mesh> --into` output after its root is pointed at the graph set);
// every descriptor must have the same SectionId, returned in *section. Entry names are returned in graph-set order.
// False (with *err) if the bytes don't parse, the root is not such a graph set, or the sections disagree.
bool InspectBank(const std::vector<uint8_t>& bytes, std::vector<BankEntry>* entries, uint16_t* section, std::string* err);

// The naming rule (same as loose files): every name must start with "<modId>." and be non-empty after it; the
// section must lie in [kSectionMin, kSectionMax]. Pure, for the offline self-test.
bool CheckEntries(const std::string& modId, const std::vector<BankEntry>& entries, uint16_t section, std::string* err);

// Pure: the same bank with every XMeshDescriptor's SectionId set to newSection (xom round trip, nothing else
// changes). Used when a mod's bank names a section that is out of the mod range or taken, so authors do not have to
// coordinate section numbers across mods.
bool RelocateBank(const std::vector<uint8_t>& bytes, uint16_t newSection, std::vector<uint8_t>* out, std::string* err);

// The node names the Airstrike / Super Airstrike graphic entities look up on their mesh (see docs/meshes.md): a mesh
// named by "vehicleMeshes" must carry all of them, or Setup's node lookup asserts mid-match.
const std::vector<std::string>& VehicleNodes();

// Pure: which of `required` no object in the closure of the bank's mesh `resourceName` (its XMeshDescriptor's GraphSet)
// carries as a Name. False (with *err) if the bytes don't parse or the bank has no such mesh.
bool MissingNodes(const std::vector<uint8_t>& bytes, const std::string& resourceName, const std::vector<std::string>& required,
                  std::vector<std::string>* missing, std::string* err);

// Engine side: the VehicleNodes() a loaded mod mesh lacks, comma separated ("" = none missing, or not a mesh this module
// loaded). Computed from the bank's bytes when LoadModBank loads it.
std::string VehicleNodesMissing(const char* resourceName);

// True when the engine sites this module calls were verified (known build, bytes intact, vtable slots as expected).
bool Available();

// Registers a stub for every descriptor in the bank and loads the file as that section's bundle. absPath is the
// bank's absolute path; it is handed to the engine game-relative when it lies under the game folder (the engine's
// file open is search-path based; an absolute path is passed through unchanged and may or may not open - see
// docs/meshes.md). Refuses (false, *err set, nothing touched) when: the sites are not intact, the file is unreadable
// or not a mesh bank, a name breaks the "<modId>." rule, a name is already live in the GRM, the section is outside
// the mod range or already loaded. After the engine call, every entry is re-resolved and must report a loaded graph
// set; otherwise false (the stubs stay registered: a weapon naming them will fail to create its mesh, not crash).
// With relocate (the manifest path), a bank whose section is outside the mod range or already taken is not refused:
// its SectionId is rewritten to the first free section from kSectionMin up, into Melange\cache\meshes\, and that copy
// is loaded. *sectionUsed (optional) receives the section the bank ended up in.
bool LoadModBank(const std::wstring& absPath, const std::string& modId, std::string* err, uint8_t sceneBin = kSceneBinWeapons,
                 uint16_t* sectionUsed = nullptr, bool relocate = false);

// GRM lookup by name (vtable slot 36, FindResource): true if a descriptor of that name exists (stub or loaded).
bool Resolves(const char* resourceName);

struct Info {
    uintptr_t descriptor = 0;  // the XMeshDescriptor object
    uint16_t section = 0;      // descriptor+0x18
    uint8_t sceneBin = 0;      // descriptor+0x1a
    bool loaded = false;       // descriptor+0x1b bit 0 (set when a bundle supplied the graph)
    uintptr_t graphSet = 0;    // descriptor+0x28
    std::string sourceFile;    // descriptor+0x20 (the per-resource .xom name from the static table, or ours)
};
bool Describe(const char* resourceName, Info* out);  // false if it does not resolve

uint32_t Count();      // banks loaded this session
uint32_t FreeSections();  // sections in kSectionMin..kSectionMax the engine has not loaded (or tried to): the budget left, 44 at launch
uint32_t Registered();  // stubs registered this session
}  // namespace melange::assets::meshes
