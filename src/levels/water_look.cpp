// LevelWaterLook: the engine side of wum.level.water (sim_core.h WaterEngine). The water plane is drawn by
// WaterCgGraphicEntity with CG\water.cg, whose parameters come from the theme's WaterPlaneTweaks container, the
// resource named by the data variable Water.TweakName ("<theme>.<time of day>Water"): NearOpacity is the water's alpha,
// ReflectionStrength / ReflectionContrast scale the environment map's reflection, SubtractColourScale darkens the diffuse
// texture, the Light* colours light it. Its fields are found by name in the container class's field list
// (weapons/fields.cpp), so their offsets are the engine's own. A write is put back when the match's VM closes (the
// container is the theme's resource and outlives the match). Render state only: the sim never reads it.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_core.h"
#include "melange/gamestate.h"
#include "weapons/engine.h"
#include "weapons/fields.h"

namespace melange::levels::water {
namespace {
enum class Kind : uint8_t { F32, Rgb, Rgba, Vec3 };
struct Known {
    const char* name;
    Kind kind;
};
// WaterPlaneTweaks' fields in use (tools/xom/schema.json; the Glint and Shadow colours are obsolete).
constexpr Known kFields[] = {
    {"NearOpacity", Kind::F32},        {"ReflectionStrength", Kind::F32},  {"ReflectionContrast", Kind::F32},
    {"SubtractColourScale", Kind::F32}, {"SpecularContrast", Kind::F32},   {"SpecularPower", Kind::F32},
    {"SpecularFadeScale", Kind::F32},  {"TextureScale", Kind::F32},        {"LandSpecularPower", Kind::F32},
    {"LandFresnelPower", Kind::F32},   {"SkyBlendColor", Kind::Rgba},      {"LightAmbient", Kind::Rgb},
    {"LightDiffuse", Kind::Rgb},       {"LightSpecular", Kind::Rgb},       {"LightFresnel", Kind::Rgb},
    {"LandFresnelColour", Kind::Rgb},  {"NormalIntensity", Kind::Vec3},    {"NormalIntensity1", Kind::Vec3},
    {"LightDirection", Kind::Vec3},
};
constexpr const char* kNames[] = {
    "NearOpacity", "ReflectionStrength", "ReflectionContrast", "SubtractColourScale", "SpecularContrast", "SpecularPower",
    "SpecularFadeScale", "TextureScale", "LandSpecularPower", "LandFresnelPower", "SkyBlendColor", "LightAmbient",
    "LightDiffuse", "LightSpecular", "LightFresnel", "LandFresnelColour", "NormalIntensity", "NormalIntensity1",
    "LightDirection", nullptr,
};

size_t Bytes(Kind k) { return k == Kind::F32 ? 4 : k == Kind::Rgb ? 3 : k == Kind::Rgba ? 4 : 12; }

struct Saved {
    uintptr_t at;
    std::vector<uint8_t> bytes;
};
std::vector<Saved> g_saved;   // main thread
int g_match = 0;

const Known* Find(const char* field) {
    for (const Known& k : kFields)
        if (std::strcmp(k.name, field) == 0) return &k;
    return nullptr;
}

// The theme's WaterPlaneTweaks container of this match, or 0.
uintptr_t Container(std::string* name) {
    gamestate::Var v{};
    if (!gamestate::Var1("Water.TweakName", &v)) return 0;
    std::string s = v.value;   // JSON text: a quoted string
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    if (name) *name = s;
    if (s.empty()) return 0;
    const uintptr_t c = weapons::Container(s.c_str());
    return c && weapons::engine::ClassName(weapons::engine::ClassOf(c)) == "WaterPlaneTweaks" ? c : 0;
}

// The field's address in the container, or 0.
uintptr_t FieldAt(uintptr_t c, const char* field) {
    const auto* list = weapons::fields::ForClass(weapons::engine::ClassOf(c));
    if (!list) return 0;
    for (const auto& e : *list)
        if (e.name == field) return c + e.offset;
    return 0;
}

const char* Set(const char* field, const float* v, int n) {
    const Known* k = Find(field);
    if (!k) return "not a water field";
    std::string name;
    const uintptr_t c = Container(&name);
    if (!c) return "the match has no WaterPlaneTweaks (Water.TweakName)";
    const uintptr_t at = FieldAt(c, field);
    if (!at) return "the field is not in the container's class";
    uint8_t buf[12] = {};
    switch (k->kind) {
        case Kind::F32:
            if (n != 1 || !std::isfinite(v[0]) || std::fabs(v[0]) > 1000) return "must be a number from -1000 to 1000";
            std::memcpy(buf, v, 4);
            break;
        case Kind::Rgb:
        case Kind::Rgba: {
            const int want = k->kind == Kind::Rgb ? 3 : 4;
            if (n < 3 || n > want) return k->kind == Kind::Rgb ? "must be {r, g, b}, 0-255" : "must be {r, g, b[, a]}, 0-255";
            if (k->kind == Kind::Rgba && n == 3) mem::SafeRead(at + 3, &buf[3], 1);   // keep its alpha
            for (int i = 0; i < n; ++i) {
                if (!(v[i] >= 0 && v[i] <= 255)) return "colour parts must be 0-255";
                buf[i] = static_cast<uint8_t>(std::lround(v[i]));
            }
            break;
        }
        case Kind::Vec3:
            if (n != 3) return "must be {x, y, z}";
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1000) return "must be numbers from -1000 to 1000";
            std::memcpy(buf, v, 12);
            break;
    }
    const size_t len = Bytes(k->kind);
    bool saved = false;
    for (const Saved& s : g_saved) saved |= s.at == at;
    if (!saved) {
        Saved s{at, std::vector<uint8_t>(len)};
        if (!mem::SafeRead(at, s.bytes.data(), len)) return "the field could not be read";
        g_saved.push_back(std::move(s));
    }
    if (!mem::Write(at, buf, len)) return "the field could not be written";
    LOG_INFO("[levels] water look: %s.%s set", name.c_str(), field);
    return nullptr;
}

int Get(const char* field, float out[4]) {
    const Known* k = Find(field);
    const uintptr_t c = k ? Container(nullptr) : 0;
    const uintptr_t at = c ? FieldAt(c, field) : 0;
    uint8_t buf[12] = {};
    if (!at || !mem::SafeRead(at, buf, Bytes(k->kind))) return -1;
    switch (k->kind) {
        case Kind::F32: std::memcpy(out, buf, 4); return 1;
        case Kind::Rgb:
        case Kind::Rgba: {
            const int n = k->kind == Kind::Rgb ? 3 : 4;
            for (int i = 0; i < n; ++i) out[i] = buf[i];
            return n;
        }
        case Kind::Vec3: std::memcpy(out, buf, 12); return 3;
    }
    return -1;
}

// Before the match's VM closes: the theme's values back.
void OnMatch(bool begin, void*) {
    if (begin || g_saved.empty()) return;
    for (const Saved& s : g_saved) mem::Write(s.at, s.bytes.data(), s.bytes.size());
    LOG_INFO("[levels] water look: %zu field(s) of the theme's water put back", g_saved.size());
    g_saved.clear();
}

class LevelWaterLook final : public Module {
public:
    const char* Name() const override { return "LevelWaterLook"; }
    const char* Description() const override { return "wum.level.water: the theme's water plane look for a level"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 55; }
    bool Install() override {
        g_match = simbridge::OnMatch(&OnMatch, nullptr);
        simcore::WaterEngine e;
        e.set = &Set;
        e.get = &Get;
        e.fields = kNames;
        simcore::SetWaterEngine(e);
        return true;
    }
    void Uninstall() override {
        simcore::SetWaterEngine(simcore::WaterEngine{});
        if (g_match) simbridge::RemoveOnMatch(g_match);
        g_match = 0;
    }
};
}  // namespace

MELANGE_MODULE(LevelWaterLook);
}  // namespace melange::levels::water
