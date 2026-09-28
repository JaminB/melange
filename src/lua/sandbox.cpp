// Sandbox: the Lua 5.4 client VM, per-mod environments, callbacks and dispatch.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#include "core/game.h"
#include "core/log.h"
#include "lua/sandbox_core.h"
#include "lua/sandbox_internal.h"
#include "melange/jlog.h"
#include "melange/lua.h"
#include "melange/mods.h"

static_assert(LUA_VERSION_NUM == 504);

namespace melange::sandbox {
namespace {
constexpr size_t kMaxQueue = 4096;
constexpr size_t kEventsPerFrame = 256;
constexpr size_t kMaxChunkBytes = 4u << 20;
constexpr size_t kMaxHandlesPerGen = 4096;
constexpr int kMaxFaults = 3;

struct Timer {
    uint32_t cb;
    double due, interval;
    uint64_t seq;
};
struct Pending {
    std::string name;
    json::Value payload;
};
struct Lib {
    std::string name;
    lua::OpenFn fn;
    uint32_t flags;
    int ref = LUA_NOREF;
    bool failed = false;
};

lua_State* g_L = nullptr;
std::atomic<bool> g_running{false};
Limits g_lim;
std::map<std::string, std::unique_ptr<ModRec>> g_mods;
int g_nextSlot = 1;
std::map<uint32_t, std::unique_ptr<Callback>> g_cbs;
std::vector<std::unique_ptr<Gen>> g_gens;
std::vector<std::unique_ptr<Callback>> g_graveCbs;
std::vector<std::unique_ptr<Gen>> g_graveGens;
uint32_t g_nextCb = 1, g_genSerial = 0;
std::map<std::string, std::vector<uint32_t>> g_listeners;
std::vector<Timer> g_timers;
uint64_t g_timerSeq = 0;
std::deque<Pending> g_queue;
std::mutex g_postMx;
std::vector<std::pair<std::string, std::string>> g_posted;
uint64_t g_dropped = 0, g_droppedLogged = 0, g_frame = 0;
int g_frozenRef = LUA_NOREF, g_gRef = LUA_NOREF, g_wumRef = LUA_NOREF, g_consoleRef = LUA_NOREF;
size_t g_consoleLibs = 0;
std::string* g_capture = nullptr;
uint32_t g_faults = 0;
double g_msAccum = 0, g_msLast = 0;
GameState g_game;

std::vector<Lib>& Libs() {
    static std::vector<Lib> v;
    return v;
}
std::vector<std::pair<SharedInit, EnvInit>>& Registrars() {
    static std::vector<std::pair<SharedInit, EnvInit>> v;
    return v;
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool IsIdent(const std::string& s) {
    if (s.empty() || !(isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    for (char c : s)
        if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}

bool IsEngineName(const std::string& n) { return n.rfind("melange.", 0) != 0 && n.rfind("mod.", 0) != 0; }

// ---------------------------------------------------------------- read-only tables
int FrozenNewIndex(lua_State* L) { return luaL_error(L, "attempt to modify a read-only table"); }
int FrozenNext(lua_State* L) {
    lua_settop(L, 2);
    lua_pushvalue(L, 2);
    if (lua_next(L, lua_upvalueindex(1))) return 2;
    lua_pushnil(L);
    return 1;
}
// Returns an iterator bound to the hidden table, never the table itself.
int FrozenPairs(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushcclosure(L, &FrozenNext, 1);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}
int FrozenLen(lua_State* L) {
    lua_pushinteger(L, static_cast<lua_Integer>(lua_rawlen(L, lua_upvalueindex(1))));
    return 1;
}

bool IsFrozen(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    idx = lua_absindex(L, idx);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_frozenRef);
    lua_pushvalue(L, idx);
    const bool f = lua_rawget(L, -2) != LUA_TNIL;
    lua_pop(L, 2);
    return f;
}

// ---------------------------------------------------------------- base library replacements
int SafeRawset(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkany(L, 2);
    luaL_checkany(L, 3);
    if (IsFrozen(L, 1)) return luaL_error(L, "attempt to modify a read-only table");
    lua_settop(L, 3);
    lua_rawset(L, 1);
    return 1;
}

int SafeSetmetatable(lua_State* L) {
    const int t = lua_type(L, 2);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
    if (t == LUA_TTABLE && lua_getfield(L, 2, "__gc") != LUA_TNIL) return luaL_error(L, "__gc metamethods are not allowed");
    if (t == LUA_TTABLE) lua_pop(L, 1);
    if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL) return luaL_error(L, "cannot change a protected metatable");
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
}

int SafeCollect(lua_State* L) {
    const std::string opt = luaL_optstring(L, 1, "collect");
    if (opt == "collect") {
        lua_gc(L, LUA_GCCOLLECT);
        lua_pushinteger(L, 0);
        return 1;
    }
    if (opt == "count") {
        const int kb = lua_gc(L, LUA_GCCOUNT), b = lua_gc(L, LUA_GCCOUNTB);
        lua_pushnumber(L, static_cast<lua_Number>(kb) + static_cast<lua_Number>(b) / 1024.0);
        return 1;
    }
    if (opt == "step") {
        lua_pushboolean(L, lua_gc(L, LUA_GCSTEP, static_cast<int>(luaL_optinteger(L, 2, 0))));
        return 1;
    }
    return luaL_error(L, "collectgarbage: only \"collect\", \"count\" and \"step\" are allowed");
}

std::string JoinArgs(lua_State* L) {
    std::string s;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* p = luaL_tolstring(L, i, &len);
        if (i > 1) s += '\t';
        s.append(p, len);
        lua_pop(L, 1);
    }
    return s;
}

int Print(lua_State* L) {
    ModLog(Current(), 1, JoinArgs(L));
    return 0;
}

int Traceback(lua_State* L) {
    // No __tostring: a message handler can run inside the count hook, where hooks are off and mod code has no budget.
    const char* msg = lua_type(L, 1) == LUA_TSTRING || lua_type(L, 1) == LUA_TNUMBER ? lua_tostring(L, 1) : nullptr;
    if (!msg) msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
    luaL_traceback(L, L, msg, 1);
    return 1;
}

int ProtectedTramp(lua_State* L) {
    auto* fn = static_cast<const std::function<void(lua_State*)>*>(lua_touserdata(L, 1));
    lua_remove(L, 1);
    (*fn)(L);
    return 0;
}

int ConsoleModsIndex(lua_State* L) {
    ModRec* m = FindMod(luaL_checkstring(L, 2));
    if (!m || !m->gen) return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, m->gen->envRef);
    return 1;
}

void CopyFields(lua_State* L, int from, int to) {
    from = lua_absindex(L, from);
    to = lua_absindex(L, to);
    lua_pushnil(L);
    while (lua_next(L, from)) {
        lua_pushvalue(L, -2);
        lua_insert(L, -2);
        lua_rawset(L, to);
    }
}

// ---------------------------------------------------------------- VM set-up
void OpenLib(lua_State* L, Lib& lib) {
    if (lib.ref != LUA_NOREF || lib.failed) return;
    lua_pushcfunction(L, lib.fn);
    if (lua_pcall(L, 0, 1, 0) != LUA_OK || lua_type(L, -1) != LUA_TTABLE) {
        SysLog(3, "library failed to open", {}, "wum." + lib.name + ": " + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "did not return a table"));
        lib.failed = true;
        lua_pop(L, 1);
        return;
    }
    PushFrozen(L, -1);
    lib.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);
}

// Pushes a new environment. `m` is nullptr for the console.
void BuildEnv(lua_State* L, ModRec* m, Gen* g) {
    lua_newtable(L);
    const int e = lua_gettop(L);
    lua_createtable(L, 0, 2);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_gRef);
    lua_setfield(L, -2, "__index");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, e);
    lua_pushvalue(L, e);
    lua_setfield(L, e, "_G");

    lua_newtable(L);
    const int w = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_wumRef);
    CopyFields(L, -1, w);
    lua_pop(L, 1);
    for (auto& r : Registrars())
        if (r.second) r.second(L, w, m);
    const bool deep = !m || m->granted;
    for (Lib& lib : Libs()) {
        if ((lib.flags & lua::kLibDeepDesert) && !deep) continue;
        OpenLib(L, lib);
        if (lib.ref == LUA_NOREF) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, lib.ref);
        lua_setfield(L, w, lib.name.c_str());
    }
    if (g && lua_getfield(L, w, "mod") == LUA_TTABLE) g->modTableRef = luaL_ref(L, LUA_REGISTRYINDEX);
    else if (g) lua_pop(L, 1);
    PushFrozen(L, w);
    lua_setfield(L, e, "wum");
    lua_pop(L, 1);

    if (!m) {
        lua_newtable(L);
        lua_createtable(L, 0, 3);
        lua_pushcfunction(L, &ConsoleModsIndex);
        lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, &FrozenNewIndex);
        lua_setfield(L, -2, "__newindex");
        lua_pushliteral(L, "locked");
        lua_setfield(L, -2, "__metatable");
        lua_setmetatable(L, -2);
        lua_setfield(L, e, "mods");
    }
}

void BuildGlobals(lua_State* L) {
    lua_newtable(L);
    lua_createtable(L, 0, 1);
    lua_pushliteral(L, "k");
    lua_setfield(L, -2, "__mode");
    lua_setmetatable(L, -2);
    g_frozenRef = luaL_ref(L, LUA_REGISTRYINDEX);

    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    lua_newtable(L);
    const int g = lua_gettop(L);
    static const char* const kBase[] = {"assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
                                        "rawlen", "select", "tonumber", "tostring", "type", "getmetatable", "_VERSION"};
    for (const char* k : kBase) {
        lua_getglobal(L, k);
        lua_setfield(L, g, k);
    }
    // An error raised by the count hook leaves hooks off on the raising thread until a pcall restores them. Lua code
    // that runs in that window could never be stopped: xpcall message handlers (called before unwinding) and the
    // __close handlers of a coroutine that died that way. These replacements never run mod code in that window.
    static const char kBoot[] =
        "local pcall, error, pack, unpack = pcall, error, table.pack, table.unpack\n"
        "local create, resume, status, close = coroutine.create, coroutine.resume, coroutine.status, coroutine.close\n"
        "local function xpcall(f, h, ...)\n"
        "  local r = pack(pcall(f, ...))\n"
        "  if r[1] then return unpack(r, 1, r.n) end\n"
        "  local _, v = pcall(h, r[2])\n"
        "  return false, v\n"
        "end\n"
        "local function wrap(f)\n"
        "  local co = create(f)\n"
        "  return function(...)\n"
        "    local r = pack(resume(co, ...))\n"
        "    if r[1] then return unpack(r, 2, r.n) end\n"
        "    error(r[2], 2)\n"
        "  end\n"
        "end\n"
        "local function safeclose(co)\n"
        "  if status(co) == 'dead' then return true end\n"
        "  return close(co)\n"
        "end\n"
        "return xpcall, wrap, safeclose\n";
    luaL_requiref(L, "table", luaopen_table, 1);
    luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
    lua_pop(L, 2);
    if (luaL_loadbufferx(L, kBoot, sizeof(kBoot) - 1, "=sandbox", "t") != LUA_OK) lua_error(L);
    lua_call(L, 0, 3);
    const int boot = lua_gettop(L) - 2;
    lua_pushvalue(L, boot);
    lua_setfield(L, g, "xpcall");
    static const luaL_Reg kRepl[] = {{"rawset", &SafeRawset}, {"setmetatable", &SafeSetmetatable},
                                     {"collectgarbage", &SafeCollect}, {"print", &Print}, {nullptr, nullptr}};
    RegisterFunctions(L, g, kRepl);

    struct LibOpen {
        const char* name;
        lua_CFunction open;
    };
    static const LibOpen kLibs[] = {{"string", luaopen_string}, {"table", luaopen_table}, {"math", luaopen_math},
                                    {"utf8", luaopen_utf8}, {"coroutine", luaopen_coroutine}};
    for (const LibOpen& lo : kLibs) {
        luaL_requiref(L, lo.name, lo.open, 0);
        lua_newtable(L);
        CopyFields(L, -2, -1);
        if (strcmp(lo.name, "string") == 0) {
            lua_pushnil(L);
            lua_setfield(L, -2, "dump");
            // String methods index the copy without dump; the metatable itself is locked.
            lua_pushliteral(L, "");
            lua_getmetatable(L, -1);
            lua_pushvalue(L, -3);
            lua_setfield(L, -2, "__index");
            lua_pushliteral(L, "locked");
            lua_setfield(L, -2, "__metatable");
            lua_pop(L, 2);
        }
        if (strcmp(lo.name, "coroutine") == 0) {
            lua_pushvalue(L, boot + 1);
            lua_setfield(L, -2, "wrap");
            lua_pushvalue(L, boot + 2);
            lua_setfield(L, -2, "close");
        }
        PushFrozen(L, -1);
        lua_setfield(L, g, lo.name);
        lua_pop(L, 2);
    }
    lua_pop(L, 3);
    luaL_requiref(L, "os", luaopen_os, 0);
    lua_createtable(L, 0, 3);
    for (const char* k : {"time", "clock", "date"}) {
        lua_getfield(L, -2, k);
        lua_setfield(L, -2, k);
    }
    PushFrozen(L, -1);
    lua_setfield(L, g, "os");
    lua_pop(L, 2);

    PushFrozen(L, g);
    g_gRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_pop(L, 1);

    lua_newtable(L);
    const int w = lua_gettop(L);
    for (auto& r : Registrars())
        if (r.first) r.first(L, w);
    // Namespaces are shared by every environment: freeze each of them.
    std::vector<std::string> keys;
    lua_pushnil(L);
    while (lua_next(L, w)) {
        if (lua_type(L, -1) == LUA_TTABLE && lua_type(L, -2) == LUA_TSTRING) keys.push_back(lua_tostring(L, -2));
        lua_pop(L, 1);
    }
    for (const std::string& k : keys) {
        lua_getfield(L, w, k.c_str());
        PushFrozen(L, -1);
        lua_setfield(L, w, k.c_str());
        lua_pop(L, 1);
    }
    g_wumRef = luaL_ref(L, LUA_REGISTRYINDEX);
}

// ---------------------------------------------------------------- callbacks and generations
void Fault(Callback* cb, const std::string& err) {
    ++g_faults;
    ModRec* m = cb->gen->mod;
    ++m->faults;
    ++cb->faults;
    SysLog(3, "callback error", m->id, std::string(KindName(cb->kind)) + " " + cb->label + ": " + err);
    if (cb->faults >= kMaxFaults && !cb->disabled) {
        cb->disabled = true;
        SysLog(2, "callback disabled after 3 faults", m->id, std::string(KindName(cb->kind)) + " " + cb->label);
    }
}

void RevokeGen(Gen* g) {
    if (!g) return;
    const std::vector<uint32_t> ids = g->callbacks;
    for (uint32_t id : ids) KillCallback(id);
    for (auto& fn : g->cleanups) fn();
    g->cleanups.clear();
    if (g_L) {
        luaL_unref(g_L, LUA_REGISTRYINDEX, g->envRef);
        luaL_unref(g_L, LUA_REGISTRYINDEX, g->modTableRef);
    }
    g->envRef = g->modTableRef = LUA_NOREF;
    g->committed = false;
    auto it = std::find_if(g_gens.begin(), g_gens.end(), [&](const auto& p) { return p.get() == g; });
    if (it != g_gens.end()) {
        g_graveGens.push_back(std::move(*it));
        g_gens.erase(it);
    }
}

void Bury() {
    if (TopCtx()) return;
    g_graveCbs.clear();
    g_graveGens.clear();
}
}  // namespace

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool ReadWhole(const std::wstring& path, size_t cap, std::string* out, std::string* err) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        *err = "cannot open " + game::Narrow(path);
        return false;
    }
    out->clear();
    char buf[16384];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (out->size() + n > cap) {
            fclose(f);
            *err = "file too large: " + game::Narrow(path);
            return false;
        }
        out->append(buf, n);
    }
    fclose(f);
    return true;
}

bool SafeRelPath(const std::string& rel) {
    if (rel.empty() || rel.find(':') != std::string::npos || rel[0] == '/' || rel[0] == '\\') return false;
    size_t p = 0;
    while (p <= rel.size()) {
        size_t q = rel.find_first_of("/\\", p);
        if (q == std::string::npos) q = rel.size();
        const std::string seg = rel.substr(p, q - p);
        if (seg == ".." || (seg.empty() && q != rel.size())) return false;
        p = q + 1;
    }
    return true;
}

namespace {
ModRec* Ensure(const std::string& id) {
    auto& p = g_mods[id];
    if (!p) {
        p = std::make_unique<ModRec>();
        p->id = id;
        p->slot = g_nextSlot < MaxSlots() ? g_nextSlot++ : 0;
    }
    return p.get();
}

bool Load(const char* id, bool reload) {
    if (!g_L || !id || !*id) return false;
    mods::ModInfo info{};
    if (!mods::Find(id, &info)) {
        SysLog(2, "unknown mod", id);
        return false;
    }
    ModRec* m = Ensure(id);
    m->name = info.name ? info.name : id;
    m->version = info.version ? info.version : "";
    m->dir = info.dir ? info.dir : L"";
    m->unsafe = info.unsafe;
    m->granted = info.unsafe && info.unsafeGranted;
    m->order = info.order;
    std::string err, src;
    if (!ReadManifest(m, &err)) {
    } else if (m->entryClient.empty()) {
        err = "no entry.client";
    } else if (!SafeRelPath(m->entryClient)) {
        err = "entry.client must be a relative path inside the mod folder";
    } else {
        std::wstring rel = Widen(m->entryClient);
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        ReadWhole(m->dir + L"\\" + rel, kMaxChunkBytes, &src, &err);
    }
    if (!err.empty()) {
        m->error = err;
        SysLog(3, reload ? "reload failed" : "load failed", m->id, err);
        return false;
    }

    g_gens.push_back(std::make_unique<Gen>());
    Gen* g = g_gens.back().get();
    g->mod = m;
    g->serial = ++g_genSerial;
    bool ok = false;
    const double t0 = NowMs();
    Protected("load", [&](lua_State* L) {
        BuildEnv(L, m, g);
        g->envRef = luaL_ref(L, LUA_REGISTRYINDEX);
        lua_pushcfunction(L, &Traceback);
        const int h = lua_gettop(L);
        const std::string chunk = "@" + m->id + "/" + m->entryClient;
        if (luaL_loadbufferx(L, src.data(), src.size(), chunk.c_str(), "t") != LUA_OK) {
            err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
            lua_settop(L, h - 1);
            return;
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, g->envRef);
        lua_setupvalue(L, -2, 1);
        PushCtx(m, g, nullptr);
        const int rc = lua_pcall(L, 0, 0, h);
        const Ctx c = PopCtx();
        if (rc != LUA_OK) err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        else if (c.exhausted) err = "instruction budget exceeded";
        else if (c.oom) err = "out of memory (ModMemoryMB)";
        else ok = true;
        lua_settop(L, h - 1);
    });
    m->msAccum += NowMs() - t0;
    if (!ok) {
        if (err.empty()) err = "internal error";
        RevokeGen(g);
        m->error = err;
        SysLog(3, reload ? "reload failed, the previous version keeps running" : "load failed", m->id, err);
        return false;
    }

    Gen* old = m->gen;
    int keepRef = LUA_NOREF;
    if (old && old->modTableRef != LUA_NOREF)
        Protected("keep", [&](lua_State* L) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, old->modTableRef);
            lua_pushliteral(L, "keep");
            lua_rawget(L, -2);
            keepRef = luaL_ref(L, LUA_REGISTRYINDEX);
            lua_pop(L, 1);
        });
    RevokeGen(old);
    m->gen = g;
    g->committed = true;
    for (uint32_t cid : std::vector<uint32_t>(g->callbacks))
        if (Callback* cb = FindCallback(cid); cb && !cb->attached) Activate(cb);
    m->error.clear();
    SysLog(1, old ? "reloaded" : "loaded", m->id, m->entryClient);

    if (old && g->modTableRef != LUA_NOREF) {
        Protected("onReload", [&](lua_State* L) {
            lua_pushcfunction(L, &Traceback);
            const int h = lua_gettop(L);
            lua_rawgeti(L, LUA_REGISTRYINDEX, g->modTableRef);
            lua_pushliteral(L, "onReload");
            if (lua_rawget(L, -2) == LUA_TFUNCTION) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, keepRef);
                PushCtx(m, g, nullptr);
                const int rc = lua_pcall(L, 1, 0, h);
                const Ctx c = PopCtx();
                if (rc != LUA_OK || c.exhausted || c.oom)
                    SysLog(3, "wum.mod.onReload failed", m->id, rc != LUA_OK && lua_tostring(L, -1) ? lua_tostring(L, -1) : "budget");
            }
            lua_settop(L, h - 1);
        });
    }
    if (g_L) luaL_unref(g_L, LUA_REGISTRYINDEX, keepRef);
    if (old) {
        json::Value p;
        p.type = json::Type::Object;
        json::Value v;
        v.type = json::Type::String;
        v.string = m->id;
        p.members.emplace_back("id", v);
        QueueEvent("melange.reload", std::move(p));
    }
    Bury();
    return true;
}

// Scans back over an identifier chain ("wum.draw.li", "t:me").
size_t ChainStart(const std::string& s) {
    size_t i = s.size();
    while (i > 0) {
        const char c = s[i - 1];
        if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == ':') --i;
        else break;
    }
    return i;
}

// t[k] without metamethod calls: raw, then through __index tables (proxies, environments). Pushes the value.
void RawLookup(lua_State* L, int t, const std::string& k) {
    t = lua_absindex(L, t);
    lua_pushvalue(L, t);
    for (int depth = 0; depth < 8; ++depth) {
        if (lua_type(L, -1) != LUA_TTABLE) break;
        lua_pushstring(L, k.c_str());
        if (lua_rawget(L, -2) != LUA_TNIL) {
            lua_remove(L, -2);
            return;
        }
        lua_pop(L, 1);
        if (!lua_getmetatable(L, -1)) break;
        lua_pushliteral(L, "__index");
        lua_rawget(L, -2);
        lua_remove(L, -2);
        lua_remove(L, -2);
    }
    lua_pop(L, 1);
    lua_pushnil(L);
}

void RawKeys(lua_State* L, int t, const std::string& prefix, bool fnOnly, std::set<std::string>* out) {
    lua_pushvalue(L, t);
    for (int depth = 0; depth < 8 && lua_type(L, -1) == LUA_TTABLE; ++depth) {
        lua_pushnil(L);
        while (lua_next(L, -2)) {
            if (lua_type(L, -2) == LUA_TSTRING && (!fnOnly || lua_type(L, -1) == LUA_TFUNCTION)) {
                std::string k = lua_tostring(L, -2);
                if (k.rfind(prefix, 0) == 0 && IsIdent(k)) out->insert(k);
            }
            lua_pop(L, 1);
        }
        if (!lua_getmetatable(L, -1)) break;
        lua_pushliteral(L, "__index");
        lua_rawget(L, -2);
        lua_remove(L, -2);
        lua_remove(L, -2);
    }
    lua_pop(L, 1);
}

int PushEnvFor(lua_State* L, ModRec* m) {
    if (m) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, m->gen->envRef);
        return lua_gettop(L);
    }
    if (g_consoleRef == LUA_NOREF || g_consoleLibs != Libs().size()) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_consoleRef);
        BuildEnv(L, nullptr, nullptr);
        g_consoleRef = luaL_ref(L, LUA_REGISTRYINDEX);
        g_consoleLibs = Libs().size();
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_consoleRef);
    return lua_gettop(L);
}

void AppendDescribed(lua_State* L, int idx, int depth, std::string* out) { *out += DescribeValue(L, idx, depth); }
}  // namespace

// ---------------------------------------------------------------- internal API
const char* KindName(CbKind k) {
    switch (k) {
        case CbKind::Event: return "event";
        case CbKind::Timer: return "timer";
        case CbKind::Panel: return "panel";
        case CbKind::Menu: return "menu";
        case CbKind::Hotkey: return "hotkey";
        case CbKind::Draw: return "draw";
        case CbKind::Reload: return "reload";
    }
    return "?";
}

LibRegistrar::LibRegistrar(SharedInit shared, EnvInit perEnv) { Registrars().emplace_back(shared, perEnv); }

void RegisterFunctions(lua_State* L, int tableIdx, const luaL_Reg* fns) {
    tableIdx = lua_absindex(L, tableIdx);
    for (; fns->name; ++fns) {
        lua_pushcfunction(L, fns->func);
        lua_setfield(L, tableIdx, fns->name);
    }
}

void PushFrozen(lua_State* L, int realIdx) {
    realIdx = lua_absindex(L, realIdx);
    lua_newtable(L);
    lua_createtable(L, 0, 5);
    lua_pushvalue(L, realIdx);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &FrozenNewIndex);
    lua_setfield(L, -2, "__newindex");
    lua_pushvalue(L, realIdx);
    lua_pushcclosure(L, &FrozenPairs, 1);
    lua_setfield(L, -2, "__pairs");
    lua_pushvalue(L, realIdx);
    lua_pushcclosure(L, &FrozenLen, 1);
    lua_setfield(L, -2, "__len");
    lua_pushliteral(L, "locked");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_frozenRef);
    lua_pushvalue(L, -2);
    lua_pushboolean(L, 1);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

int ErrorF(lua_State* L, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    luaL_where(L, 1);
    lua_pushstring(L, buf);
    lua_concat(L, 2);
    return lua_error(L);
}

std::string DescribeValue(lua_State* L, int idx, int depth) {
    idx = lua_absindex(L, idx);
    char buf[64];
    switch (lua_type(L, idx)) {
        case LUA_TNIL: return "nil";
        case LUA_TBOOLEAN: return lua_toboolean(L, idx) ? "true" : "false";
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(lua_tointeger(L, idx)));
            else snprintf(buf, sizeof(buf), "%.14g", static_cast<double>(lua_tonumber(L, idx)));
            return buf;
        case LUA_TSTRING: {
            size_t n;
            const char* s = lua_tolstring(L, idx, &n);
            return depth > 0 ? "\"" + std::string(s, n) + "\"" : std::string(s, n);
        }
        case LUA_TTABLE: {
            if (depth > 0) {
                snprintf(buf, sizeof(buf), "table: %p", lua_topointer(L, idx));
                return buf;
            }
            std::string s = "{";
            int count = 0;
            lua_pushnil(L);
            while (lua_next(L, idx)) {
                if (count == 32) {
                    s += ", ...";
                    lua_pop(L, 2);
                    break;
                }
                if (count++) s += ", ";
                if (lua_type(L, -2) == LUA_TSTRING) s += lua_tostring(L, -2);
                else s += "[" + DescribeValue(L, -2, 1) + "]";
                s += " = " + DescribeValue(L, -1, 1);
                lua_pop(L, 1);
            }
            return s + "}";
        }
        default:
            snprintf(buf, sizeof(buf), "%s: %p", luaL_typename(L, idx), lua_topointer(L, idx));
            return buf;
    }
}

lua_State* L() { return g_L; }
const Limits& GetLimits() { return g_lim; }
bool Running() { return g_running.load(); }
GameState& Game() { return g_game; }
std::string* Capture() { return g_capture; }

ModRec* Current() {
    const Ctx* c = TopCtx();
    return c ? c->mod : nullptr;
}

Gen* CurrentGen() {
    const Ctx* c = TopCtx();
    return c ? c->gen : nullptr;
}

ModRec* FindMod(const std::string& id) {
    auto it = g_mods.find(id);
    return it == g_mods.end() ? nullptr : it->second.get();
}

std::vector<ModRec*> LoadedMods() {
    std::vector<ModRec*> v;
    for (auto& [id, m] : g_mods)
        if (m->gen) v.push_back(m.get());
    std::stable_sort(v.begin(), v.end(), [](ModRec* a, ModRec* b) { return a->order < b->order; });
    return v;
}

Callback* FindCallback(uint32_t id) {
    auto it = g_cbs.find(id);
    return it == g_cbs.end() ? nullptr : it->second.get();
}

Callback* NewCallback(lua_State* L, int fnIdx, CbKind kind, std::string label) {
    Ctx* c = TopCtx();
    if (!c || !c->mod || !c->gen) {
        luaL_error(L, "only mod code can register callbacks");
        return nullptr;
    }
    luaL_checktype(L, fnIdx, LUA_TFUNCTION);
    if (c->gen->callbacks.size() >= kMaxHandlesPerGen) {
        luaL_error(L, "too many handles (%d)", static_cast<int>(kMaxHandlesPerGen));
        return nullptr;
    }
    lua_pushvalue(L, fnIdx);
    const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    auto cb = std::make_unique<Callback>();
    cb->id = g_nextCb++;
    cb->gen = c->gen;
    cb->ref = ref;
    cb->kind = kind;
    cb->label = std::move(label);
    Callback* raw = cb.get();
    c->gen->callbacks.push_back(raw->id);
    g_cbs.emplace(raw->id, std::move(cb));
    return raw;
}

bool Activate(Callback* cb) {
    if (!cb || cb->dead) return false;
    if (!cb->gen->committed) return true;
    const bool ok = cb->attach ? cb->attach() : true;
    if (!ok) {
        SysLog(2, "registration failed", cb->gen->mod->id, std::string(KindName(cb->kind)) + " " + cb->label);
        KillCallback(cb->id);
        return false;
    }
    cb->attached = true;
    return true;
}

void KillCallback(uint32_t id) {
    auto it = g_cbs.find(id);
    if (it == g_cbs.end()) return;
    Callback* cb = it->second.get();
    cb->dead = true;
    if (cb->attached && cb->revoke) cb->revoke();
    cb->attached = false;
    if (cb->kind == CbKind::Event) RemoveListener(id);
    if (g_L) luaL_unref(g_L, LUA_REGISTRYINDEX, cb->ref);
    cb->ref = LUA_NOREF;
    g_graveCbs.push_back(std::move(it->second));
    g_cbs.erase(it);
}

bool CallbackLive(const Callback* cb) {
    return cb && !cb->dead && !cb->disabled && cb->gen->committed && cb->gen->mod->gen == cb->gen;
}

bool Invoke(Callback* cb, const std::function<int(lua_State*)>& pushArgs, int nresults,
            const std::function<void(lua_State*)>& onResults) {
    if (!g_L || !CallbackLive(cb)) return false;
    ModRec* m = cb->gen->mod;
    bool ok = false;
    std::string err;
    const double t0 = NowMs();
    const bool ran = Protected("callback", [&](lua_State* L) {
        lua_pushcfunction(L, &Traceback);
        const int h = lua_gettop(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, cb->ref);
        const int n = pushArgs ? pushArgs(L) : 0;
        PushCtx(m, cb->gen, cb);
        const int rc = lua_pcall(L, n, nresults, h);
        const Ctx c = PopCtx();
        if (rc != LUA_OK) err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        else if (c.exhausted) err = "instruction budget exceeded";
        else if (c.oom) err = "out of memory (ModMemoryMB)";
        else {
            ok = true;
            if (onResults) onResults(L);
        }
        lua_settop(L, h - 1);
    });
    const double dt = NowMs() - t0;
    g_msAccum += dt;
    m->msAccum += dt;
    if (!ok && !cb->dead) Fault(cb, ran ? err : "internal error");
    return ok;
}

bool Protected(const char* what, const std::function<void(lua_State*)>& fn) {
    if (!g_L) return false;
    lua_State* L = g_L;
    const int top = lua_gettop(L);
    lua_pushcfunction(L, &ProtectedTramp);
    lua_pushlightuserdata(L, const_cast<std::function<void(lua_State*)>*>(&fn));
    const int rc = lua_pcall(L, 1, 0, 0);
    if (rc != LUA_OK) {
        const char* msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "non-string error";
        SysLog(3, "host call failed", Current() ? Current()->id : "", std::string(what) + ": " + msg);
    }
    lua_settop(L, top);
    return rc == LUA_OK;
}

bool ReadManifest(ModRec* m, std::string* err) {
    m->entryClient.clear();
    m->filesystem = "none";
    m->settings.clear();
    const std::wstring path = m->dir + L"\\spice.json";
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    json::Value doc;
    json::Error je;
    if (!json::ParseFile(path, &doc, &je)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "spice.json:%d:%d: ", je.line, je.col);
        *err = buf + je.text;
        return false;
    }
    if (const json::Value* e = doc.Get("entry"); e && e->IsObject())
        if (const json::Value* c = e->Get("client"); c && c->IsString()) m->entryClient = c->string;
    if (const json::Value* p = doc.Get("permissions"); p && p->IsObject()) {
        if (const json::Value* f = p->Get("filesystem"); f && f->IsString()) m->filesystem = f->string;
        if (const json::Value* u = p->Get("unsafe"); u && u->IsBool()) m->unsafe = m->unsafe || u->boolean;
    }
    if (const json::Value* s = doc.Get("settings"); s && s->IsArray()) {
        for (const json::Value& it : s->items) {
            if (!it.IsObject()) continue;
            const json::Value* k = it.Get("key");
            const json::Value* t = it.Get("type");
            if (!k || !k->IsString() || !t || !t->IsString()) continue;
            SettingDecl d;
            d.key = k->string;
            d.type = t->string;
            if (const json::Value* o = it.Get("options"); o && o->IsArray())
                for (const json::Value& x : o->items)
                    if (x.IsString()) d.options.push_back(x.string);
            if (const json::Value* v = it.Get("min"); v && v->IsNumber()) d.min = v->number, d.hasMin = true;
            if (const json::Value* v = it.Get("max"); v && v->IsNumber()) d.max = v->number, d.hasMax = true;
            const json::Value* dv = it.Get("default");
            if (dv && dv->IsString()) d.def = dv->string;
            else if (dv && dv->IsBool()) d.def = dv->boolean ? "true" : "false";
            else if (dv && dv->IsNumber()) d.def = WriteJson(*dv);
            else if (d.type == "bool") d.def = "false";
            else if (d.type == "int" || d.type == "float") d.def = "0";
            else if (d.type == "enum" && !d.options.empty()) d.def = d.options[0];
            m->settings.push_back(std::move(d));
        }
    }
    return true;
}

// ---------------------------------------------------------------- dispatch
void QueueEvent(std::string name, json::Value payload) {
    if (g_queue.size() >= kMaxQueue) {
        ++g_dropped;
        return;
    }
    g_queue.push_back({std::move(name), std::move(payload)});
}

void AddListener(const std::string& name, uint32_t cbId) {
    auto& v = g_listeners[name];
    const bool first = v.empty();
    v.push_back(cbId);
    if (first && IsEngineName(name)) EngineSubscribe(name, true);
}

void RemoveListener(uint32_t cbId) {
    for (auto it = g_listeners.begin(); it != g_listeners.end();) {
        auto& v = it->second;
        const size_t before = v.size();
        v.erase(std::remove(v.begin(), v.end(), cbId), v.end());
        if (v.empty() && before) {
            if (IsEngineName(it->first)) EngineSubscribe(it->first, false);
            it = g_listeners.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t AddTimer(uint32_t cbId, double delay, double interval) {
    g_timers.push_back({cbId, NowSeconds() + std::max(0.0, delay), interval, ++g_timerSeq});
    return cbId;
}

void FireDirect(const std::string& name, const json::Value& payload) {
    auto it = g_listeners.find(name);
    if (it == g_listeners.end()) return;
    std::vector<Callback*> cbs;
    for (uint32_t id : it->second)
        if (Callback* cb = FindCallback(id); CallbackLive(cb)) cbs.push_back(cb);
    std::stable_sort(cbs.begin(), cbs.end(), [](Callback* a, Callback* b) {
        return a->gen->mod->order != b->gen->mod->order ? a->gen->mod->order < b->gen->mod->order : a->id < b->id;
    });
    std::vector<uint32_t> ids;
    for (Callback* c : cbs) ids.push_back(c->id);
    for (uint32_t id : ids) {
        Invoke(FindCallback(id), [&](lua_State* L) {
            PushJson(L, payload);
            lua_pushstring(L, name.c_str());
            return 2;
        });
    }
}

bool Start(const Limits& lim) {
    if (g_L) return true;
    g_lim = lim;
    ResetLimits();
    g_L = lua_newstate(&Alloc, nullptr);
    if (!g_L) return false;
    lua_atpanic(g_L, [](lua_State* L) -> int {
        LOG_ERROR("[sandbox] Lua panic: %s", lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        return 0;
    });
    InstallHook(g_L);
    if (!Protected("start", [](lua_State* L) { BuildGlobals(L); })) {
        lua_close(g_L);
        g_L = nullptr;
        return false;
    }
    g_running = true;
    SysLog(1, "started");
    return true;
}

void Stop() {
    if (!g_L) return;
    g_running = false;
    for (ModRec* m : LoadedMods()) UnloadMod(m->id.c_str());
    for (auto& [id, m] : g_mods) StorageFlush(m.get(), true);
    for (auto& [n, v] : g_listeners)
        if (IsEngineName(n)) EngineSubscribe(n, false);
    g_listeners.clear();
    g_timers.clear();
    g_queue.clear();
    for (Lib& lib : Libs()) lib.ref = LUA_NOREF, lib.failed = false;
    g_consoleRef = LUA_NOREF;
    g_consoleLibs = 0;
    lua_close(g_L);
    g_L = nullptr;
    g_graveCbs.clear();
    g_graveGens.clear();
    g_cbs.clear();
    g_gens.clear();
    ResetLimits();
}

void Frame() {
    if (!g_L) return;
    ++g_frame;
    g_msLast = g_msAccum;
    g_msAccum = 0;
    for (auto& [id, m] : g_mods) {
        m->msFrame = m->msAccum;
        m->msAccum = 0;
    }
    std::vector<std::pair<std::string, std::string>> posted;
    {
        std::lock_guard lk(g_postMx);
        posted.swap(g_posted);
    }
    for (auto& [name, text] : posted) {
        json::Value v;
        json::Error e;
        if (!json::Parse(text, &v, &e) || !v.IsObject()) {
            SysLog(2, "PostEvent: payload is not a JSON object", {}, name);
            continue;
        }
        QueueEvent(std::move(name), std::move(v));
    }

    const double now = NowSeconds();
    std::vector<Timer> due;
    for (auto it = g_timers.begin(); it != g_timers.end();) {
        Callback* cb = FindCallback(it->cb);
        if (!cb) {
            it = g_timers.erase(it);
            continue;
        }
        if (it->due <= now) due.push_back(*it);
        ++it;
    }
    std::sort(due.begin(), due.end(), [](const Timer& a, const Timer& b) { return a.due != b.due ? a.due < b.due : a.seq < b.seq; });
    for (const Timer& t : due) {
        Callback* cb = FindCallback(t.cb);
        if (!cb) continue;
        if (t.interval < 0) {
            Invoke(cb, {});
            KillCallback(t.cb);
            continue;
        }
        for (Timer& live : g_timers)
            if (live.cb == t.cb) {
                live.due += t.interval;
                if (live.due < now) live.due = now + t.interval;
            }
        Invoke(cb, {});
    }

    for (size_t n = 0; n < kEventsPerFrame && !g_queue.empty(); ++n) {
        Pending p = std::move(g_queue.front());
        g_queue.pop_front();
        FireDirect(p.name, p.payload);
    }
    if (g_dropped != g_droppedLogged) {
        SysLog(2, "event queue overflow", {}, std::to_string(g_dropped - g_droppedLogged) + " events dropped");
        g_droppedLogged = g_dropped;
    }

    if (g_listeners.count("melange.frame")) {
        json::Value p;
        p.type = json::Type::Object;
        json::Value f;
        f.type = json::Type::Number;
        f.number = static_cast<double>(g_frame);
        p.members.emplace_back("frame", f);
        FireDirect("melange.frame", p);
    }
    for (auto& [id, m] : g_mods) StorageFlush(m.get(), false);
    Bury();
}

std::vector<std::string> ApiNames() {
    std::vector<std::string> out;
    ModRec probe;
    probe.id = "api-probe";
    probe.unsafe = probe.granted = true;
    Protected("api", [&](lua_State* L) {
        BuildEnv(L, &probe, nullptr);
        RawLookup(L, -1, "wum");
        std::set<std::string> ns;
        RawKeys(L, -1, "", false, &ns);
        for (const std::string& n : ns) {
            RawLookup(L, -1, n);
            if (lua_type(L, -1) == LUA_TTABLE) {
                std::set<std::string> fns;
                RawKeys(L, -1, "", false, &fns);
                for (const std::string& f : fns) out.push_back("wum." + n + "." + f);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    });
    return out;
}

void FlushAllStorage() {
    for (auto& [id, m] : g_mods) StorageFlush(m.get(), true);
}

void ModLog(ModRec* m, int level, const std::string& text) {
    if (std::string* cap = Capture()) {
        if (!cap->empty()) *cap += '\n';
        static const char* const kPre[] = {"[debug] ", "", "[warn] ", "[error] "};
        *cap += kPre[std::clamp(level, 0, 3)] + text;
    }
    const std::string id = m ? m->id : "console";
    if (m) {
        const uint64_t sec = static_cast<uint64_t>(NowSeconds());
        if (sec != m->logWindow) {
            if (m->logDropped) SysLog(2, "wum.log rate limit", m->id, std::to_string(m->logDropped) + " records dropped");
            m->logWindow = sec;
            m->logCount = 0;
            m->logDropped = 0;
        }
        if (++m->logCount > 200) {
            ++m->logDropped;
            return;
        }
    }
    static const jlog::Level kLv[] = {jlog::Level::Debug, jlog::Level::Info, jlog::Level::Warn, jlog::Level::Error};
    jlog::Rec("mod", kLv[std::clamp(level, 0, 3)], text).Str("mod", id).Emit();
}

void SysLog(int level, const std::string& msg, const std::string& modId, const std::string& detail) {
    static const jlog::Level kLv[] = {jlog::Level::Debug, jlog::Level::Info, jlog::Level::Warn, jlog::Level::Error};
    jlog::Rec r("sandbox", kLv[std::clamp(level, 0, 3)], msg);
    if (!modId.empty()) r.Str("mod", modId);
    if (!detail.empty()) r.Str("detail", detail);
    r.Emit();
    if (level >= 2)
        log::Write(level >= 3 ? "ERROR" : "WARN ", "[sandbox] %s%s%s%s%s", msg.c_str(), modId.empty() ? "" : " (",
                   modId.c_str(), modId.empty() ? "" : ")", detail.empty() ? "" : (": " + detail).c_str());
}

// ---------------------------------------------------------------- the contract with Thumper and the console
bool LoadMod(const char* id) { return Load(id, FindMod(id ? id : "") && FindMod(id)->gen); }

void UnloadMod(const char* id) {
    ModRec* m = FindMod(id ? id : "");
    if (!m || !m->gen) return;
    RevokeGen(m->gen);
    m->gen = nullptr;
    StorageFlush(m, true);
    SysLog(1, "unloaded", m->id);
    Bury();
}

bool ReloadMod(const char* id) { return Load(id, true); }

EvalOut Eval(const char* modIdOrNull, const std::string& code) {
    if (!g_L) return {false, "the Sandbox is not running"};
    ModRec* m = nullptr;
    if (modIdOrNull) {
        m = FindMod(modIdOrNull);
        if (!m || !m->gen) return {false, std::string("mod not loaded: ") + modIdOrNull};
    }
    std::string src = code;
    if (!src.empty() && src[0] == '=') src = "return " + src.substr(1);
    std::string out, result;
    bool ok = false;
    std::string* prevCap = g_capture;
    g_capture = &out;
    Protected("eval", [&](lua_State* L) {
        const int env = PushEnvFor(L, m);
        lua_pushcfunction(L, &Traceback);
        const int h = lua_gettop(L);
        const std::string ret = "return " + src;
        if (luaL_loadbufferx(L, ret.data(), ret.size(), "=console", "t") != LUA_OK) {
            lua_pop(L, 1);
            if (luaL_loadbufferx(L, src.data(), src.size(), "=console", "t") != LUA_OK) {
                result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
                lua_settop(L, env - 1);
                return;
            }
        }
        lua_pushvalue(L, env);
        lua_setupvalue(L, -2, 1);
        PushCtx(m, m ? m->gen : nullptr, nullptr);
        const int rc = lua_pcall(L, 0, LUA_MULTRET, h);
        const Ctx c = PopCtx();
        if (rc != LUA_OK) {
            result = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        } else if (c.exhausted || c.oom) {
            result = c.exhausted ? "instruction budget exceeded" : "out of memory";
        } else {
            ok = true;
            for (int i = h + 1; i <= lua_gettop(L); ++i) {
                if (i > h + 1) result += '\t';
                AppendDescribed(L, i, 0, &result);
            }
        }
        lua_settop(L, env - 1);
    });
    g_capture = prevCap;
    if (!result.empty()) {
        if (!out.empty()) out += '\n';
        out += result;
    }
    return {ok, out};
}

void Complete(const char* modIdOrNull, const std::string& prefix, std::vector<std::string>* out) {
    if (!out) return;
    out->clear();
    if (!g_L) return;
    ModRec* m = modIdOrNull ? FindMod(modIdOrNull) : nullptr;
    if (modIdOrNull && (!m || !m->gen)) return;
    const size_t start = ChainStart(prefix);
    const std::string head = prefix.substr(0, start), chain = prefix.substr(start);
    const size_t cut = chain.find_last_of(".:");
    const std::string base = cut == std::string::npos ? "" : chain.substr(0, cut + 1);
    const std::string last = cut == std::string::npos ? chain : chain.substr(cut + 1);
    const bool method = cut != std::string::npos && chain[cut] == ':';
    std::set<std::string> keys;
    Protected("complete", [&](lua_State* L) {
        PushEnvFor(L, m);
        size_t p = 0;
        while (cut != std::string::npos && p < cut) {
            size_t q = chain.find_first_of(".:", p);
            if (q == std::string::npos || q > cut) q = cut;
            RawLookup(L, -1, chain.substr(p, q - p));
            lua_remove(L, -2);
            if (lua_type(L, -1) != LUA_TTABLE) return;
            p = q + 1;
        }
        RawKeys(L, -1, last, method, &keys);
    });
    for (const std::string& k : keys) out->push_back(head + base + k);
}

bool Status(const char* id, ModStatus* out) {
    ModRec* m = FindMod(id ? id : "");
    if (!m || !out) return false;
    *out = {};
    out->loaded = m->gen != nullptr;
    out->error = m->error;
    out->faults = m->faults;
    out->bytes = SlotBytes(m->slot);
    out->instructions = m->instructions;
    out->msLastFrame = m->msFrame;
    if (m->gen)
        for (uint32_t cid : m->gen->callbacks)
            if (Callback* cb = FindCallback(cid)) {
                ++out->callbacks;
                if (cb->disabled) ++out->disabledCallbacks;
            }
    return true;
}
}  // namespace melange::sandbox

// ---------------------------------------------------------------- melange/lua.h
namespace melange::lua {
using namespace melange::sandbox;

bool Ready() { return sandbox::Running(); }
lua_State* ClientState() { return sandbox::L(); }

bool AddLibrary(const char* name, OpenFn fn, uint32_t flags) {
    static const char* const kReserved[] = {"mod", "log", "events", "timers", "config", "storage", "game",
                                            "ui", "draw", "render", "postfx", "unsafe", "sim"};
    if (!name || !fn || !IsIdent(name)) return false;
    for (const char* r : kReserved)
        if (strcmp(r, name) == 0) return false;
    for (const Lib& l : Libs())
        if (l.name == name) return false;
    Libs().push_back({name, fn, flags});
    return true;
}

const char* CurrentMod() {
    ModRec* m = Current();
    return m ? m->id.c_str() : nullptr;
}

bool PostEvent(const char* name, const char* jsonObject) {
    if (!name || !*name || !sandbox::Running()) return false;
    std::lock_guard lk(g_postMx);
    if (g_posted.size() >= kMaxQueue) return false;
    g_posted.emplace_back(name, jsonObject && *jsonObject ? jsonObject : "{}");
    return true;
}

Stats GetStats() {
    Stats s{};
    for (auto& [id, m] : g_mods)
        if (m->gen) ++s.mods;
    for (auto& [id, cb] : g_cbs) {
        ++s.callbacks;
        if (cb->disabled) ++s.disabledCallbacks;
    }
    s.faults = g_faults;
    s.instructions = TotalInstructions();
    s.bytes = TotalBytes();
    s.msLastFrame = g_msLast;
    return s;
}
}  // namespace melange::lua
