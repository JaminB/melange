#include "weapons/registry.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#include "assets/icons.h"
#include "assets/searchpath.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "melange/assets.h"
#include "melange/bus.h"
#include "melange/jlog.h"
#include "melange/sim.h"
#include "mods/thumper_internal.h"
#include "weapons/engine.h"
#include "weapons/hud.h"
#include "weapons/manifest.h"
#include "weapons/panel.h"
#include "weapons/registry_core.h"
#include "weapons/vid.h"

namespace melange::weapons {
namespace {
class GameEngine final : public core::Engine {
public:
    uintptr_t Lookup(const char* name) override { return engine::Lookup(name); }
    uintptr_t LookupDesc(const char* name) override { return engine::LookupDesc(name); }
    int AddResource(const char* name, uintptr_t obj, uint32_t flags) override { return engine::AddResource(name, obj, 0, flags); }
    int LoadModBank(const char* mod, const char* rel, std::string* err) override {
        char b[256] = {};
        const int r = assets::LoadModBank(mod, rel, b, sizeof b);
        if (err) *err = b;
        return r;
    }
    uintptr_t ClassOf(uintptr_t c) override { return engine::ClassOf(c); }
    FieldType Field(uintptr_t c, const char* f, uint32_t* off) override { return weapons::Field(c, f, off); }
    bool Read(uintptr_t a, void* out, size_t n) override { return mem::SafeRead(a, out, n); }
    bool Write(uintptr_t a, const void* data, size_t n) override { return mem::Write(a, data, n); }
    bool AssignString(uintptr_t field, const char* s) override { return engine::AssignXString(field, s); }
    bool ReadXString(uintptr_t field, std::string* out) override {
        uintptr_t p = 0;
        if (!mem::SafeRead(field, &p, sizeof p) || !p) return false;
        *out = engine::XStringValue(field);
        return true;
    }
    int AddText(const char* key, const char* value) override { return engine::AddString(key, value, 0, 1); }
    bool GetText(const char* key, std::string* out) override { return engine::TextOf(key, out); }
    uint32_t ReserveIcon(const char* mod, const char* rel, std::string* err) override {
        char b[256] = {};
        uint32_t code = 0;
        if (!assets::ReservePanelIcon(mod, rel, &code, b, sizeof b)) code = 0;
        if (err) *err = b;
        return code;
    }
    bool HudUsable(const char* mod) override {
        std::string want = "mods/";
        for (const char* p = mod; *p; ++p) want.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
        want += "/assets/loose";
        for (const auto& a : assets::searchpath::Added())
            if (a == want || (a.size() > want.size() && a.compare(a.size() - want.size(), want.size(), want) == 0 &&
                              a[a.size() - want.size() - 1] == '/'))
                return true;
        LOG_WARN("[weapons] %s: its assets/loose folder is not a search path, so the HUD keeps the base's icon", mod);
        return false;
    }
    bool HudFileExists(const char* mod, const char* file) override {
        thumper::Entry e;
        if (!thumper::FindEntry(mod, &e)) return false;
        const std::wstring path = e.dir + L"\\" + game::Widen(e.manifest.assetsRoot) + L"\\loose\\" + game::Widen(file);
        const DWORD a = GetFileAttributesW(path.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
    bool PatchPanelIcon(const char* mod, const char* rel, uint32_t iconCode, std::string* err) override {
        char b[256] = {};
        const bool ok = assets::PatchVanillaPanelIcon(mod, rel, iconCode, b, sizeof b);
        if (err) *err = b;
        return ok;
    }
    void ClearPanelIcons() override { assets::ClearVanillaPanelIcons(); }
    bool EnableHooks(bool on) override {
        if (on && engine::Suppressed()) {
            LOG_ERROR("[weapons] the weapon hooks are suppressed: no clones in this match");
            return false;
        }
        // The selection hooks are for clones; a rename-only match needs just the panel text hooks.
        const bool a = registry::Core().Count() == 0 || vid::Enable(on), b = panel::Enable(on), c = hud::Enable(on);
        return a && b && c;
    }
    uint32_t Tick() override { return sim::Tick(); }
};

GameEngine g_engine;
core::Registry g_core(g_engine);
bool g_configured = false, g_installed = false;

void OnTurnEnded(const bus::MessageView&, void*) { g_core.TurnEnded(); }

void Fill(const manifest::CloneDecl& d, CloneInfo* o) {
    *o = CloneInfo{};
    o->k = d.k;
    o->vid = kVidBase + d.k;
    o->base = d.baseId;
    strncpy_s(o->name, d.name.c_str(), _TRUNCATE);
    strncpy_s(o->mod, d.mod.c_str(), _TRUNCATE);
    o->cell = static_cast<int8_t>(d.cell);
}
}  // namespace

int Declared(CloneInfo* out, int max) {
    if (g_configured) {
        for (int k = 0; out && k < std::min(g_core.Count(), max); ++k) out[k] = g_core.At(k)->info;
        return g_core.Count();
    }
    const auto& all = manifest::Frozen();
    const int n = static_cast<int>(all.size());
    for (int i = 0; out && i < std::min(n, max); ++i) Fill(all[i], &out[i]);
    return n;
}

bool Live() { return g_core.Live(); }

int ActiveClone() { return g_core.Active(); }

bool IsVid(int32_t v) { return v >= kVidBase && v < kVidBase + kMaxClones; }

int32_t BaseOf(int32_t v) {
    const int n = g_configured ? g_core.Count() : static_cast<int>(manifest::Frozen().size());
    if (IsVid(v) && v - kVidBase < n)
        return g_configured ? g_core.At(v - kVidBase)->info.base : manifest::Frozen()[v - kVidBase].baseId;
    LOG_ERROR("[weapons] unknown virtual id %x mapped to the Bazooka", static_cast<unsigned>(v));
    return core::kFallbackBase;
}

namespace registry {
core::Registry& Core() { return g_core; }

bool Install() {
    if (g_installed) return true;
    g_core.Configure(manifest::Frozen(), manifest::FrozenText(), manifest::FrozenIcons());
    g_configured = true;
    const bool v = g_core.Count() == 0 || vid::Create(), p = panel::Create(), h = hud::Create();
    if (!(v && p && h)) {
        LOG_ERROR("[weapons] creating the weapon hooks failed (selection %d, panel %d, HUD %d): no clones or renames this launch", v, p, h);
        return false;
    }
    if (!bus::SubscribeName("GameLogic.Turn.Ended", bus::Path::Post, &OnTurnEnded))
        LOG_WARN("[weapons] no GameLogic.Turn.Ended subscription: a swapped name slot is restored only on reselection");
    g_installed = true;
    return true;
}

void OnInit() {
    if (!g_installed) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    const bool live = g_core.Init(&why);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const bool clones = g_core.Count() > 0;
    assets::icons::Activate(live && clones);
    if (clones) {
        if (live)
            LOG_INFO("[weapons] match %u: %d clone(s) live in %.3f ms", sim::MatchSerial(), g_core.Count(), ms);
        else
            LOG_ERROR("[weapons] match %u: no clones in this match: %s", sim::MatchSerial(), why.c_str());
        jlog::Rec("weapons", live ? jlog::Level::Info : jlog::Level::Error, "clones")
            .Bool("live", live).Int("count", g_core.Count()).Float("ms", ms).Str("why", why);
    }
    if (g_core.TextCount()) {
        int applied = 0;
        for (int i = 0; i < g_core.TextCount(); ++i) applied += g_core.TextAt(i)->id >= 0;
        LOG_INFO("[weapons] match %u: %d of %d weaponText rename(s) live", sim::MatchSerial(), applied, g_core.TextCount());
        jlog::Rec("weapons", jlog::Level::Info, "weapon_text")
            .Bool("live", g_core.TextLive()).Int("declared", g_core.TextCount()).Int("applied", applied);
    }
    if (g_core.IconCount()) {
        int panels = 0, huds = 0;
        for (int i = 0; i < g_core.IconCount(); ++i) {
            panels += g_core.IconAt(i)->panel;
            huds += g_core.IconAt(i)->hud;
        }
        LOG_INFO("[weapons] match %u: weaponIcons %d panel and %d HUD icon(s) armed of %d rule(s)", sim::MatchSerial(), panels, huds,
                 g_core.IconCount());
        jlog::Rec("weapons", jlog::Level::Info, "weapon_icons")
            .Bool("live", g_core.IconsLive()).Int("declared", g_core.IconCount()).Int("panel", panels).Int("hud", huds);
    }
}

void OnMatchEnd() {
    if (g_installed) g_core.MatchEnd();
    assets::icons::Activate(false);
}

const CloneInfo* ByDesc(uintptr_t desc) { return g_core.ByDesc(desc); }
}  // namespace registry
}  // namespace melange::weapons
