// Sandbox module: runs the client VM from the frame loop and connects it to Thumper, the bus, modfs and the game.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <set>

#include "core/events.h"
#include "core/game.h"
#include "core/log.h"
#include "core/module.h"
#include "lua/engine50.h"
#include "lua/sandbox_core.h"
#include "lua/sandbox_internal.h"
#include "melange/bus.h"
#include "melange/jlog.h"
#include "melange/lua.h"
#include "melange/mods.h"
#include "melange/sim.h"
#include "melange/testcmd.h"
#include "render/mirage/modfs.h"
#include "tools/json_mini.h"

namespace melange::sandbox {
double NowSeconds() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart) / freq;
}

namespace {
std::map<std::string, bus::SubId> g_busSubs;
std::atomic<bool> g_modsDirty{true};
bool g_hotReload = true;
bool g_lua50 = false;
bool g_booted = false;
bool g_turnSeen = false;
bool g_onlineMatch = false;
void* g_lastVm = nullptr;
std::set<std::wstring> g_watched;
std::set<std::string> g_pendingReload;

class PayloadOut final : public bus::JsonOut {
public:
    json::Value obj;
    PayloadOut() { obj.type = json::Type::Object; }
    void Int(const char* k, int64_t v) override { Num(k, static_cast<double>(v)); }
    void Uint(const char* k, uint64_t v) override { Num(k, static_cast<double>(v)); }
    void Hex(const char* k, uint64_t v) override {
        char b[24];
        snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
        Str(k, b);
    }
    void Float(const char* k, double v) override { Num(k, v); }
    void Str(const char* k, std::string_view v) override {
        json::Value x;
        x.type = json::Type::String;
        x.string.assign(v);
        obj.members.emplace_back(k, std::move(x));
    }
    void Vec3(const char* k, const float v[3]) override {
        json::Value a;
        a.type = json::Type::Array;
        for (int i = 0; i < 3; ++i) {
            json::Value x;
            x.type = json::Type::Number;
            x.number = v[i];
            a.items.push_back(x);
        }
        obj.members.emplace_back(k, std::move(a));
    }

private:
    void Num(const char* k, double v) {
        json::Value x;
        x.type = json::Type::Number;
        x.number = v;
        obj.members.emplace_back(k, std::move(x));
    }
};

void OnBus(const bus::MessageView& m, void*) {
    try {
        PayloadOut out;
        bus::Decode(m, out);
        QueueEvent(m.name, std::move(out.obj));
    } catch (...) {
    }
}

void OnTurnStarted(const bus::MessageView&, void*) {
    ++Game().turn;
    g_turnSeen = true;
}

json::Value Obj(std::initializer_list<std::pair<const char*, json::Value>> kv) {
    json::Value o;
    o.type = json::Type::Object;
    for (auto& [k, v] : kv) o.members.emplace_back(k, v);
    return o;
}
json::Value Str(const std::string& s) {
    json::Value v;
    v.type = json::Type::String;
    v.string = s;
    return v;
}
json::Value Bool(bool b) {
    json::Value v;
    v.type = json::Type::Bool;
    v.boolean = b;
    return v;
}

void UpdateGame() {
    GameState& g = Game();
    if (!g_booted && events::FrameCount() >= 30 && (!bus::Installed() || bus::RegistryReady())) g_booted = true;
    void* vm = g_lua50 ? lua50::MatchState() : nullptr;
    if (vm != g_lastVm) {
        g_lastVm = vm;
        g.turn = 0;
        g_turnSeen = false;
    }
    const bool inMatch = g_lua50 ? vm != nullptr : g_onlineMatch;
    const char* scene = !g_booted ? "boot" : !inMatch ? "menu" : (g_lua50 && !g_turnSeen) ? "loading" : "match";
    g.online = g_onlineMatch;
    g.tick = sim::Tick();
    if (inMatch != g.inMatch) {
        g.inMatch = inMatch;
        QueueEvent(inMatch ? "melange.match.start" : "melange.match.end", Obj({{"online", Bool(g.online)}}));
    }
    if (g.scene != scene) {
        g.scene = scene;
        QueueEvent("melange.scene", Obj({{"scene", Str(scene)}}));
    }
}

void WatchMod(const ModRec* m) {
    if (!g_hotReload) return;
    const size_t slash = m->entryClient.find_first_of("/\\");
    if (slash == std::string::npos) {
        SysLog(1, "hot reload needs entry.client in a subfolder (e.g. client/init.lua)", m->id);
        return;
    }
    const std::wstring sub = Widen(m->entryClient.substr(0, slash));
    if (!g_watched.insert(sub).second) return;
    mirage::modfs::Watch(sub.c_str(), [](const wchar_t* path, void*) {
        const std::wstring p = path;
        if (p.size() < 4 || _wcsicmp(p.c_str() + p.size() - 4, L".lua") != 0) return;
        for (ModRec* m : LoadedMods()) {
            const std::wstring pre = m->dir + L"\\";
            if (p.size() > pre.size() && _wcsnicmp(p.c_str(), pre.c_str(), pre.size()) == 0) g_pendingReload.insert(m->id);
        }
    }, nullptr);
}

void Reconcile() {
    std::vector<mods::ModInfo> list(256);
    const int total = mods::List(list.data(), static_cast<int>(list.size()));
    list.resize(static_cast<size_t>(std::clamp(total, 0, 256)));
    std::set<std::string> seen;
    for (const mods::ModInfo& info : list) {
        if (!info.id) continue;
        seen.insert(info.id);
        const bool want = info.hasClient && (info.state == mods::State::Enabled || info.state == mods::State::PendingConsent);
        ModRec* r = FindMod(info.id);
        const bool loaded = r && r->gen;
        if (want && !loaded) {
            LoadMod(info.id);
        } else if (want && loaded && (r->unsafe != info.unsafe || r->granted != (info.unsafe && info.unsafeGranted))) {
            ReloadMod(info.id);
        } else if (!want && loaded) {
            UnloadMod(info.id);
        }
        if (ModRec* m = FindMod(info.id); m && m->gen) WatchMod(m);
    }
    for (ModRec* m : LoadedMods())
        if (!seen.count(m->id)) UnloadMod(m->id.c_str());
    QueueEvent("melange.mods.changed", Obj({}));
}

void OnFrame() {
    if (!Running()) return;
    try {
        UpdateGame();
        if (g_modsDirty.exchange(false)) Reconcile();
        if (!g_pendingReload.empty()) {
            std::set<std::string> ids;
            ids.swap(g_pendingReload);
            for (const std::string& id : ids) ReloadMod(id.c_str());
        }
        Frame();
    } catch (const std::exception& e) {
        SysLog(3, "frame dispatch failed", {}, e.what());
    }
}

// ---------------------------------------------------------------- test verbs
bool VerbEval(std::string_view args, void*) {
    std::string code(args);
    const char* mod = nullptr;
    std::string id;
    if (!code.empty() && code[0] == '@') {
        const size_t sp = code.find(' ');
        id = code.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
        code = sp == std::string::npos ? "" : code.substr(sp + 1);
        mod = id.c_str();
    }
    const EvalOut r = Eval(mod, code);
    jlog::Rec("sandbox", r.ok ? jlog::Level::Info : jlog::Level::Warn, "lua.eval")
        .Str("mod", id)
        .Bool("ok", r.ok)
        .Str("code", code)
        .Str("text", r.text)
        .Emit();
    LOG_INFO("[sandbox] lua.eval %s: %s", r.ok ? "ok" : "error", r.text.c_str());
    return r.ok;
}

bool VerbReload(std::string_view args, void*) {
    const std::string id(args);
    const bool ok = ReloadMod(id.c_str());
    LOG_INFO("[sandbox] lua.reload %s: %s", id.c_str(), ok ? "ok" : "failed");
    return ok;
}

bool VerbStats(std::string_view, void*) {
    const lua::Stats s = lua::GetStats();
    jsonmini::Arr mods;
    for (ModRec* m : LoadedMods()) {
        ModStatus st;
        Status(m->id.c_str(), &st);
        std::map<std::string, int> kinds;
        for (uint32_t id : m->gen->callbacks)
            if (Callback* cb = FindCallback(id)) ++kinds[KindName(cb->kind)];
        jsonmini::Obj k;
        for (auto& [n, c] : kinds) k.Int(n, c);
        mods.Raw(jsonmini::Obj()
                     .Str("id", m->id)
                     .UInt("callbacks", st.callbacks)
                     .UInt("disabled", st.disabledCallbacks)
                     .UInt("faults", st.faults)
                     .UInt("bytes", st.bytes)
                     .UInt("instructions", st.instructions)
                     .Raw("handles", k.End())
                     .End());
    }
    jlog::Rec("sandbox", jlog::Level::Info, "lua.stats")
        .Uint("mods", s.mods)
        .Uint("callbacks", s.callbacks)
        .Uint("faults", s.faults)
        .Uint("disabledCallbacks", s.disabledCallbacks)
        .Uint("instructions", s.instructions)
        .Uint("bytes", s.bytes)
        .Float("msLastFrame", s.msLastFrame)
        .Raw("perMod", mods.End())
        .Emit();
    LOG_INFO("[sandbox] lua.stats mods=%u callbacks=%u faults=%u disabled=%u bytes=%llu ms=%.3f", s.mods, s.callbacks,
             s.faults, s.disabledCallbacks, static_cast<unsigned long long>(s.bytes), s.msLastFrame);
    return true;
}

bool VerbApi(std::string_view, void*) {
    jsonmini::Arr a;
    for (const std::string& n : ApiNames()) a.Str(n);
    jlog::Rec("sandbox", jlog::Level::Info, "lua.api").Raw("names", a.End()).Emit();
    return true;
}

class Sandbox final : public Module {
public:
    const char* Name() const override { return "Sandbox"; }
    const char* Description() const override { return "Lua 5.4 scripting for client mods"; }
    int Order() const override { return 50; }
    bool Install() override {
        Limits lim;
        lim.instrPerCall = std::max(10000, Int("InstrPerCall", 500000));
        lim.modBytes = static_cast<size_t>(std::clamp(Int("ModMemoryMB", 16), 1, 512)) << 20;
        lim.totalBytes = static_cast<size_t>(std::clamp(Int("TotalMemoryMB", 96), 8, 1024)) << 20;
        lim.hotReload = Bool("HotReload", true);
        g_hotReload = lim.hotReload;
        if (!Start(lim)) return false;
        g_lua50 = game::IsKnownBuild() && lua50::Check();
        if (bus::Installed()) bus::SubscribeName("GameLogic.Turn.Started", bus::Path::Post, &OnTurnStarted);
        events::Subscribe(events::Event::Frame, [] { OnFrame(); });
        events::Subscribe(events::Event::Shutdown, [] { FlushAllStorage(); });
        events::Subscribe(events::Event::MatchStart, [] { g_onlineMatch = true; });
        events::Subscribe(events::Event::MatchEnd, [] { g_onlineMatch = false; });
        mods::OnChange([](void*) { g_modsDirty = true; }, nullptr);
        testcmd::Register("lua.eval", &VerbEval);
        testcmd::Register("lua.reload", &VerbReload);
        testcmd::Register("lua.stats", &VerbStats);
        testcmd::Register("lua.api", &VerbApi);
        return true;
    }
    void Uninstall() override { Stop(); }
};
}  // namespace

void EngineSubscribe(const std::string& name, bool on) {
    if (on) {
        if (g_busSubs.count(name) || !bus::Installed()) return;
        if (bus::SubId id = bus::SubscribeName(name.c_str(), bus::Path::Post, &OnBus)) g_busSubs[name] = id;
        else SysLog(2, "engine message subscription failed", Current() ? Current()->id : "", name);
    } else if (auto it = g_busSubs.find(name); it != g_busSubs.end()) {
        bus::Unsubscribe(it->second);
        g_busSubs.erase(it);
    }
}

MELANGE_MODULE(Sandbox);
}  // namespace melange::sandbox
