// InspectBank / CheckEntries: the pure half of the mod mesh-bank loader (no engine, no OS calls beyond the standard
// library), kept apart from meshbank.cpp so an offline self-test can link this file and src/xom alone.
#include "assets/meshbank.h"

#include <algorithm>

#include "xom/xom.h"

namespace melange::assets::meshes {
bool InspectBank(const std::vector<uint8_t>& bytes, std::vector<BankEntry>* entries, uint16_t* section, std::string* err) {
    xom::Document doc;
    if (!xom::parse(bytes.data(), bytes.size(), doc, err)) return false;
    const xom::Object* root = doc.object(doc.root);
    if (!root || root->type != "XGraphSet" || root->opaque || root->inTail) {
        if (err) *err = "the root object is not an XGraphSet (a bundle lists its descriptors in one)";
        return false;
    }
    const xom::Value* graphs = root->field("Graphs");
    if (!graphs || graphs->size() == 0) {
        if (err) *err = "the root XGraphSet has no Graphs entries";
        return false;
    }
    bool haveSection = false;
    uint16_t sec = 0;
    for (size_t i = 0; i < graphs->size(); ++i) {
        const xom::Value entry = graphs->at(i);
        const xom::Value* ref = entry.member("Graph");
        const xom::Object* desc = ref ? doc.object(ref->asRef()) : nullptr;
        if (!desc || desc->type != "XMeshDescriptor" || desc->opaque) {
            if (err) *err = "root entry #" + std::to_string(i + 1) + " does not reference an XMeshDescriptor";
            return false;
        }
        const xom::Value* id = desc->field("ResourceId");
        const xom::Value* sid = desc->field("SectionId");
        const xom::Value* gs = desc->field("GraphSet");
        const xom::Value* fl = desc->field("Flags");
        if (!id || id->type != xom::Type::String || id->str.empty() || !sid || !gs) {
            if (err) *err = "root entry #" + std::to_string(i + 1) + ": XMeshDescriptor lacks ResourceId/SectionId/GraphSet";
            return false;
        }
        const xom::Object* graph = doc.object(gs->asRef());
        if (!graph || graph->type != "XGraphSet") {
            if (err) *err = id->str + ": GraphSet does not reference an XGraphSet";
            return false;
        }
        const uint16_t s = static_cast<uint16_t>(sid->asUInt());
        if (haveSection && s != sec) {
            if (err) *err = id->str + " has SectionId " + std::to_string(s) + " but the bank's first descriptor has " + std::to_string(sec);
            return false;
        }
        haveSection = true;
        sec = s;
        if (entries) entries->push_back({id->str, static_cast<uint16_t>(fl ? fl->asUInt() : 0)});
    }
    if (section) *section = sec;
    return true;
}

bool RelocateBank(const std::vector<uint8_t>& bytes, uint16_t newSection, std::vector<uint8_t>* out, std::string* err) {
    if (!InspectBank(bytes, nullptr, nullptr, err)) return false;
    xom::Document doc;
    if (!xom::parse(bytes.data(), bytes.size(), doc, err)) return false;
    const xom::Value* graphs = doc.object(doc.root)->field("Graphs");
    for (size_t i = 0; i < graphs->size(); ++i) {
        const xom::Value entry = graphs->at(i);
        xom::Object* desc = doc.object(entry.member("Graph")->asRef());
        desc->field("SectionId")->setInt(newSection);
    }
    return xom::serialize(doc, *out, err);
}

namespace {
void CollectRefsOf(const xom::Value& v, std::vector<uint32_t>& out) {
    if (v.type == xom::Type::Ref) {
        if (v.array) {
            for (auto& it : v.items)
                if (it.bits) out.push_back(static_cast<uint32_t>(it.bits));
        } else if (v.bits) {
            out.push_back(static_cast<uint32_t>(v.bits));
        }
        return;
    }
    if (v.array) {
        if (v.type == xom::Type::Struct && !v.packed())
            for (auto& it : v.items) CollectRefsOf(it, out);
        return;
    }
    if (v.type == xom::Type::Struct)
        for (auto& m : v.members) CollectRefsOf(m.second, out);
}
}  // namespace

const std::vector<std::string>& VehicleNodes() {
    // BomberGraphicEntity::Setup looks up rear_rotor and top_rotor on the mesh instance (an HRESULT assert if one is
    // missing) and the trail locators; BomberLogicEntity finds the camera as perspShape. See docs/meshes.md.
    static const std::vector<std::string> nodes = {"rear_rotor", "top_rotor", "trail1", "trail2", "perspShape"};
    return nodes;
}

bool MissingNodes(const std::vector<uint8_t>& bytes, const std::string& resourceName, const std::vector<std::string>& required,
                  std::vector<std::string>* missing, std::string* err) {
    xom::Document doc;
    if (!xom::parse(bytes.data(), bytes.size(), doc, err)) return false;
    const xom::Object* desc = nullptr;
    for (auto& o : doc.objects)
        if (o.type == "XMeshDescriptor" && !o.opaque && !o.inTail) {
            const xom::Value* rid = o.field("ResourceId");
            if (rid && rid->str == resourceName) {
                desc = &o;
                break;
            }
        }
    const xom::Value* gs = desc ? desc->field("GraphSet") : nullptr;
    if (!gs || !gs->asRef()) {
        if (err) *err = "no XMeshDescriptor named " + resourceName + " with a GraphSet";
        return false;
    }
    // Every Name string in the closure of the mesh's graph set: groups, shapes and the graph entries all carry one, and
    // the engine's node lookup matches on it. (Which class holds a given name does not matter here.)
    std::vector<std::string> names;
    std::vector<uint32_t> todo = {gs->asRef()};
    std::vector<bool> seen(doc.objects.size() + 1, false);
    while (!todo.empty()) {
        const uint32_t r = todo.back();
        todo.pop_back();
        const xom::Object* o = doc.object(r);
        if (!o || seen[r]) continue;
        seen[r] = true;
        for (auto& f : o->fields) {
            if (f.first == "Name" && f.second.type == xom::Type::String && !f.second.array) names.push_back(f.second.str);
            CollectRefsOf(f.second, todo);
            if (f.second.type == xom::Type::Struct && f.second.array && !f.second.packed())
                for (auto& item : f.second.items) {  // not at(): it returns a copy, and member() points into its argument
                    const xom::Value* n = item.member("Name");
                    if (n && n->type == xom::Type::String) names.push_back(n->str);
                }
        }
    }
    if (missing) {
        missing->clear();
        for (auto& r : required)
            if (std::find(names.begin(), names.end(), r) == names.end()) missing->push_back(r);
    }
    return true;
}

bool CheckEntries(const std::string& modId, const std::vector<BankEntry>& entries, uint16_t section, std::string* err) {
    if (modId.empty()) {
        if (err) *err = "no mod id";
        return false;
    }
    if (section < kSectionMin || section > kSectionMax) {
        if (err)
            *err = "SectionId " + std::to_string(section) + " is outside the mod range " + std::to_string(kSectionMin) + ".." +
                   std::to_string(kSectionMax) + " (xomtool convert --section N)";
        return false;
    }
    if (entries.empty()) {
        if (err) *err = "the bank declares no mesh";
        return false;
    }
    const std::string prefix = modId + ".";
    for (auto& e : entries) {
        if (e.name.size() <= prefix.size() || e.name.compare(0, prefix.size(), prefix) != 0) {
            if (err) *err = "\"" + e.name + "\" is not named \"" + prefix + "<something>\"";
            return false;
        }
        for (auto& o : entries)
            if (&o != &e && o.name == e.name) {
                if (err) *err = "\"" + e.name + "\" is declared twice";
                return false;
            }
    }
    return true;
}
}  // namespace melange::assets::meshes
