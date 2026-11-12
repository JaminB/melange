// LevelWaterLook: the engine side of wum.level.water (sim_core.h WaterEngine). The water plane is drawn by
// WaterCgGraphicEntity with CG\water.cg, whose parameters come from the theme's WaterPlaneTweaks container, the
// resource named by the data variable Water.TweakName ("<theme>.<time of day>Water"): NearOpacity is the water's alpha,
// ReflectionStrength / ReflectionContrast scale the environment map's reflection, SubtractColourScale darkens the diffuse
// texture, the Light* colours light it. Its fields are found by name in the container class's field list
// (weapons/fields.cpp), so their offsets are the engine's own. A write is put back when the match's VM closes (the
// container is the theme's resource and outlives the match). Render state only: the sim never reads it.
//
// The sea material (Melange 0.15): water.cg takes its colour from the theme's water texture, so no field makes a sea of
// sand or lava. Sea = {r, g, b[, opacity]} and SeaKeep = {reflection, foam} are not engine fields: they are kept here
// and told to client mods as the event melange.level.sea {on, r, g, b, opacity, reflection, foam}, which a mod that
// replaces the water shader draws (docs/erg.md "Sea material"). The match's end sends {on = false}.
//
// The level haze (Melange 0.15): the colour the far distance and the band under the horizon fade to is a graphics mod's
// own (the game has no fog; Sunstone hazes toward the sky's horizon colour, which rings a sand sea in pale blue). Haze =
// {r, g, b[, amount]} and HazeRange = {start, distance} are kept the same way and told as melange.level.haze {on, r, g,
// b, amount, start, distance}: the colour 0-255, amount 0-1 (how far the mod's own haze colour gives way to it),
// start and distance in world units (0: the mod's own).
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
#include "lua/event_names.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_core.h"
#include "melange/gamestate.h"
#include "melange/lua.h"
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
    "LightDirection", "Sea", "SeaKeep", "Haze", "HazeRange", nullptr,
};

// The sea material of the running match (main thread): the colour 0-255, the rest 0-1.
struct Sea {
    bool on = false;
    float r = 0, g = 0, b = 0, opacity = 1, reflection = 0, foam = 0;
};
Sea g_sea;

void PostSea() {
    char json[192];
    if (g_sea.on)
        snprintf(json, sizeof json, "{\"on\":true,\"r\":%g,\"g\":%g,\"b\":%g,\"opacity\":%g,\"reflection\":%g,\"foam\":%g}", g_sea.r, g_sea.g,
                 g_sea.b, g_sea.opacity, g_sea.reflection, g_sea.foam);
    else
        snprintf(json, sizeof json, "{\"on\":false}");
    melange::lua::PostEvent(melange::sandbox::ev::kLevelSea, json);
}

// The level haze of the running match (main thread): the colour 0-255, amount 0-1, the range in world units (0: the mod's).
struct Haze {
    bool on = false;
    float r = 0, g = 0, b = 0, amount = 1, start = 0, distance = 0;
};
Haze g_haze;

void PostHaze() {
    char json[192];
    if (g_haze.on)
        snprintf(json, sizeof json, "{\"on\":true,\"r\":%g,\"g\":%g,\"b\":%g,\"amount\":%g,\"start\":%g,\"distance\":%g}", g_haze.r, g_haze.g, g_haze.b,
                 g_haze.amount, g_haze.start, g_haze.distance);
    else
        snprintf(json, sizeof json, "{\"on\":false}");
    melange::lua::PostEvent(melange::sandbox::ev::kLevelHaze, json);
}

// Haze = {r, g, b[, amount]} or HazeRange = {start, distance}: nullptr, or why not.
const char* SetHaze(const char* field, const float* v, int n) {
    if (std::strcmp(field, "Haze") == 0) {
        if (n < 3 || n > 4) return "must be {r, g, b[, amount]}: the colour 0-255, the amount 0-1";
        for (int i = 0; i < 3; ++i)
            if (!(v[i] >= 0 && v[i] <= 255)) return "colour parts must be 0-255";
        if (n == 4 && !(v[3] >= 0 && v[3] <= 1)) return "the amount must be 0-1";
        g_haze.on = true;
        g_haze.r = v[0], g_haze.g = v[1], g_haze.b = v[2];
        g_haze.amount = n == 4 ? v[3] : 1.0f;
        LOG_INFO("[levels] water look: level haze %g %g %g, amount %g", g_haze.r, g_haze.g, g_haze.b, g_haze.amount);
    } else {
        if (n != 2) return "must be {start, distance}, world units (0: the mod's own)";
        if (!(v[0] >= 0 && v[0] <= 100000) || !(v[1] >= 0 && v[1] <= 100000)) return "start and distance must be 0-100000 world units";
        g_haze.start = v[0], g_haze.distance = v[1];
    }
    if (g_haze.on) PostHaze();
    return nullptr;
}

// Sea = {r, g, b[, opacity]} or SeaKeep = {reflection, foam}: nullptr, or why not.
const char* SetSea(const char* field, const float* v, int n) {
    if (std::strcmp(field, "Sea") == 0) {
        if (n < 3 || n > 4) return "must be {r, g, b[, opacity]}: the colour 0-255, the opacity 0-1";
        for (int i = 0; i < 3; ++i)
            if (!(v[i] >= 0 && v[i] <= 255)) return "colour parts must be 0-255";
        if (n == 4 && !(v[3] >= 0 && v[3] <= 1)) return "the opacity must be 0-1";
        g_sea.on = true;
        g_sea.r = v[0], g_sea.g = v[1], g_sea.b = v[2];
        g_sea.opacity = n == 4 ? v[3] : 1.0f;
        LOG_INFO("[levels] water look: sea material %g %g %g, opacity %g", g_sea.r, g_sea.g, g_sea.b, g_sea.opacity);
    } else {
        if (n != 2) return "must be {reflection, foam}, each 0-1";
        if (!(v[0] >= 0 && v[0] <= 1) || !(v[1] >= 0 && v[1] <= 1)) return "reflection and foam must be 0-1";
        g_sea.reflection = v[0], g_sea.foam = v[1];
    }
    if (g_sea.on) PostSea();
    return nullptr;
}

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
    if (std::strcmp(field, "Sea") == 0 || std::strcmp(field, "SeaKeep") == 0) return SetSea(field, v, n);
    if (std::strcmp(field, "Haze") == 0 || std::strcmp(field, "HazeRange") == 0) return SetHaze(field, v, n);
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
    if (std::strcmp(field, "Sea") == 0) {
        if (!g_sea.on) return -1;
        out[0] = g_sea.r, out[1] = g_sea.g, out[2] = g_sea.b, out[3] = g_sea.opacity;
        return 4;
    }
    if (std::strcmp(field, "SeaKeep") == 0) {
        if (!g_sea.on) return -1;
        out[0] = g_sea.reflection, out[1] = g_sea.foam;
        return 2;
    }
    if (std::strcmp(field, "Haze") == 0) {
        if (!g_haze.on) return -1;
        out[0] = g_haze.r, out[1] = g_haze.g, out[2] = g_haze.b, out[3] = g_haze.amount;
        return 4;
    }
    if (std::strcmp(field, "HazeRange") == 0) {
        if (!g_haze.on) return -1;
        out[0] = g_haze.start, out[1] = g_haze.distance;
        return 2;
    }
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
    if (!begin && g_sea.on) {
        g_sea = Sea{};
        PostSea();
        LOG_INFO("[levels] water look: the sea material is off again");
    }
    if (!begin && g_haze.on) {
        g_haze = Haze{};
        PostHaze();
        LOG_INFO("[levels] water look: the level haze is off again");
    }
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
