// Mod environments in the engine's Lua 5.0.1 match VM, the instruction budget, and the match console.
// Nothing here writes a global of the match VM: environments and callbacks live in registry references.
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "lua/sim/bridge_internal.h"
#include "lua/sim/sim_internal.h"

namespace melange::simcore {
Frame g_frames[kMaxDepth];
int g_depth = 0;

namespace {
constexpr char kBudgetMsg[] = "melange: instruction budget exceeded";
// xpcall is deliberately not in this list: the engine's own xpcall runs its handler as the pcall's message
// handler, which fires *before* the stack unwinds and hooks are re-armed. A handler that loops there can never
// be stopped. XpcallSim below runs the handler through its own, later pcall instead, by which point the budget
// hook is back on.
constexpr const char* kBase[] = {"assert", "error", "ipairs", "next", "pairs", "pcall", "rawequal", "rawget",
                                 "rawset", "setmetatable", "getmetatable", "tonumber", "tostring", "type", "unpack"};
l5::Hook g_prevHook = nullptr;
int g_prevMask = 0;
// Every nested level used to get its own fresh Chunks() budget, so a mod that resends its own message from a
// handler on that message could multiply its work by its fan-out at every one of the 8 depth levels (about
// fanout^depth handler calls from one top-level send). g_chainLeft is a whole-chain budget, shared by every
// frame of one top-level dispatch, that bounds the total work regardless of fan-out.
int g_chainLeft = 0;

int Chunks() { return std::max(1, (g_cfg.instrPerCall + 999) / 1000); }

void __cdecl BudgetHook(l5::State* L, void*) {
    if (g_depth <= 0) return;
    Frame& f = g_frames[g_depth - 1];
    if (f.suspended) return;
    --g_chainLeft;
    if (--f.left > 0 && !f.exhausted && g_chainLeft > 0) return;
    if (!f.exhausted) {
        f.exhausted = true;
        l5::A().sethook(L, &BudgetHook, l5::kMaskCount, 1);
    }
    l5::A().pushstring(L, kBudgetMsg);
    l5::A().error(L);
}

void ArmTop(l5::State* L) {
    const Frame& f = g_frames[g_depth - 1];
    if (f.suspended)
        l5::A().sethook(L, nullptr, 0, 0);
    else
        l5::A().sethook(L, &BudgetHook, l5::kMaskCount, f.exhausted ? 1 : 1000);
}

bool PushFrame(int mod, bool topLevel) {
    if (g_depth >= kMaxDepth) return false;
    l5::State* L = g.L;
    if (g_depth == 0) {
        g_prevHook = l5::A().gethook(L);
        g_prevMask = l5::A().gethookmask(L);
        g_chainLeft = Chunks() * kMaxDepth;
    }
    if (g_chainLeft <= 0) return false;
    g_frames[g_depth++] = {mod, std::min(Chunks(), g_chainLeft), false, false, topLevel};
    ArmTop(L);
    return true;
}

bool PopFrame() {
    l5::State* L = g.L;
    const bool exhausted = g_frames[--g_depth].exhausted;
    if (g_depth > 0)
        ArmTop(L);
    else
        l5::A().sethook(L, g_prevMask ? g_prevHook : nullptr, g_prevMask, 0);
    return exhausted;
}

void PushLib(l5::State* L, const l5::LibReg* reg, const char* skip) {
    const auto& a = l5::A();
    a.newtable(L);
    for (; reg && reg->name; ++reg) {
        if (skip && std::strcmp(reg->name, skip) == 0) continue;
        a.pushstring(L, reg->name);
        a.pushcclosure(L, reg->fn, 0);
        a.rawset(L, -3);
    }
}

int __cdecl RandomSeedRaises(l5::State* L) {
    l5::A().pushstring(L, "math.randomseed is not available in sim mods (wum.sim.random is seeded by the match)");
    l5::A().error(L);
    return 0;
}

// xpcall(f, h, ...): f runs under an ordinary pcall; on error, h runs under a *separate* later pcall, by which
// point the budget hook has been re-armed, so a handler that loops is still stopped.
int __cdecl XpcallSim(l5::State* L) {
    const auto& a = l5::A();
    const int n = a.gettop(L);
    if (n < 2) {
        a.pushstring(L, "xpcall: expected (f, h, ...)");
        a.error(L);
    }
    a.pushvalue(L, 2);
    int href = TakeRef();
    a.remove(L, 2);
    constexpr int kMultret = -1;  // LUA_MULTRET, stable across Lua versions
    const int rc = a.pcall(L, n - 2, kMultret, 0);
    if (rc == 0) {
        DropRef(href);
        a.pushboolean(L, 1);
        a.insert(L, 1);
        return a.gettop(L);
    }
    a.rawgeti(L, l5::kRegistry, href);
    DropRef(href);
    a.insert(L, -2);
    a.pcall(L, 1, 1, 0);  // its own success/failure both just become the second return value, as sandbox.cpp does
    a.pushboolean(L, 0);
    a.insert(L, -2);
    return 2;
}

// Protected: arg 1 = lightuserdata(int* mod). Returns the new environment.
int __cdecl BuildEnvK(l5::State* L) {
    const auto& a = l5::A();
    const int mod = *static_cast<int*>(a.touserdata(L, 1));
    a.newtable(L);  // 2: base
    for (const char* n : kBase) {
        a.pushstring(L, n);
        a.pushstring(L, n);
        a.rawget(L, l5::kGlobals);
        a.rawset(L, 2);
    }
    a.pushstring(L, "xpcall");
    a.pushcclosure(L, &XpcallSim, 0);
    a.rawset(L, 2);
    PushWum(mod);   // 3: wum
    a.newtable(L);  // 4: math, a copy of the engine's with random/randomseed replaced
    a.pushstring(L, "math");
    a.rawget(L, l5::kGlobals);  // 5
    if (a.type(L, 5) == l5::kTTable) {
        a.pushnil(L);
        while (a.next(L, 5)) {
            a.pushvalue(L, -2);
            a.insert(L, -2);
            a.rawset(L, 4);
        }
    }
    a.settop(L, 4);
    a.pushstring(L, "random");
    a.pushstring(L, "sim");
    a.rawget(L, 3);
    a.pushstring(L, "random");
    a.rawget(L, 6);
    a.remove(L, 6);
    a.rawset(L, 4);
    a.pushstring(L, "randomseed");
    a.pushcclosure(L, &RandomSeedRaises, 0);
    a.rawset(L, 4);
    a.pushstring(L, "math");
    a.pushvalue(L, 4);
    a.rawset(L, 2);
    a.settop(L, 3);
    a.pushstring(L, "wum");
    a.pushvalue(L, 3);
    a.rawset(L, 2);
    a.pushstring(L, "string");
    PushLib(L, l5::StringLib(), "dump");
    a.rawset(L, 2);
    a.pushstring(L, "table");
    PushLib(L, l5::TableLib(), nullptr);
    a.rawset(L, 2);
    a.pushstring(L, "print");
    a.pushstring(L, "log");
    a.rawget(L, 3);
    a.pushstring(L, "info");
    a.rawget(L, 5);
    a.remove(L, 5);
    a.rawset(L, 2);
    a.newtable(L);  // 4: env
    a.newtable(L);  // 5: its metatable
    a.pushstring(L, "__index");
    a.pushvalue(L, 2);
    a.rawset(L, 5);
    a.setmetatable(L, 4);
    a.pushstring(L, "_G");
    a.pushvalue(L, 4);
    a.rawset(L, 2);
    return 1;
}

char g_capture[16384];
size_t g_captureLen = 0;
void Capture(const char* s) {
    const size_t n = std::strlen(s);
    const size_t room = sizeof g_capture - 1 - g_captureLen;
    const size_t k = std::min(n, room);
    std::memcpy(g_capture + g_captureLen, s, k);
    g_captureLen += k;
    g_capture[g_captureLen] = 0;
}

int __cdecl ConsolePrint(l5::State* L) {
    const auto& a = l5::A();
    const int n = a.gettop(L);
    for (int i = 1; i <= n; ++i) {
        const int t = a.type(L, i);
        const char* s = t == l5::kTString || t == l5::kTNumber ? a.tostring(L, i)
                        : t == l5::kTBoolean                   ? (a.toboolean(L, i) ? "true" : "false")
                        : t == l5::kTNil                       ? "nil"
                                                               : "<value>";
        if (i > 1) Capture("\t");
        Capture(s);
    }
    Capture("\n");
    return 0;
}

// Protected: the console environment (__index = the match globals; print/echo captured; mods[id] = env).
int __cdecl BuildConsoleK(l5::State* L) {
    const auto& a = l5::A();
    a.newtable(L);  // 1: env
    a.newtable(L);
    a.pushstring(L, "__index");
    a.pushvalue(L, l5::kGlobals);
    a.rawset(L, 2);
    a.setmetatable(L, 1);
    a.pushstring(L, "print");
    a.pushcclosure(L, &ConsolePrint, 0);
    a.rawset(L, 1);
    a.pushstring(L, "echo");
    a.pushcclosure(L, &ConsolePrint, 0);
    a.rawset(L, 1);
    a.pushstring(L, "mods");
    a.newtable(L);
    for (const Mod& m : g.mods) {
        if (!m.loaded || m.envRef < 0) continue;
        a.pushstring(L, m.id.c_str());
        a.rawgeti(L, l5::kRegistry, m.envRef);
        a.rawset(L, -3);
    }
    a.rawset(L, 1);
    return 1;
}

std::string ErrorText(l5::State* L, int rc, bool exhausted) {
    if (exhausted) return "instruction budget exceeded (" + std::to_string(g_cfg.instrPerCall) + " instructions)";
    if (rc == -1) return "dispatch nested too deep";
    const auto& a = l5::A();
    if (a.gettop(L) > 0 && a.type(L, -1) == l5::kTString) return a.tostring(L, -1);
    return "error object is not a string";
}
}  // namespace

int CurrentModIndex() { return g_depth > 0 ? g_frames[g_depth - 1].mod : -1; }

int TakeRef() {
    const int r = l5::A().ref(g.L, l5::kRegistry);
    if (r >= 0) ++g.refs;
    return r;
}

void DropRef(int& ref) {
    if (ref >= 0 && g.L) {
        l5::A().unref(g.L, l5::kRegistry, ref);
        --g.refs;
    }
    ref = -1;
}

void SuspendBudget() {
    if (g_depth <= 0) return;
    g_frames[g_depth - 1].suspended = true;
    ArmTop(g.L);
}

void ResumeBudget() {
    if (g_depth <= 0) return;
    g_frames[g_depth - 1].suspended = false;
    ArmTop(g.L);
}

int PcallBudgeted(int mod, int nargs, int nres, bool topLevel, bool* exhausted) {
    const auto& a = l5::A();
    *exhausted = false;
    if (!PushFrame(mod, topLevel)) {
        a.settop(g.L, -(nargs + 1) - 1);
        ++g.depthDrops;
        return -1;
    }
    const int rc = a.pcall(g.L, nargs, nres, 0);
    *exhausted = PopFrame();
    return rc;
}

bool Invoke(int mod, int fnRef, PushArgs push, const void* ctx, bool topLevel, std::string* err) {
    l5::State* L = g.L;
    const auto& a = l5::A();
    const int top = a.gettop(L);
    if (!a.checkstack(L, 24)) {
        if (err) *err = "stack overflow";
        return false;
    }
    a.rawgeti(L, l5::kRegistry, fnRef);
    const int nargs = push ? push(L, ctx) : 0;
    bool exhausted = false;
    const int rc = PcallBudgeted(mod, nargs, 0, topLevel, &exhausted);
    const bool ok = rc == 0 && !exhausted;
    if (!ok && err) *err = ErrorText(L, rc, exhausted);
    a.settop(L, top);
    return ok;
}

bool BuildEnv(int mod) {
    l5::State* L = g.L;
    const auto& a = l5::A();
    const int top = a.gettop(L);
    int m = mod;
    a.pushcclosure(L, &BuildEnvK, 0);
    a.pushlightuserdata(L, &m);
    const int rc = a.pcall(L, 1, 1, 0);
    if (rc) {
        LOG_ERROR("[sim] %s: building the environment failed: %s", g.mods[mod].id.c_str(),
                  ErrorText(L, rc, false).c_str());
        a.settop(L, top);
        return false;
    }
    g.mods[mod].envRef = TakeRef();
    a.settop(L, top);
    return g.mods[mod].envRef >= 0;
}

bool LoadChunk(int mod, const std::string& code, std::string* err) {
    l5::State* L = g.L;
    const auto& a = l5::A();
    const int top = a.gettop(L);
    Mod& m = g.mods[mod];
    if (!code.empty() && code[0] == '\x1b') {
        *err = "precompiled chunks are not accepted";
        return false;
    }
    if (a.loadbuffer(L, code.data(), code.size(), m.chunkName.c_str())) {
        *err = ErrorText(L, 1, false);
        a.settop(L, top);
        return false;
    }
    a.rawgeti(L, l5::kRegistry, m.envRef);
    if (!a.setfenv(L, -2)) {
        *err = "setfenv failed";
        a.settop(L, top);
        return false;
    }
    bool exhausted = false;
    const int rc = PcallBudgeted(mod, 0, 0, true, &exhausted);
    const bool ok = rc == 0 && !exhausted;
    if (!ok) *err = ErrorText(L, rc, exhausted);
    a.settop(L, top);
    return ok;
}

bool BuildConsoleEnv() {
    l5::State* L = g.L;
    const auto& a = l5::A();
    const int top = a.gettop(L);
    a.pushcclosure(L, &BuildConsoleK, 0);
    const int rc = a.pcall(L, 0, 1, 0);
    if (rc) {
        a.settop(L, top);
        return false;
    }
    g.consoleRef = TakeRef();
    a.settop(L, top);
    return g.consoleRef >= 0;
}
}  // namespace melange::simcore

namespace melange::simbridge {
using namespace simcore;

sandbox::EvalOut EvalMatch(const std::string& code) {
    if (!l5::Check()) return {false, "the engine's Lua check failed"};
    if (!g.L) return {false, "no match VM"};
    if (g_depth > 0) return {false, "the match VM is busy"};
    l5::State* L = g.L;
    const auto& a = l5::A();
    if (g.consoleRef < 0 && !BuildConsoleEnv()) return {false, "cannot build the console environment"};
    std::string src = code;
    if (!src.empty() && src[0] == '=') src = "return " + src.substr(1);
    const int top = a.gettop(L);
    std::string expr = "return " + src;
    if (a.loadbuffer(L, expr.data(), expr.size(), "=console")) {
        a.settop(L, top);
        if (a.loadbuffer(L, src.data(), src.size(), "=console")) {
            std::string e = ErrorText(L, 1, false);
            a.settop(L, top);
            return {false, e};
        }
    }
    a.rawgeti(L, l5::kRegistry, g.consoleRef);
    a.setfenv(L, -2);
    g_captureLen = 0;
    g_capture[0] = 0;
    bool exhausted = false;
    const int rc = PcallBudgeted(-1, 0, -1, false, &exhausted);
    std::string out = g_capture;
    if (rc || exhausted) {
        out += ErrorText(L, rc, exhausted);
        a.settop(L, top);
        return {false, out};
    }
    for (int i = top + 1; i <= a.gettop(L); ++i) {
        const int t = a.type(L, i);
        if (i > top + 1) out += "\t";
        if (t == l5::kTString || t == l5::kTNumber)
            out += a.tostring(L, i);
        else if (t == l5::kTBoolean)
            out += a.toboolean(L, i) ? "true" : "false";
        else if (t == l5::kTNil)
            out += "nil";
        else
            out += t == l5::kTTable ? "table" : t == l5::kTFunction ? "function" : "userdata";
    }
    a.settop(L, top);
    return {true, out};
}

void CompleteMatch(const std::string& prefix, std::vector<std::string>* out) {
    if (!out || !g.L) return;
    l5::State* L = g.L;
    const auto& a = l5::A();
    if (g.consoleRef < 0 && !BuildConsoleEnv()) return;
    const size_t cut = prefix.find_last_of(".:");
    const std::string head = cut == std::string::npos ? "" : prefix.substr(0, cut + 1);
    const std::string part = cut == std::string::npos ? prefix : prefix.substr(cut + 1);
    std::vector<std::string> path;
    for (size_t i = 0; cut != std::string::npos && i < cut;) {
        size_t j = prefix.find_first_of(".:", i);
        if (j == std::string::npos || j > cut) j = cut;
        path.push_back(prefix.substr(i, j - i));
        i = j + 1;
    }
    const int top = a.gettop(L);
    std::vector<std::string> found;
    auto collect = [&](int t) {
        a.pushnil(L);
        while (a.next(L, t)) {
            if (a.type(L, -2) == l5::kTString) {
                std::string k(a.tostring(L, -2), a.strlen(L, -2));
                if (k.compare(0, part.size(), part) == 0) found.push_back(head + k);
            }
            a.settop(L, -2);
        }
    };
    if (path.empty()) {
        a.rawgeti(L, l5::kRegistry, g.consoleRef);
        collect(a.gettop(L));
        collect(l5::kGlobals);
    } else {
        a.rawgeti(L, l5::kRegistry, g.consoleRef);
        a.pushstring(L, path[0].c_str());
        a.rawget(L, -2);
        if (a.type(L, -1) == l5::kTNil) {
            a.settop(L, -2);
            a.pushstring(L, path[0].c_str());
            a.rawget(L, l5::kGlobals);
        }
        bool ok = a.type(L, -1) == l5::kTTable;
        for (size_t i = 1; ok && i < path.size(); ++i) {
            a.pushstring(L, path[i].c_str());
            a.rawget(L, -2);
            ok = a.type(L, -1) == l5::kTTable;
        }
        if (ok) collect(a.gettop(L));
    }
    a.settop(L, top);
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (found.size() > 200) found.resize(200);
    out->insert(out->end(), found.begin(), found.end());
}
}  // namespace melange::simbridge
