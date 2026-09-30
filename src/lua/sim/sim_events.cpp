// The per-match lifecycle of the sim bridge: loading mods at Init, forwarded messages, the tick, timers, faults,
// C++ tick hooks, and mod message names.
#include <algorithm>
#include <chrono>
#include <cstring>

#include "core/log.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"

namespace melange::simcore {
Match g;
Config g_cfg;

namespace {
using Clock = std::chrono::steady_clock;
std::vector<ModSource> g_sources;
std::vector<LevelSource> g_levelSources;
std::string g_levelKey;
uint32_t g_serial = 0;
LogSink g_sink = nullptr;
void (*g_hooksChanged)() = nullptr;
std::vector<TickHook> g_tickHooks;
int g_nextTickHook = 1;
struct MatchObserver {
    int handle;
    simbridge::MatchFn fn;
    void* user;
};
std::vector<MatchObserver> g_matchObservers;
int g_nextObserver = 1;
struct BeforeLoad {
    int handle;
    simbridge::InitFn fn;
    void* user;
};
std::vector<BeforeLoad> g_beforeLoad;
std::vector<std::pair<std::string, uint16_t>> g_modMessages;
bool g_frozen = false;
double g_usLast = 0, g_usBridge = 0;
double g_ring[256];
uint32_t g_ringCount = 0;

size_t LiveCallbacks(int mod) {
    size_t n = 0;
    for (auto& [h, s] : g.subs) n += s.mod == mod && !s.dead;
    for (auto& [h, t] : g.timers) n += t.mod == mod && !t.dead;
    return n;
}

void Kill(Callback& cb) {
    cb.dead = true;
    DropRef(cb.fnRef);
}

void Compact() {
    if (g_depth) return;
    std::erase_if(g.subs, [](const auto& kv) { return kv.second.dead; });
    std::erase_if(g.timers, [](const auto& kv) { return kv.second.dead; });
}

int PushName(l5::State* L, const void* ctx) {
    l5::A().pushstring(L, static_cast<const char*>(ctx));
    return 1;
}

bool ForLevel(const LevelSource& s, const std::string& key) {
    return !key.empty() && (key == s.key || key == s.key + ".S");
}

size_t LevelCount() {
    size_t n = 0;
    for (auto& s : g_levelSources) n += ForLevel(s, g_levelKey);
    return n;
}

int PushTick(l5::State* L, const void* ctx) {
    l5::A().pushnumber(L, static_cast<float>(*static_cast<const uint32_t*>(ctx)));
    return 1;
}

struct DispatchFloats {
    const char* event;
    const std::vector<float>* args;
};
int PushDispatch(l5::State* L, const void* ctx) {
    const auto& d = *static_cast<const DispatchFloats*>(ctx);
    l5::A().pushstring(L, d.event);
    const size_t n = std::min<size_t>(d.args->size(), 16);
    for (size_t i = 0; i < n; ++i) l5::A().pushnumber(L, (*d.args)[i]);
    return 1 + static_cast<int>(n);
}

struct DispatchArgList {
    const char* event;
    const simbridge::Arg* args;
    int n;
};
int PushDispatchArgs(l5::State* L, const void* ctx) {
    const auto& d = *static_cast<const DispatchArgList*>(ctx);
    l5::A().pushstring(L, d.event);
    const int n = std::min(d.n, 16);
    for (int i = 0; i < n; ++i) {
        if (d.args[i].kind == simbridge::Arg::Str)
            l5::A().pushstring(L, d.args[i].str ? d.args[i].str : "");
        else
            l5::A().pushnumber(L, d.args[i].num);
    }
    return 1 + n;
}

void NotifyBeforeLoad() {
    const auto obs = g_beforeLoad;
    for (auto& o : obs) {
        try {
            o.fn(o.user);
        } catch (...) {
            LOG_ERROR("[sim] an OnBeforeModsLoad observer threw");
        }
    }
}

void NotifyMatch(bool begin) {
    const auto obs = g_matchObservers;
    for (auto& o : obs) {
        try {
            o.fn(begin, o.user);
        } catch (...) {
            LOG_ERROR("[sim] an OnMatch observer threw");
        }
    }
}

int LoadedCount() {
    int n = 0;
    for (auto& m : g.mods) n += m.loaded;
    return n;
}

int HeapKB() { return g.L ? std::max(0, l5::A().getgccount(g.L) - g.baseHeapKB) : 0; }

bool Segment(const char*& p, bool first) {
    const char* s = p;
    if (first && !(*p >= 'A' && *p <= 'Z')) return false;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || (!first && *p == '_'))
        ++p;
    return p > s;
}

// ^[A-Z][A-Za-z0-9]*(\.[A-Za-z0-9_]+){1,5}$
bool ValidMessageName(const char* name) {
    const char* p = name;
    if (!Segment(p, true)) return false;
    int dots = 0;
    while (*p == '.') {
        ++p;
        if (!Segment(p, false)) return false;
        ++dots;
    }
    return !*p && dots >= 1 && dots <= 5 && std::strlen(name) < 128;
}
}  // namespace

void Configure(const Config& c) { g_cfg = c; }

void SetSources(std::vector<ModSource> mods) {
    g_sources = std::move(mods);
    if (g_hooksChanged) g_hooksChanged();
}

size_t SourceCount() { return g_sources.size() + LevelCount(); }

void SetLevelSources(std::vector<LevelSource> sources) {
    g_levelSources = std::move(sources);
    if (g_hooksChanged) g_hooksChanged();
}

void SetLevel(const std::string& key) {
    if (key == g_levelKey) return;
    g_levelKey = key;
    if (g_hooksChanged) g_hooksChanged();
}

std::string LevelDigest() {
    std::string s;
    for (auto& m : g.mods)
        if (m.level) s += (s.empty() ? "" : ",") + m.sha256;
    return s;
}

bool IsLevelMod(int mod) { return mod >= 0 && mod < static_cast<int>(g.mods.size()) && g.mods[mod].level; }

void TurnStarted() {
    if (g.L) g.turnPending = true;   // the first turn starts before Init loads the sims
}
void SetLogSink(LogSink fn) { g_sink = fn; }
void SetHooksChanged(void (*fn)()) { g_hooksChanged = fn; }
bool Active() { return g.active; }
bool HasTickHooks() { return !g_tickHooks.empty(); }
bool HasBeforeLoad() { return !g_beforeLoad.empty(); }
bool NeedsTickHooks() { return g.active || !g_tickHooks.empty(); }
l5::State* MatchL() { return g.L; }

void ContextCreated(l5::State* L) {
    if (g.L) LOG_WARN("[sim] match VM %p was never seen closing; its state is dropped", g.L);
    g = Match{};
    g.L = L;
    g.serial = ++g_serial;
    g.seed = l5::LogicSeed();
    g_depth = 0;
}

bool Init(const std::vector<std::string>& forwarded, bool gateOpen) {
    if (!g.L || g.initDone) return g.active;
    g.initDone = true;
    for (auto& n : forwarded) {
        const uint16_t id = l5::Lookup(n.c_str());
        if (id != 0xffff) g.forwarded[id] = n;
    }
    std::vector<const ModSource*> srcs;
    for (auto& s : g_sources) srcs.push_back(&s);
    std::vector<const LevelSource*> levels;
    for (auto& s : g_levelSources)
        if (ForLevel(s, g_levelKey)) levels.push_back(&s);
    for (auto* s : levels) srcs.push_back(&s->src);
    if (srcs.empty() && g_beforeLoad.empty()) return false;
    if (!gateOpen) {
        LOG_INFO("[sim] match %u: %zu sim mods suspended (gate closed)", g.serial, srcs.size());
        return false;
    }
    NotifyBeforeLoad();
    if (srcs.empty()) return false;
    const auto t0 = Clock::now();
    CaptureEngineRefs();
    g.baseHeapKB = l5::A().getgccount(g.L);
    for (size_t k = 0; k < srcs.size(); ++k) {
        const ModSource& s = *srcs[k];
        Mod m;
        m.id = s.id;
        m.version = s.version;
        m.chunkName = s.chunkName;
        m.rng = g.seed ^ Fnv1a(s.id.c_str());
        if (k >= g_sources.size()) {
            const LevelSource& l = *levels[k - g_sources.size()];
            m.level = true;
            m.levelKey = l.key;
            m.stem = l.stem;
            m.sha256 = l.sha256;
            m.knots = l.knots;
        }
        g.mods.push_back(std::move(m));
    }
    for (int i = 0; i < static_cast<int>(g.mods.size()); ++i) {
        Mod& m = g.mods[i];
        const int kb0 = l5::A().getgccount(g.L);
        std::string err;
        if (!BuildEnv(i)) {
            err = "cannot build the environment";
        } else if (LoadChunk(i, srcs[i]->code, &err)) {
            m.loaded = true;
        }
        if (!m.loaded) {
            ++g.faults;
            ++m.faults;
            const char* what = m.level ? "level script" : "entry.sim";
            LOG_ERROR("[sim] %s: %s failed, the mod is not loaded: %s", m.id.c_str(), what, err.c_str());
            ModLog(i, 3, (std::string(what) + " failed: " + err).c_str());
            DropModCallbacks(i);
            DropRef(m.envRef);
        }
        m.heapKB = std::max(0, l5::A().getgccount(g.L) - kb0);
    }
    Compact();
    g.active = LoadedCount() > 0;
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    std::string list;
    for (auto& m : g.mods) list += " " + m.id + (m.loaded ? "" : "(failed)") + ":" + std::to_string(m.heapKB) + "KB";
    LOG_INFO("[sim] match %u: %d/%zu sim mods loaded in %.2f ms (seed %08x, allowAll %d):%s", g.serial, LoadedCount(),
             g.mods.size(), ms, g.seed, l5::AllowAll(), list.c_str());
    if (g.active) NotifyMatch(true);
    return g.active;
}

void Message(uint16_t id) {
    if (!g.active || !g.L) return;
    const auto it = g.forwarded.find(id);
    if (it == g.forwarded.end() || l5::RunState() == 2) return;
    const std::string name = it->second;
    DeliverEvent(name.c_str(), &PushName, name.c_str());
    Compact();
}

void Update() {
    if (!g.L) return;
    const auto t0 = Clock::now();
    const uint32_t tick = ++g.tick;
    if (g.active) {
        std::vector<std::pair<uint32_t, uint32_t>> due;
        for (auto& [h, t] : g.timers)
            if (!t.dead && t.due <= tick) due.push_back({t.due, h});
        std::sort(due.begin(), due.end());
        for (auto& [d, h] : due) {
            auto it = g.timers.find(h);
            if (it == g.timers.end() || it->second.dead) continue;
            const int mod = it->second.mod, ref = it->second.fnRef;
            std::string err;
            const bool ok = Invoke(mod, ref, &PushTick, &tick, false, &err);
            it = g.timers.find(h);
            if (it == g.timers.end()) continue;
            Timer& t = it->second;
            if (!ok) RecordFault(mod, &t, "timer", err);
            if (t.dead) continue;
            if (t.period) {
                while (t.due <= tick) t.due += t.period;
            } else {
                Kill(t);
            }
        }
        DeliverEvent("tick", &PushTick, &tick);
        if (g.turnPending) {
            g.turnPending = false;
            const std::vector<float> turn{static_cast<float>(++g.turns)};
            const DispatchFloats d{"sim.turnStarted", &turn};
            DeliverEvent(d.event, &PushDispatch, &d);
        }
    }
    if (!g_tickHooks.empty()) {
        auto hooks = g_tickHooks;
        std::stable_sort(hooks.begin(), hooks.end(), [](const TickHook& a, const TickHook& b) { return a.order < b.order; });
        for (auto& h : hooks) {
            try {
                h.fn(tick, h.user);
            } catch (...) {
                LOG_ERROR("[sim] tick hook %d threw at tick %u", h.handle, tick);
            }
        }
    }
    Compact();
    if (g.active) {
        const int kb = HeapKB();
        if (kb > 256 && !g.heapWarned) {
            g.heapWarned = true;
            LOG_WARN("[sim] sim mods hold about %d KB in the match VM (budget 256 KB); the engine collects it every tick",
                     kb);
        }
    }
    g_usBridge = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
    if (g_cfg.logTicks)
        LOG_INFO("[sim] tick %u: %zu subs, %zu timers, %.1f us", tick, g.subs.size(), g.timers.size(), g_usBridge);
}

void ContextClosing(l5::State* L) {
    if (!g.L || L != g.L) return;
    if (g.active) NotifyMatch(false);
    for (auto& [h, s] : g.subs) DropRef(s.fnRef);
    for (auto& [h, t] : g.timers) DropRef(t.fnRef);
    for (auto& m : g.mods) DropRef(m.envRef);
    for (int& r : g.engineRef) DropRef(r);
    DropRef(g.consoleRef);
    if (g.initDone && !g.mods.empty())
        LOG_INFO("[sim] match %u closed: %u ticks, %u faults, %d refs left", g.serial, g.tick, g.faults, g.refs);
    const uint32_t serial = g.serial;
    g = Match{};
    g.serial = serial;
    g_depth = 0;
}

void NoteUpdateTime(double us) {
    g_usLast = us;
    g_ring[g_ringCount++ % 256] = us;
}

bool KnownEvent(const char* name) noexcept {
    if (!name || !*name) return false;
    if (std::strcmp(name, "tick") == 0 || std::strncmp(name, "sim.", 4) == 0) return true;
    for (auto& [id, n] : g.forwarded)
        if (n == name) return true;
    return IsModMessage(name);
}

bool IsModMessage(const char* name) noexcept {
    for (auto& [n, id] : g_modMessages)
        if (n == name) return true;
    return false;
}

uint32_t AddSub(int mod, const char* event, int fnRef) noexcept {
    if (LiveCallbacks(mod) >= kMaxCallbacksPerMod) return 0;
    const uint32_t h = g.nextHandle++;
    Sub& s = g.subs[h];
    s.mod = mod;
    s.fnRef = fnRef;
    s.event = event;
    return h;
}

bool RemoveSub(int mod, uint32_t handle) noexcept {
    const auto it = g.subs.find(handle);
    if (it == g.subs.end() || it->second.mod != mod || it->second.dead) return false;
    Kill(it->second);
    return true;
}

uint32_t AddTimer(int mod, uint32_t delay, uint32_t period, int fnRef) noexcept {
    if (LiveCallbacks(mod) >= kMaxCallbacksPerMod) return 0;
    const uint32_t h = g.nextHandle++;
    Timer& t = g.timers[h];
    t.mod = mod;
    t.fnRef = fnRef;
    t.due = g.tick + delay;
    t.period = period;
    return h;
}

bool CancelTimer(int mod, uint32_t handle) noexcept {
    const auto it = g.timers.find(handle);
    if (it == g.timers.end() || it->second.mod != mod || it->second.dead) return false;
    Kill(it->second);
    return true;
}

void DropModCallbacks(int mod) {
    for (auto& [h, s] : g.subs)
        if (s.mod == mod && !s.dead) Kill(s);
    for (auto& [h, t] : g.timers)
        if (t.mod == mod && !t.dead) Kill(t);
}

void DeliverEvent(const char* event, PushArgs push, const void* ctx) {
    std::vector<std::pair<int, uint32_t>> order;
    for (auto& [h, s] : g.subs)
        if (!s.dead && s.event == event) order.push_back({s.mod, h});
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [mod, h] : order) {
        auto it = g.subs.find(h);
        if (it == g.subs.end() || it->second.dead) continue;
        std::string err;
        if (Invoke(mod, it->second.fnRef, push, ctx, false, &err)) continue;
        it = g.subs.find(h);
        RecordFault(mod, it != g.subs.end() ? &it->second : nullptr, event, err);
    }
}

void RecordFault(int mod, Callback* cb, const char* where, const std::string& err) {
    ++g.faults;
    if (mod >= 0 && mod < static_cast<int>(g.mods.size())) ++g.mods[mod].faults;
    const char* id = mod >= 0 && mod < static_cast<int>(g.mods.size()) ? g.mods[mod].id.c_str() : "?";
    const uint32_t n = cb ? ++cb->faults : 0;
    LOG_WARN("[sim] %s: fault in %s at tick %u (%u/3): %s", id, where, g.tick, n, err.c_str());
    ModLog(mod, 3, (std::string("fault in ") + where + ": " + err).c_str());
    if (cb && n >= 3 && !cb->dead) {
        Kill(*cb);
        LOG_WARN("[sim] %s: callback for %s disabled after 3 faults (tick %u)", id, where, g.tick);
        ModLog(mod, 3, (std::string("callback disabled after 3 faults: ") + where).c_str());
    }
}

void ModLog(int mod, int level, const char* text) noexcept {
    const char* id = mod >= 0 && mod < static_cast<int>(g.mods.size()) ? g.mods[mod].id.c_str() : "console";
    if (g_sink)
        g_sink(id, level, g.tick, text);
    else
        LOG_INFO("[mod] %s (tick %u): %s", id, g.tick, text);
}

bool RegisterModMessage(const char* name, uint16_t* idOut, const std::vector<std::string>& vanillaPrefixes) {
    if (g_frozen) {
        LOG_WARN("[sim] mod message '%s' refused: registration is frozen", name ? name : "");
        return false;
    }
    if (!name || !ValidMessageName(name)) {
        LOG_WARN("[sim] mod message '%s' refused: invalid name", name ? name : "");
        return false;
    }
    if (g_modMessages.size() >= kMaxModMessages) {
        LOG_WARN("[sim] mod message '%s' refused: the budget of %zu names is spent", name, kMaxModMessages);
        return false;
    }
    const std::string first(name, std::strchr(name, '.'));
    if (std::find(vanillaPrefixes.begin(), vanillaPrefixes.end(), first) != vanillaPrefixes.end()) {
        LOG_WARN("[sim] mod message '%s' refused: '%s' is a vanilla prefix", name, first.c_str());
        return false;
    }
    if (l5::Lookup(name) != 0xffff) {
        LOG_WARN("[sim] mod message '%s' refused: already registered", name);
        return false;
    }
    const size_t n = std::strlen(name) + 1;
    char* keep = new char[n];  // the engine's registry keeps this pointer for the life of the process
    std::memcpy(keep, name, n);
    l5::Register(keep);
    const uint16_t id = l5::Lookup(keep);
    if (id == 0xffff) {
        LOG_ERROR("[sim] mod message '%s': registration failed (registry %u/%u)", name, l5::RegistryCount(),
                  l5::RegistryCapacity());
        return false;
    }
    g_modMessages.push_back({keep, id});
    if (idOut) *idOut = id;
    LOG_INFO("[sim] mod message '%s' registered as %04x (%zu/%zu)", name, id, g_modMessages.size(), kMaxModMessages);
    return true;
}

void FreezeModMessages() {
    if (!g_frozen) LOG_INFO("[sim] mod message names frozen (%zu registered)", g_modMessages.size());
    g_frozen = true;
}

bool ModMessagesFrozen() { return g_frozen; }

Counters GetCounters() {
    return {static_cast<uint32_t>(std::max(0, g.refs)), static_cast<uint32_t>(g.subs.size()),
            static_cast<uint32_t>(g.timers.size()), static_cast<uint32_t>(g_tickHooks.size()),
            g.consoleRef >= 0 ? 1u : 0u, g.depthDrops, g_usBridge};
}
}  // namespace melange::simcore

namespace melange::sim {
using namespace simcore;
bool InMatch() { return g.L != nullptr || lua50::MatchState() != nullptr; }
uint32_t MatchSerial() { return g.serial; }
uint32_t Tick() { return g.tick; }
bool ModsActive() { return g.active; }

int AddTickHook(TickFn fn, void* user, int order) {
    if (!fn) return 0;
    const int h = g_nextTickHook++;
    g_tickHooks.push_back({h, order, fn, user});
    if (g_hooksChanged) g_hooksChanged();
    return h;
}

void RemoveTickHook(int handle) {
    std::erase_if(g_tickHooks, [handle](const TickHook& t) { return t.handle == handle; });
}

Stats GetStats() {
    Stats s{};
    s.ticks = g.tick;
    s.simMods = static_cast<uint32_t>(LoadedCount());
    s.faults = g.faults;
    s.usLastTick = g_usLast;
    const uint32_t n = std::min<uint32_t>(g_ringCount, 256);
    if (n) {
        std::vector<double> v(g_ring, g_ring + n);
        const size_t k = std::min<size_t>(n - 1, static_cast<size_t>(n * 0.95));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        s.usP95Tick = v[k];
    }
    s.heapKB = static_cast<uint32_t>(HeapKB());
    return s;
}
}  // namespace melange::sim

namespace melange::simbridge {
using namespace simcore;
int OnMatch(MatchFn fn, void* user) {
    if (!fn) return 0;
    g_matchObservers.push_back({g_nextObserver, fn, user});
    return g_nextObserver++;
}

void RemoveOnMatch(int handle) {
    std::erase_if(g_matchObservers, [handle](const MatchObserver& o) { return o.handle == handle; });
}

int OnBeforeModsLoad(InitFn fn, void* user) {
    if (!fn) return 0;
    g_beforeLoad.push_back({g_nextObserver, fn, user});
    if (g_hooksChanged) g_hooksChanged();
    return g_nextObserver++;
}

void RemoveOnBeforeModsLoad(int handle) {
    std::erase_if(g_beforeLoad, [handle](const BeforeLoad& o) { return o.handle == handle; });
    if (g_hooksChanged) g_hooksChanged();
}

void DispatchArgs(const char* event, const Arg* args, int n) {
    if (!g.active || !event || std::strncmp(event, "sim.", 4) != 0 || n < 0 || (n && !args)) return;
    const DispatchArgList d{event, args, n};
    DeliverEvent(event, &PushDispatchArgs, &d);
    Compact();
}

void Dispatch(const char* event, const std::vector<float>& args) {
    if (!g.active || !event || std::strncmp(event, "sim.", 4) != 0) return;
    const DispatchFloats d{event, &args};
    DeliverEvent(event, &PushDispatch, &d);
    Compact();
}

const char* ModIdAt(int modIndex) {
    return modIndex >= 0 && modIndex < static_cast<int>(g.mods.size()) ? g.mods[modIndex].id.c_str() : nullptr;
}

const char* CurrentMod() { return ModIdAt(CurrentModIndex()); }

bool InTopLevelChunk() { return g_depth > 0 && g_frames[g_depth - 1].topLevel; }

std::vector<std::string> LoadedMods() {
    std::vector<std::string> v;
    for (auto& m : g.mods)
        if (m.loaded) v.push_back(m.id);
    return v;
}

bool PushModEnv(const char* id) {
    if (!g.L || !id) return false;
    for (auto& m : g.mods) {
        if (!m.loaded || m.envRef < 0 || m.id != id) continue;
        lua50::A().rawgeti(g.L, lua50::kRegistry, m.envRef);
        return true;
    }
    return false;
}

std::vector<std::pair<std::string, uint16_t>> ModMessages() { return g_modMessages; }
}  // namespace melange::simbridge
