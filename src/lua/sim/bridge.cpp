// SimBridge: sim mods in the engine's Lua 5.0.1 match VM. This file owns the module, the engine hooks and the mod
// files; the per-match logic is in sim_*.cpp.
#include <windows.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/module.h"
#include "lua/engine50.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_core.h"
#include "melange/bus.h"
#include "melange/jlog.h"
#include "melange/mods.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "tools/json_read.h"

namespace l5 = melange::lua50;
namespace core = melange::simcore;

namespace {
constexpr uintptr_t kForwardList = 0x921368;  // null-terminated const char*[]: messages HandleMessage forwards to Lua
constexpr size_t kSsL = 0x38;

bool g_installed = false;
melange::simbridge::Gate g_gate = nullptr;
// Fired by NetSession regardless of whether Handshake is installed, so the fallback below can tell offline
// from online even with [Handshake] Enabled=0 (no g_gate registered at all).
bool g_inLobby = false;
std::vector<std::string> g_modIds;
std::vector<std::string> g_forwarded;
SafetyHookInline g_hInit, g_hMessage, g_hUpdate;

l5::State* StateOf(uintptr_t ss) {
    uintptr_t L = 0;
    melange::mem::SafeRead(ss + kSsL, &L, sizeof L);
    return reinterpret_cast<l5::State*>(L);
}

bool Ours(uintptr_t ss) { return ss && core::MatchL() && StateOf(ss) == core::MatchL(); }

void Enable(SafetyHookInline& h, bool on, const char* what) {
    if (!h || h.enabled() == on) return;
    const bool ok = on ? h.enable().has_value() : h.disable().has_value();
    if (!ok) LOG_ERROR("[sim] %s the %s hook failed", on ? "enabling" : "disabling", what);
}

void RefreshInitHook() {
    if (g_installed) Enable(g_hInit, core::SourceCount() > 0 || core::HasTickHooks(), "Init");
}

const std::vector<std::string>& Forwarded() {
    if (!g_forwarded.empty()) return g_forwarded;
    for (int i = 0; i < 128; ++i) {
        uintptr_t p = 0;
        char buf[128] = {};
        if (!melange::mem::SafeRead(kForwardList + i * 4, &p, 4) || !p) break;
        if (!melange::mem::SafeRead(p, buf, sizeof buf - 1)) break;
        g_forwarded.emplace_back(buf);
    }
    LOG_INFO("[sim] %zu forwarded message names", g_forwarded.size());
    return g_forwarded;
}

int __fastcall HkInit(uintptr_t ss, void*, uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4, uintptr_t a5,
                      uintptr_t a6, uintptr_t a7, uintptr_t a8, uintptr_t a9, uintptr_t a10, uintptr_t a11) {
    const int r = g_hInit.thiscall<int>(ss, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    if (!Ours(ss)) return r;
    try {
        bool open = true;
        if (core::SourceCount()) {
            if (g_gate) open = g_gate();
            // No Handshake to agree with peers on content: fail closed online, same as an unmatched hash would.
            else if (g_inLobby) open = false;
        }
        core::Init(Forwarded(), open);
    } catch (...) {
        LOG_ERROR("[sim] loading the sim mods threw");
    }
    Enable(g_hMessage, core::Active(), "HandleMessage");
    Enable(g_hUpdate, core::NeedsTickHooks(), "Update");
    return r;
}

int __stdcall HkHandleMessage(uintptr_t ss, uintptr_t msg) {
    uint16_t id = 0;
    melange::mem::SafeRead(msg + 4, &id, sizeof id);
    const int r = g_hMessage.stdcall<int>(ss, msg);
    if ((id & 0x8000) && Ours(ss)) {
        try {
            core::Message(id);
        } catch (...) {
            LOG_ERROR("[sim] message %04x: dispatch threw", id);
        }
    }
    return r;
}

int __stdcall HkUpdate(uintptr_t ss, uint32_t arg) {
    const auto t0 = std::chrono::steady_clock::now();
    const int r = g_hUpdate.stdcall<int>(ss, arg);
    if (Ours(ss)) {
        try {
            core::Update();
        } catch (...) {
            LOG_ERROR("[sim] tick %u threw", melange::sim::Tick());
        }
        core::NoteUpdateTime(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
    }
    return r;
}

void OnContext(bool created, l5::State* L, void*) {
    if (created) {
        core::ContextCreated(L);
        return;
    }
    core::ContextClosing(L);
    Enable(g_hMessage, false, "HandleMessage");
    Enable(g_hUpdate, false, "Update");
}

void Sink(const char* modId, int level, uint32_t tick, const char* text) {
    using melange::jlog::Level;
    const Level lv = level <= 0 ? Level::Debug : level == 1 ? Level::Info : level == 2 ? Level::Warn : Level::Error;
    melange::jlog::Rec("mod", lv, text).Str("mod", modId).Uint("tick", tick).Str("vm", "sim");
    LOG_INFO("[mod] %s @%u: %s", modId, tick, text);
}

std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (char c : s) w += static_cast<wchar_t>(static_cast<unsigned char>(c));
    return w;
}

bool SafeRelative(const std::string& p) {
    if (p.empty() || p.size() > 200 || p[0] == '/' || p[0] == '\\' || p.find(':') != std::string::npos) return false;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find_first_of("/\\", i);
        if (j == std::string::npos) j = p.size();
        const std::string seg = p.substr(i, j - i);
        if (seg.empty() || seg == "." || seg == "..") return false;
        i = j + 1;
    }
    return true;
}

bool ReadSource(const std::string& id, core::ModSource* out) {
    if (!SafeRelative(id) || id.find_first_of("/\\") != std::string::npos) {
        LOG_ERROR("[sim] '%s' is not a mod id", id.c_str());
        return false;
    }
    std::wstring dir;
    melange::mods::ModInfo info{};
    if (melange::mods::Find(id.c_str(), &info) && info.dir && *info.dir)
        dir = info.dir;
    else
        dir = melange::game::GameDir() + L"\\Mods\\" + Widen(id);
    melange::json::Value v;
    melange::json::Error err;
    if (!melange::json::ParseFile(dir + L"\\spice.json", &v, &err)) {
        LOG_ERROR("[sim] %s: spice.json: %d:%d %s", id.c_str(), err.line, err.col, err.text.c_str());
        return false;
    }
    const auto* entry = v.Get("entry");
    const auto* sim = entry ? entry->Get("sim") : nullptr;
    const auto* ver = v.Get("version");
    if (!sim || !sim->IsString() || !SafeRelative(sim->string)) {
        LOG_ERROR("[sim] %s: spice.json has no usable entry.sim", id.c_str());
        return false;
    }
    std::string rel = sim->string;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    std::wstring path = dir + L"\\" + Widen(rel);
    std::replace(path.begin(), path.end(), L'/', L'\\');
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        LOG_ERROR("[sim] %s: cannot open entry.sim '%s'", id.c_str(), rel.c_str());
        return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string code;
    bool ok = size.QuadPart <= (1 << 20);
    if (ok) {
        code.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = code.empty() || (ReadFile(f, code.data(), static_cast<DWORD>(code.size()), &got, nullptr) && got == code.size());
    }
    CloseHandle(f);
    if (!ok) {
        LOG_ERROR("[sim] %s: cannot read entry.sim '%s' (1 MB limit)", id.c_str(), rel.c_str());
        return false;
    }
    out->id = id;
    out->version = ver && ver->IsString() ? ver->string : "0.0.0";
    out->chunkName = "@" + id + "/" + rel;
    out->code = std::move(code);
    return true;
}

void ApplyModList() {
    std::vector<core::ModSource> src;
    for (auto& id : g_modIds) {
        core::ModSource s;
        if (ReadSource(id, &s)) src.push_back(std::move(s));
    }
    LOG_INFO("[sim] sim mod list: %zu of %zu readable", src.size(), g_modIds.size());
    core::SetSources(std::move(src));
}

bool VerbStats(std::string_view, void*) {
    const auto s = melange::sim::GetStats();
    const auto c = core::GetCounters();
    LOG_INFO("[sim] stats: inMatch=%d serial=%u ticks=%u simMods=%u faults=%u usLast=%.1f usP95=%.1f heapKB=%u | "
             "refs=%u subs=%u timers=%u tickHooks=%u console=%u depthDrops=%u usBridge=%.1f | hooks: init=%d "
             "message=%d update=%d",
             melange::sim::InMatch(), melange::sim::MatchSerial(), s.ticks, s.simMods, s.faults, s.usLastTick,
             s.usP95Tick, s.heapKB, c.refs, c.subs, c.timers, c.tickHooks, c.consoleEnvs, c.dispatchDepthDrops,
             c.usBridgeLast, g_hInit && g_hInit.enabled(), g_hMessage && g_hMessage.enabled(),
             g_hUpdate && g_hUpdate.enabled());
    melange::jlog::Rec("sim", melange::jlog::Level::Info, "stats")
        .Uint("ticks", s.ticks).Uint("simMods", s.simMods).Uint("faults", s.faults).Float("usLast", s.usLastTick)
        .Float("usP95", s.usP95Tick).Uint("heapKB", s.heapKB).Uint("refs", c.refs).Uint("subs", c.subs)
        .Uint("timers", c.timers).Bool("hookInit", g_hInit && g_hInit.enabled())
        .Bool("hookMessage", g_hMessage && g_hMessage.enabled()).Bool("hookUpdate", g_hUpdate && g_hUpdate.enabled());
    return true;
}

bool VerbMods(std::string_view, void*) {
    std::string listed, loaded, msgs;
    for (auto& id : g_modIds) listed += " " + id;
    for (auto& id : melange::simbridge::LoadedMods()) loaded += " " + id;
    for (auto& [n, id] : melange::simbridge::ModMessages()) {
        char b[16];
        snprintf(b, sizeof b, "=%04x", id);
        msgs += " " + n + b;
    }
    LOG_INFO("[sim] mods: listed:%s | loaded:%s | messages:%s%s", listed.empty() ? " -" : listed.c_str(),
             loaded.empty() ? " -" : loaded.c_str(), msgs.empty() ? " -" : msgs.c_str(),
             core::ModMessagesFrozen() ? " (frozen)" : "");
    return true;
}

class SimBridge final : public melange::Module {
public:
    const char* Name() const override { return "SimBridge"; }
    const char* Description() const override { return "sim mods in the match's Lua VM"; }
    bool RequiresKnownBuild() const override { return true; }
    int Order() const override { return 52; }
    bool Install() override {
        core::Config cfg;
        cfg.instrPerCall = std::clamp(Int("InstrPerCall", 200000), 1000, 50000000);
        cfg.logTicks = Bool("LogTicks", false);
        core::Configure(cfg);
        if (!l5::Check()) {
            LOG_WARN("[sim] engine Lua check failed: SimBridge stays inert");
            return true;
        }
        if (!l5::Track()) {
            LOG_ERROR("[sim] match VM tracking failed: SimBridge stays inert");
            return true;
        }
        using F = safetyhook::InlineHook::Flags;
        g_hInit = safetyhook::create_inline(l5::kInit, &HkInit, F::StartDisabled);
        g_hMessage = safetyhook::create_inline(l5::kHandleMessage, &HkHandleMessage, F::StartDisabled);
        g_hUpdate = safetyhook::create_inline(l5::kUpdate, &HkUpdate, F::StartDisabled);
        if (!g_hInit || !g_hMessage || !g_hUpdate) {
            LOG_ERROR("[sim] creating the Init/HandleMessage/Update hooks failed: SimBridge stays inert");
            g_hInit = {};
            g_hMessage = {};
            g_hUpdate = {};
            return true;
        }
        core::SetLogSink(&Sink);
        core::SetHooksChanged(&RefreshInitHook);
        l5::OnContext(&OnContext, nullptr);
        melange::events::Subscribe(melange::events::Event::LobbyEnter, [] { g_inLobby = true; });
        melange::events::Subscribe(melange::events::Event::LobbyLeave, [] { g_inLobby = false; });
        melange::testcmd::Register("sim.stats", &VerbStats);
        melange::testcmd::Register("sim.mods", &VerbMods);
        g_installed = true;
        if (!g_modIds.empty()) ApplyModList();
        RefreshInitHook();
        LOG_INFO("[sim] installed (instrPerCall=%d, %zu sim mods listed)", cfg.instrPerCall, g_modIds.size());
        return true;
    }
    void Uninstall() override {
        g_installed = false;
        g_hUpdate = {};
        g_hMessage = {};
        g_hInit = {};
    }
};
}  // namespace

namespace melange::simbridge {
void SetModList(const std::vector<std::string>& idsInLoadOrder) {
    g_modIds = idsInLoadOrder;
    if (g_installed) ApplyModList();
}

void SetGate(Gate g) { g_gate = g; }
}  // namespace melange::simbridge

namespace melange::sim {
bool RegisterModMessage(const char* name, uint16_t* idOut) {
    static std::vector<std::string> prefixes;
    if (prefixes.empty()) {
        if (!bus::RegistryReady()) {
            LOG_WARN("[sim] mod message '%s' refused: the engine's message registry is not ready", name ? name : "");
            return false;
        }
        std::set<std::string> s;
        bus::ForEachName([&s](bus::MsgId, const char* n) {
            const char* dot = std::strchr(n, '.');
            if (dot) s.emplace(n, dot);
        });
        prefixes.assign(s.begin(), s.end());
    }
    return core::RegisterModMessage(name, idOut, prefixes);
}

void FreezeModMessages() { core::FreezeModMessages(); }
}  // namespace melange::sim

MELANGE_MODULE(SimBridge);
