// Levels: map packs, Erg Test levels and the online map gate. This file owns the module, the ini section, the hooks,
// the observers and the state verbs; registration lives in registry.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/module.h"
#include "levels/engine.h"
#include "levels/registry.h"
#include "melange/jlog.h"
#include "melange/levels.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "mods/lobby.h"

namespace {
namespace lv = melange::levels;
namespace eng = melange::levels::engine;

bool g_enabled = false, g_hooks = false;
lv::registry::Config g_cfg;

template <class Fn>
struct Observer {
    int handle;
    Fn fn;
    void* user;
};
std::vector<Observer<lv::StartFn>> g_start;
std::vector<Observer<lv::TestStateFn>> g_test;
int g_nextHandle = 1;
uint32_t g_starts = 0;

bool InLobby() { return melange::handshake::lobby::Current() != 0; }

const char* Decide(const char* frontendKey, void*) {
    const char* key = frontendKey;
    if (const char* o = lv::registry::TakeOverride(frontendKey); o && *o) {
        if (InLobby()) LOG_WARN("[levels] an override was armed in a lobby; ignored");
        else key = o;
    }
    ++g_starts;
    lv::LevelInfo info{};
    const bool known = lv::registry::Find(key, &info);
    eng::Details d;
    std::string stem = known ? info.stem : "";
    if (stem.empty() && eng::LevelDetails(key, &d)) stem = d.file;
    const lv::LevelStart s{key, stem.c_str(), known ? info.source : lv::Source::Vanilla, InLobby()};
    for (auto o : std::vector(g_start)) o.fn(s, o.user);
    melange::jlog::Rec("levels", melange::jlog::Level::Info, "level_start")
        .Str("key", key).Str("stem", stem).Bool("online", s.online).Bool("override", key != frontendKey);
    return key;
}

const char* SourceName(lv::Source s) {
    switch (s) {
        case lv::Source::Pack: return "pack";
        case lv::Source::Test: return "test";
        default: return "vanilla";
    }
}

bool VerbState(std::string_view, void*) {
    const auto st = lv::GetStats();
    char armed[80] = {};
    lv::Armed(armed, sizeof armed);
    float water = 0;
    const bool haveWater = lv::WaterLevel(&water);
    LOG_INFO("[levels] state: enabled=%d hooks=%d levelHook=%d pickerHook=%d frontend=%d current='%s' armed='%s' "
             "packs=%u levels=%u test=%u starts=%u held=%u cshDeleted=%u msRegister=%.3f water=%s%.2f online=%d",
             g_enabled, g_hooks, eng::LevelHookEnabled(), eng::PickerHookEnabled(), eng::AtFrontend(),
             eng::CurrentLevelKey(), armed, st.packs, st.levels, st.testLevels, st.starts, st.heldStarts, st.cshDeleted,
             st.msRegister, haveWater ? "" : "-", water, g_cfg.online);
    melange::jlog::Rec("levels", melange::jlog::Level::Info, "state")
        .Bool("enabled", g_enabled).Bool("hooks", g_hooks).Uint("levels", st.levels).Uint("starts", st.starts);
    return true;
}

bool VerbList(std::string_view a, void*) {
    const bool all = a.find("all") != std::string_view::npos;
    std::vector<lv::LevelInfo> v(256);
    const int n = lv::List(v.data(), static_cast<int>(v.size()), all);
    LOG_INFO("[levels] list: %d level(s)%s", n, all ? " (with vanilla)" : "");
    for (int i = 0; i < n && i < static_cast<int>(v.size()); ++i)
        LOG_INFO("[levels]   %s stem=%s mod=%s title='%s' source=%s type=%u theme=%u registered=%d", v[i].key, v[i].stem,
                 v[i].mod, v[i].title, SourceName(v[i].source), v[i].levelType, v[i].themeType, v[i].registered);
    return true;
}

class Levels final : public melange::Module {
public:
    const char* Name() const override { return "Levels"; }
    const char* Description() const override { return "map packs, Erg Test levels and the online map gate"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 56; }
    bool Install() override {
        using melange::config::EnsureKey;
        EnsureKey("Levels", "Online", "1");
        EnsureKey("Levels", "RandomPool", "0");
        EnsureKey("Levels", "DevWater", "0");
        g_cfg.online = Bool("Online", true);
        g_cfg.randomPool = Bool("RandomPool", false);
        g_cfg.devWater = Bool("DevWater", false);
        melange::testcmd::Register("levels.state", &VerbState);
        melange::testcmd::Register("levels.list", &VerbList);
        const bool sites = eng::SitesOk();
        g_enabled = sites;
        if (g_enabled) {
            lv::registry::Install(g_cfg);
            melange::events::Subscribe(melange::events::Event::Frame, [] {
                lv::registry::OnFrame();
                if (!g_hooks && lv::registry::HasModLevels()) lv::internal::InstallHooks();
            });
        }
        melange::jlog::Rec("levels", melange::jlog::Level::Info, "installed")
            .Bool("sites", sites).Bool("online", g_cfg.online).Bool("randomPool", g_cfg.randomPool);
        LOG_INFO("[levels] installed: sites %s, Online=%d RandomPool=%d DevWater=%d", sites ? "ok" : "CHANGED (inert)",
                 g_cfg.online, g_cfg.randomPool, g_cfg.devWater);
        return true;
    }
};
}  // namespace

namespace melange::levels {
bool Enabled() { return g_enabled; }

int List(LevelInfo* out, int max, bool includeVanilla) {
    return g_enabled ? registry::List(out, max, includeVanilla) : 0;
}

bool IsModLevel(const char* key) {
    LevelInfo info{};
    return g_enabled && key && registry::Find(key, &info) && info.source != Source::Vanilla;
}

bool RegisterTest(const char* stem, const char* title, char* err, size_t errLen) {
    if (!g_enabled) {
        if (err && errLen) snprintf(err, errLen, "[Levels] is disabled");
        return false;
    }
    return registry::RegisterTest(stem, title, err, errLen);
}

bool ArmNextLevel(const char* key, int timeoutS) {
    if (!g_enabled || !key || !*key || InLobby()) return false;
    return registry::Arm(key, timeoutS);
}

void Disarm() {
    if (g_enabled) registry::Disarm();
}

bool Armed(char* key, size_t keyLen) {
    if (!g_enabled) {
        if (key && keyLen) key[0] = 0;
        return false;
    }
    return registry::Armed(key, keyLen);
}

int OnTestState(TestStateFn fn, void* user) {
    if (!fn) return 0;
    g_test.push_back({g_nextHandle, fn, user});
    return g_nextHandle++;
}

void RemoveOnTestState(int handle) {
    std::erase_if(g_test, [handle](const auto& o) { return o.handle == handle; });
}

int OnLevelStart(StartFn fn, void* user) {
    if (!fn) return 0;
    g_start.push_back({g_nextHandle, fn, user});
    return g_nextHandle++;
}

void RemoveOnLevelStart(int handle) {
    std::erase_if(g_start, [handle](const auto& o) { return o.handle == handle; });
}

bool WaterLevel(float* out) {
    if (!g_enabled || !out || !sim::InMatch()) return false;
    return engine::GetFloat("Water.Level", out);
}

bool SetWaterLevelOffline(float v) {
    if (!g_enabled || !g_cfg.devWater || !sim::InMatch() || InLobby()) return false;
    const bool ok = engine::SetFloat("Water.Level", v);
    LOG_INFO("[levels] Water.Level := %.2f (dev, offline): %d", v, ok);
    return ok;
}

Online OnlineStatus(const char* key) {
    if (!melange::handshake::lobby::Current()) return Online::NotInLobby;
    return g_enabled ? registry::Status(key) : Online::Allowed;
}

Stats GetStats() {
    Stats s = g_enabled ? registry::GetStats() : Stats{};
    s.starts = g_starts;
    return s;
}
}  // namespace melange::levels

namespace melange::levels::internal {
void FireTestState(TestState s, const char* key, const char* detail) {
    for (auto o : std::vector(g_test)) o.fn(s, key ? key : "", detail ? detail : "", o.user);
}

void InstallHooks() {
    if (g_hooks || !g_enabled) return;
    g_hooks = true;
    const bool level = engine::InstallLevelHook(&Decide, nullptr);
    const bool picker = engine::InstallPickerHook(&registry::Keep);
    LOG_INFO("[levels] hooks installed: level name %s, picker %s", level ? "ok" : "FAILED", picker ? "ok" : "FAILED");
    jlog::Rec("levels", jlog::Level::Info, "hooks").Bool("level", level).Bool("picker", picker);
}
}  // namespace melange::levels::internal

MELANGE_MODULE(Levels);
