// Weapons and Assets: weapon clones from content mods and their mod assets. This file owns the modules, the ini
// sections, the lifecycle wiring and the state verbs; the components live beside it.
#include <chrono>
#include <cstdio>
#include <string>

#include "assets/icons.h"
#include "assets/searchpath.h"
#include "assets/upload.h"
#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "melange/assets.h"
#include "melange/jlog.h"
#include "melange/testcmd.h"
#include "melange/weapons.h"
#include "mods/thumper_internal.h"
#include "weapons/behaviour.h"
#include "weapons/contrib.h"
#include "weapons/engine.h"
#include "weapons/fields.h"
#include "weapons/manifest.h"
#include "weapons/registry.h"
#include "weapons/registry_core.h"

namespace {
namespace eng = melange::weapons::engine;
namespace wm = melange::weapons::manifest;
bool g_weapons = false, g_assets = false, g_rootsPending = false;

template <class T>
T Rd(uintptr_t a) {
    T v{};
    melange::mem::SafeRead(a, &v, sizeof(T));
    return v;
}

const char* TypeName(melange::weapons::FieldType t) {
    using melange::weapons::FieldType;
    switch (t) {
        case FieldType::F32: return "f32";
        case FieldType::I32: return "i32";
        case FieldType::U32: return "u32";
        case FieldType::U16: return "u16";
        case FieldType::U8: return "u8";
        case FieldType::Bool: return "bool";
        case FieldType::String: return "string";
        default: return "none";
    }
}

bool BasesOk() {
    bool ok = true;
    for (int id : {1, 2, 6, 7, 16}) {
        const char* want = wm::BaseName(id);
        const std::string have = eng::ReadCString(reinterpret_cast<uintptr_t>(eng::EnumName(id)), 64);
        if (have != want) {
            LOG_ERROR("[weapons] weapon id %d is '%s', expected '%s'", id, have.c_str(), want);
            ok = false;
        }
    }
    return ok;
}

void OnContext(bool created, melange::lua50::State*, void*) {
    if (created) return;
    melange::weapons::behaviour::OnMatchEnd();
    melange::weapons::registry::OnMatchEnd();
}

void BeforeMods(void*) {
    melange::weapons::registry::OnInit();
    melange::weapons::behaviour::OnMatchBegin();
}

bool VerbState(std::string_view, void*) {
    using namespace melange::weapons;
    const auto& decls = wm::Frozen();
    LOG_INFO("[weapons] state: enabled=%d declared=%zu live=%d active=%d fieldsTrusted=%d suppressed=%d", Enabled(),
             decls.size(), Live(), ActiveClone(), g_weapons && fields::Trusted(), eng::Suppressed());
    for (auto& d : decls) {
        std::string set;
        for (auto& s : d.set) set += " " + s.field;
        LOG_INFO("[weapons]   k=%u vid=%x %s base %s(%d) cell %d mod %s bank '%s' icon '%s' hud '%s' set:%s", d.k,
                 kVidBase + d.k, d.name.c_str(), d.base.c_str(), d.baseId, d.cell, d.mod.c_str(), d.bank.c_str(),
                 d.panelIcon.c_str(), d.hudIcon.c_str(), set.empty() ? " -" : set.c_str());
    }
    const auto& text = registry::Core();
    LOG_INFO("[weapons]   weaponText: declared=%zu live=%d", wm::FrozenText().size(), text.TextLive());
    for (auto& t : wm::FrozenText()) LOG_INFO("[weapons]   text %s by %s name='%s' help='%s'", t.weapon.c_str(), t.mod.c_str(),
                                              t.name.c_str(), t.help.c_str());
    for (int i = 0; i < text.TextCount(); ++i) {
        const melange::weapons::core::TextRule* t = text.TextAt(i);
        LOG_INFO("[weapons]   text %s: id %d slot %08x key %s name=%d help=%d", t->decl.weapon.c_str(), t->id,
                 static_cast<unsigned>(t->slot), t->token.c_str(), t->text, t->help);
    }
    LOG_INFO("[weapons]   weaponIcons: declared=%zu live=%d hudLive=%d", wm::FrozenIcons().size(), text.IconsLive(), text.HudIconsLive());
    for (int i = 0; i < text.IconCount(); ++i) {
        const melange::weapons::core::IconRule* t = text.IconAt(i);
        LOG_INFO("[weapons]   icons %s by %s: id %d panel=%d (code %x) %s hud=%d %s -> %s", t->decl.weapon.c_str(), t->decl.mod.c_str(), t->id,
                 t->panel, static_cast<unsigned>(t->iconCode), t->decl.panelIcon.c_str(), t->hud, t->hudFile ? t->hudFile : "-",
                 t->decl.hudIcon.c_str());
    }
    LOG_INFO("[weapons]   vehicleMeshes: declared=%zu live=%d", wm::FrozenVehicles().size(), text.VehiclesLive());
    for (int i = 0; i < text.VehicleCount(); ++i) {
        const melange::weapons::core::VehicleRule* t = text.VehicleAt(i);
        LOG_INFO("[weapons]   vehicle %s by %s: %s armed=%d", t->decl.vehicle.c_str(), t->decl.mod.c_str(), t->decl.mesh.c_str(), t->armed);
    }
    CloneInfo info[kMaxClones] = {};
    const int n = Declared(info, kMaxClones);
    for (int k = 0; k < n && k < kMaxClones; ++k)
        LOG_INFO("[weapons]   k=%d live=%d container %08x desc %08x iconCode %x", k, info[k].live,
                 static_cast<unsigned>(info[k].container), static_cast<unsigned>(info[k].descriptor), info[k].iconCode);
    for (int c : kFreeCells)
        LOG_INFO("[weapons]   cell %d = {%x,%x}", c, Rd<uint32_t>(eng::kPanel + 8 * c), Rd<uint32_t>(eng::kPanel + 8 * c + 4));
    for (int id : {1, 2, 6, 7, 16})
        LOG_INFO("[weapons]   slot %d = %s", id, eng::ReadCString(reinterpret_cast<uintptr_t>(eng::EnumName(id)), 64).c_str());
    const auto b = behaviour::GetCounters();
    LOG_INFO("[weapons]   behaviour hooks=%d extras/explosion=%d fires=%u ticks=%u impacts=%u explosions=%u extras=%u",
             behaviour::HooksEnabled(), behaviour::ExtraLimit(), b.fires, b.ticks, b.impacts, b.explosions, b.extras);
    for (Event e : {Event::Fire, Event::Tick, Event::Impact, Event::Explosion}) {
        const auto c = behaviour::EventCost(e);
        if (c.n) LOG_INFO("[weapons]   cost %d: n=%u p50=%.1f us p95=%.1f us max=%.1f us", static_cast<int>(e), c.n, c.p50Us, c.p95Us, c.maxUs);
    }
    for (auto& h : eng::Hooks())
        LOG_INFO("[weapons]   hook %s @%08x created=%d wanted=%d enabled=%d", h.name.c_str(), static_cast<unsigned>(h.site),
                 h.created, h.wanted, h.enabled);
    melange::jlog::Rec("weapons", melange::jlog::Level::Info, "state")
        .Bool("enabled", Enabled()).Uint("declared", decls.size()).Bool("live", Live()).Int("active", ActiveClone())
        .Uint("hooks", eng::Hooks().size());
    return true;
}

bool VerbField(std::string_view a, void*) {
    std::string s(a);
    const size_t sp = s.find(' ');
    const std::string name = s.substr(0, sp), field = sp == std::string::npos ? "" : s.substr(sp + 1);
    const uintptr_t c = melange::weapons::Container(name.c_str());
    uint32_t off = 0;
    const auto t = c ? melange::weapons::Field(c, field.c_str(), &off) : melange::weapons::FieldType::None;
    LOG_INFO("[weapons] field %s.%s: container %08x type %s offset %x", name.c_str(), field.c_str(), static_cast<unsigned>(c),
             TypeName(t), off);
    return c != 0;
}

// Every enabled content mod with an assets/loose/ folder gets a search path, once per launch. Panel icons and
// banks are per-clone and per-match, so they stay with the registry (it asks ReservePanelIcon/LoadModBank).
void AddModRoots() {
    for (auto& e : melange::thumper::Snapshot()) {
        if (!e.sessionActive || !e.contentRelevant) continue;
        char err[256] = {};
        if (!melange::assets::AddModRoot(e.manifest.id.c_str(), err, sizeof err) && *err)
            LOG_WARN("[assets] %s: %s", e.manifest.id.c_str(), err);
    }
}

bool VerbAssets(std::string_view, void*) {
    const auto s = melange::assets::GetStats();
    const auto u = melange::assets::upload::GetStats();
    LOG_INFO("[assets] stats: enabled=%d roots=%u banks=%u icons=%u uploadsPatched=%u msLastPatch=%.3f patchers=%u "
             "uploadHook=%d uploadsSeen=%u paths=%zu",
             melange::assets::Enabled(), s.roots, s.banks, s.icons, s.uploadsPatched, s.msLastPatch, u.patchers, u.hooked,
             u.uploadsSeen,
             melange::assets::searchpath::Added().size());
    return true;
}

class Weapons final : public melange::Module {
public:
    const char* Name() const override { return "Weapons"; }
    const char* Description() const override { return "weapon clones from content mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 54; }
    bool Install() override {
        melange::config::EnsureKey("Weapons", "ExtraPerExplosion", "8");
        melange::config::EnsureKey("Weapons", "LogEvents", "0");
        const bool sites = eng::SitesOk();
        const bool bases = BasesOk();
        g_weapons = sites && bases;
        melange::testcmd::Register("weapons.state", &VerbState);
        melange::testcmd::Register("weapons.field", &VerbField);
        const size_t n = wm::Frozen().size();
        if (g_weapons) melange::weapons::behaviour::InstallLua();
        const size_t nt = wm::FrozenText().size(), ni = wm::FrozenIcons().size(), nv = wm::FrozenVehicles().size();
        // Renames, icon replacements and vehicle meshes alone need the registry and the match lifecycle but none of the clone behaviour.
        if (g_weapons && (n || nt || ni || nv) && melange::weapons::registry::Install()) {
            if (n) {
                melange::weapons::behaviour::Install(Int("ExtraPerExplosion", 8), Bool("LogEvents", false));
                melange::weapons::contrib::Register();
            }
            melange::simbridge::OnBeforeModsLoad(&BeforeMods, nullptr);
            melange::lua50::OnContext(&OnContext, nullptr);
        }
        melange::jlog::Rec("weapons", melange::jlog::Level::Info, "installed")
            .Bool("sites", sites).Bool("bases", bases).Uint("declared", n).Uint("renames", nt).Uint("iconRules", ni).Uint("vehicleRules", nv);
        LOG_INFO("[weapons] installed: sites %s, bases %s, %zu clone(s) declared, %zu weaponText rename(s), %zu weaponIcons rule(s), %zu vehicleMeshes rule(s)",
                 sites ? "ok" : "CHANGED", bases ? "ok" : "CHANGED", n, nt, ni, nv);
        return true;
    }
};

class Assets final : public melange::Module {
public:
    const char* Name() const override { return "Assets"; }
    const char* Description() const override { return "panel icons, mod loose-file roots and mod banks"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 54; }
    bool Install() override {
        g_assets = melange::assets::upload::Available() && eng::SitesOk();
        melange::testcmd::Register("assets.stats", &VerbAssets);
        if (g_assets) {
            g_rootsPending = true;
            melange::events::Subscribe(melange::events::Event::Frame, [] {
                if (!g_rootsPending || !eng::AppReady()) return;
                g_rootsPending = false;
                AddModRoots();
                const auto t0 = std::chrono::steady_clock::now();
                size_t n = 0;
                for (auto& d : wm::Frozen()) {
                    if (d.panelIcon.empty()) continue;
                    melange::assets::PreloadPanelIcon(d.mod.c_str(), d.panelIcon.c_str());
                    ++n;
                }
                for (auto& d : wm::FrozenIcons()) {
                    if (d.panelIcon.empty()) continue;
                    melange::assets::PreloadPanelIcon(d.mod.c_str(), d.panelIcon.c_str());
                    ++n;
                }
                if (!n) return;
                LOG_INFO("[assets] %zu panel icon(s) decoded in %.3f ms", n,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                melange::assets::upload::Prepare();
            });
        }
        LOG_INFO("[assets] installed: %s", g_assets ? "ok" : "code bytes differ, inert");
        return true;
    }
};
}  // namespace

namespace melange::weapons {
bool Enabled() { return g_weapons; }

uintptr_t Container(const char* resourceName) { return engine::Lookup(resourceName); }

FieldType Field(uintptr_t container, const char* field, uint32_t* offset) {
    if (!container || !field) return FieldType::None;
    return fields::Find(engine::ClassOf(container), field, offset);
}
}  // namespace melange::weapons

namespace melange::assets {
bool Enabled() { return g_assets; }
}  // namespace melange::assets

MELANGE_MODULE(Weapons);
MELANGE_MODULE(Assets);
