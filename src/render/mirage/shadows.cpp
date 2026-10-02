// Module "MirageShadows": the engine's shadow-map size (the /SHADOWMAP cfg option) from enabled mods' requests.
// Off by default: with no request and [MirageShadows] ShadowMapSize=auto the engine keeps its own cfg value.
#include <windows.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <safetyhook.hpp>

#include "core/config.h"
#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "melange/compat.h"
#include "melange/graphics.h"
#include "melange/mods.h"
#include "melange/testcmd.h"
#include "render/mirage/engine.h"
#include "render/mirage/shadows_logic.h"
#include "render/mirage/stages.h"

namespace melange::mirage::shadows {
namespace {
namespace logic = melange::mirage::shadows::logic;
// Build #1077: g_Config holds the size as a u16 at +0x88; the rebuild function creates the shadow targets from it
// (called once at display init, and by the engine's own shadow-resolution debug command).
constexpr uintptr_t kConfigPtr = 0x95a100, kSizeOffset = 0x88, kRebuild = 0x4d66d0;

std::mutex g_mx;
int g_ini = -1;
logic::Effective g_eff;
std::map<std::string, int> g_runtime;  // mod id -> runtime request
int g_vanilla = 0;
bool g_checked = false, g_ok = false, g_rebuildSeen = false, g_dirty = false;
safetyhook::MidHook g_hook;

uintptr_t Config() {
    uintptr_t c = 0;
    mem::SafeRead(kConfigPtr, &c, 4);
    return c;
}

int ReadSize() {
    uintptr_t c = Config();
    uint16_t v = 0;
    return c && mem::SafeRead(c + kSizeOffset, &v, 2) ? v : 0;
}

bool WriteSize(int s) {
    uintptr_t c = Config();
    if (!c || s <= 0) return false;
    *reinterpret_cast<volatile uint16_t*>(c + kSizeOffset) = static_cast<uint16_t>(s);
    return true;
}

bool Check() {
    if (g_checked) return g_ok;
    g_checked = true;
    g_ok = game::IsKnownBuild() && engine::Check() && mem::Expect(kRebuild, {0x6A, 0xFF, 0x68, 0xF0, 0xD1, 0x7C, 0x00}) &&
           mem::Expect(kRebuild + 0x2f, {0xA1, 0x00, 0xA1, 0x95, 0x00, 0x0F, 0xB7, 0x80, 0x88, 0x00, 0x00, 0x00});
    if (!g_ok) {
        LOG_WARN("[mirage] [MirageShadows] shadow-map code not recognised; the size stays the engine's");
        compat::Report(compat::Kind::Feature, "shadowmap", compat::Status::Failed, "engine shadow-map code not recognised", "builtin");
    }
    return g_ok;
}

void CaptureVanilla() {
    if (g_vanilla > 0) return;
    int v = ReadSize();
    if (v > 0) g_vanilla = v;
}

void OnRebuild(safetyhook::Context&) {
    std::lock_guard lk(g_mx);
    CaptureVanilla();
    g_rebuildSeen = true;
    if (g_eff.size > 0 && ReadSize() != g_eff.size) WriteSize(g_eff.size);
}

bool InstallHook() {
    if (g_hook) return true;
    if (!Check()) return false;
    g_hook = safetyhook::create_mid(kRebuild, &OnRebuild);
    if (!g_hook) {
        LOG_WARN("[mirage] [MirageShadows] hook installation failed; the size stays the engine's");
        compat::Report(compat::Kind::Feature, "shadowmap", compat::Status::Failed, "hook installation failed", "builtin");
        return false;
    }
    return true;
}

std::string Describe() {
    char b[160];
    snprintf(b, sizeof b, "effective=%d (%s) vanilla=%d engine=%d", g_eff.size,
             g_eff.size == 0 ? "vanilla" : g_ini >= 0 ? "ini override" : "mod-requested", g_vanilla, ReadSize());
    return b;
}

void Recompute() {
    std::vector<int> reqs;
    mods::ModInfo info[256];
    int n = mods::List(info, 256);
    for (int i = 0; i < n; ++i) {
        if (info[i].state != mods::State::Enabled) continue;
        auto rt = g_runtime.find(info[i].id);
        reqs.push_back(logic::ModRequest(mods::ShadowMapRequest(info[i].id), rt == g_runtime.end() ? -1 : rt->second));
    }
    logic::Effective before = g_eff;
    g_eff = logic::Merge(g_ini, reqs);
    if (before.size != g_eff.size) g_dirty = true;
}

void OnModsChanged(void*) {
    std::lock_guard lk(g_mx);
    Recompute();
    if (g_eff.size > 0) InstallHook();
}

// Main thread, between frames: the same rebuild the engine's own resolution switch performs.
void OnFrame() {
    int target;
    {
        std::lock_guard lk(g_mx);
        if (!g_dirty) return;
        if (!g_rebuildSeen && (events::FrameCount() < 30 || !engine::Rm())) return;
        g_dirty = false;
        CaptureVanilla();
        target = logic::Target(g_eff, g_vanilla);
        if (target <= 0 || ReadSize() == target) return;
        if (g_eff.size > 0 && !InstallHook()) return;
        WriteSize(target);
        g_rebuildSeen = true;
    }
    reinterpret_cast<void(__cdecl*)()>(kRebuild)();
    LOG_INFO("[mirage] shadow map rebuilt at %d", ReadSize());
}

bool VerbInfo(std::string_view, void*) {
    std::lock_guard lk(g_mx);
    LOG_INFO("[mirage] shadowmap: %s hooked=%d", Describe().c_str(), static_cast<bool>(g_hook));
    return true;
}

class MirageShadows final : public melange::Module {
public:
    const char* Name() const override { return "MirageShadows"; }
    const char* Description() const override { return "shadow-map size requested by mods"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 47; }

    bool Install() override {
        if (!melange::mirage::stages::CoreEnabled(Name())) return false;
        melange::config::EnsureKey(Name(), "ShadowMapSize", "auto");
        std::string s = melange::config::GetString(Name(), "ShadowMapSize", "auto");
        if (!logic::ParseIni(s, &g_ini)) {
            LOG_WARN("[mirage] [MirageShadows] ShadowMapSize=%s not understood (auto, vanilla, 512, 1024, 2048 or 4096); using auto",
                     s.c_str());
            g_ini = -1;
        }
        Check();
        {
            std::lock_guard lk(g_mx);
            Recompute();
            g_dirty = false;
        }
        melange::testcmd::Register("mirage.shadows", &VerbInfo);
        melange::mods::OnChange(&OnModsChanged, nullptr);
        melange::events::Subscribe(melange::events::Event::Frame, [] { OnFrame(); });
        if (g_eff.size == 0) {
            compat::Report(compat::Kind::Feature, "shadowmap", compat::Status::Skipped, "no mod request and no override", "builtin");
            LOG_INFO("[mirage] shadow map: vanilla (no mod request and no [MirageShadows] override)");
            return true;
        }
        if (InstallHook()) {
            compat::Report(compat::Kind::Feature, "shadowmap", compat::Status::Loaded, std::to_string(g_eff.size).c_str(), "builtin");
            LOG_INFO("[mirage] shadow map: %d (%s)", g_eff.size, g_ini >= 0 ? "ini override" : "mod-requested");
        }
        return true;
    }
};
}  // namespace

MELANGE_MODULE(MirageShadows);
}  // namespace melange::mirage::shadows

namespace melange::graphics {
namespace sh = melange::mirage::shadows;

bool SetShadowMapRequest(const char* mod, int size) {
    if (!mod || !*mod || (size > 0 && !sh::logic::ValidSize(size)) || size < -1) return false;
    if (!sh::Check()) return false;
    std::lock_guard lk(sh::g_mx);
    if (size < 0) sh::g_runtime.erase(mod);
    else sh::g_runtime[mod] = size;
    sh::Recompute();
    if (sh::g_eff.size > 0) sh::InstallHook();
    return true;
}

ShadowMapInfo GetShadowMapInfo() {
    bool ok = sh::Check();
    std::lock_guard lk(sh::g_mx);
    return {sh::ReadSize(), sh::g_eff.size, sh::g_vanilla, sh::g_eff.anyModRequest, ok};
}
}  // namespace melange::graphics
