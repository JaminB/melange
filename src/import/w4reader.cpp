#include "import/w4reader.h"

#include <algorithm>

#include "xom/xom.h"

namespace melange::import {
namespace {
bool Parse(const std::vector<uint8_t>& b, xom::Document* d, const char* what, std::string* err) {
    std::string e;
    if (b.empty() || !xom::parse(b.data(), b.size(), *d, &e)) {
        *err = std::string(what) + ": " + (e.empty() ? "empty" : e);
        return false;
    }
    return true;
}

std::string Str(const xom::Object& o, const char* f) {
    const xom::Value* v = o.field(f);
    return v && v->type == xom::Type::String && !v->array ? v->str : std::string();
}

int64_t Int(const xom::Object& o, const char* f, int64_t def) {
    const xom::Value* v = o.field(f);
    if (!v || v->array) return def;
    switch (v->type) {
        case xom::Type::U8: case xom::Type::I8: case xom::Type::U16: case xom::Type::I16: case xom::Type::U32:
        case xom::Type::I32: case xom::Type::U64: case xom::Type::I64: case xom::Type::Enum:
            return v->asInt();
        default:
            return def;
    }
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}
}  // namespace

std::vector<std::string> SplitScripts(const std::string& list) {
    std::vector<std::string> out;
    size_t p = 0;
    while (p <= list.size()) {
        size_t q = list.find(',', p);
        if (q == std::string::npos) q = list.size();
        std::string t = Trim(list.substr(p, q - p));
        if (!t.empty()) out.push_back(std::move(t));
        p = q + 1;
    }
    return out;
}

bool ReadRegistry(const std::vector<uint8_t>& bytes, std::vector<RegistryEntry>* out, std::string* err) {
    out->clear();
    xom::Document d;
    if (!Parse(bytes, &d, "registry", err)) return false;
    for (const auto& o : d.objects) {
        if (o.type != "XContainerResourceDetails" || o.opaque || o.inTail) continue;
        const xom::Value* v = o.field("Value");
        if (!v || v->type != xom::Type::Ref || v->array) continue;
        const xom::Object* det = d.object(v->asRef());
        if (!det || det->type != "WXFE_LevelDetails" || det->opaque || det->inTail) continue;
        RegistryEntry e;
        e.key = Str(o, "Name");
        e.fileName = Str(*det, "Level_FileName");
        e.frontendName = Str(*det, "Frontend_Name");
        e.frontendImage = Str(*det, "Frontend_Image");
        e.scripts = SplitScripts(Str(*det, "Level_ScriptName"));
        e.levelType = static_cast<int>(Int(*det, "Level_Type", -1));
        if (!e.key.empty()) out->push_back(std::move(e));
    }
    std::sort(out->begin(), out->end(), [](const RegistryEntry& a, const RegistryEntry& b) { return a.key < b.key; });
    if (out->empty()) {
        *err = "registry: no level entries";
        return false;
    }
    return true;
}

bool ReadDescriptor(const std::vector<uint8_t>& bytes, const std::string& authorKey, Descriptor* out, std::string* err) {
    *out = Descriptor{};
    xom::Document d;
    if (!Parse(bytes, &d, "descriptor", err)) return false;
    bool bank = false, theme = false;
    for (const auto& o : d.objects) {
        if (o.type == "XDataBank") bank = true;
        if (o.opaque || o.inTail) continue;
        const std::string name = Str(o, "Name");
        if (o.type == "XStringResourceDetails") {
            const std::string val = Str(o, "Value");
            if (name == "Databank.Theme") out->theme = val, theme = true;
            else if (name == "Databank.TimeOfDay") out->timeOfDay = val;
            else if (name == "Databank.MaterialFile") out->materialFile = val;
            else if (name == "Heightmap.BaseTexture") out->heightmapBase = val;
            else if (name == "Heightmap.SecondTexture") out->heightmapSecond = val;
            else if (!authorKey.empty() && name == authorKey) out->author = val;
        } else if (o.type == "XUintResourceDetails") {
            const int64_t val = Int(o, "Value", 0);
            const uint32_t u = val < 0 || val > 0xffffffffll ? 0 : static_cast<uint32_t>(val);
            if (name == "Databank.CustomTextureBank") out->customTextureBank = u;
            else if (name == "Databank.CustomDetailBank") out->customDetailBank = u;
        }
    }
    if (!bank || !theme) {
        *err = "descriptor: not a level descriptor (no XDataBank or Databank.Theme)";
        return false;
    }
    return true;
}

bool ReadStrings(const std::vector<uint8_t>& bytes, std::map<std::string, std::string>* out, std::string* err) {
    out->clear();
    xom::Document d;
    if (!Parse(bytes, &d, "language bank", err)) return false;
    for (const auto& o : d.objects)
        if (o.type == "XStringResourceDetails" && !o.opaque && !o.inTail) out->emplace(Str(o, "Name"), Str(o, "Value"));
    return true;
}
}  // namespace melange::import
