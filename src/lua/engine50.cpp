#include "lua/engine50.h"

#include <windows.h>
#include <safetyhook.hpp>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>

#include "core/game.h"
#include "core/log.h"
#include "core/mem.h"

namespace melange::lua50 {
namespace {
template <class F>
F At(uintptr_t a) {
    return reinterpret_cast<F>(a);
}

const Api kApi = {
    At<int (*)(State*)>(0x41f4b0), At<void (*)(State*, int)>(0x41f4c0), At<int (*)(State*, int)>(0x41f5f0),
    At<int (*)(State*, int)>(0x41f660), At<int (*)(State*, int)>(0x41f6a0),
    At<float (*)(State*, int)>(0x41f7d0), At<int (*)(State*, int)>(0x41f830), At<const char* (*)(State*, int)>(0x41f860),
    At<void* (*)(State*, int)>(0x41f940),
    At<void (*)(State*)>(0x41f9f0), At<void (*)(State*, float)>(0x41fa10), At<void (*)(State*, const char*)>(0x41fa80),
    At<void (*)(State*, int (*)(State*), int)>(0x41fb60), At<void (*)(State*, int)>(0x41fbe0),
    At<void (*)(State*, void*)>(0x41fc00),
    At<void (*)(State*, int)>(0x41fc20), At<void (*)(State*, int)>(0x41fdf0), At<void (*)(State*, int)>(0x41fc90),
    At<void (*)(State*, int)>(0x41fe20),
    At<void (*)(State*, int, int)>(0x41fcc0), At<void (*)(State*, int, int)>(0x41fea0), At<void (*)(State*)>(0x41fd00),
    At<int (*)(State*, int)>(0x41fd40), At<int (*)(State*, int)>(0x41ff10),
    At<int (*)(State*, int, int, int)>(0x420000), At<void (*)(State*)>(0x420290), At<int (*)(State*, int)>(0x4202a0),
    At<int (*)(State*, void (*)(State*, void*), int, int)>(0x420550),
    At<int (*)(State*, int)>(0x42fd00), At<void (*)(State*, int, int)>(0x42fdf0),
    At<int (*)(State*, const char*, size_t, const char*)>(0x4300c0),
    At<int (*)(State*, int)>(0x41f3a0), At<size_t (*)(State*, int)>(0x41f8c0),
    At<void (*)(State*, int)>(0x41f5c0), At<void (*)(State*, int)>(0x41f550), At<void (*)(State*, int)>(0x41f510),
    At<void (*)(State*, int)>(0x41f590), At<void (*)(State*, const char*, size_t)>(0x41fa30),
    At<int (*)(State*, int)>(0x41ff70), At<void (*)(State*, int)>(0x41fda0),
    At<Hook (*)(State*)>(0x420590), At<int (*)(State*)>(0x4205a0), At<int (*)(State*)>(0x420230),
};

struct Prologue {
    uintptr_t addr;
    const char* bytes;
};
// First bytes of every entry point on #1077.
constexpr Prologue kPrologues[] = {
    {0x41f4b0, "8b4c24048b41082b"}, {0x41f4c0, "8b4c24088b442404"}, {0x41f5f0, "8b5424088b442404e8"},
    {0x41f660, "518b54240c8b442408"}, {0x41f6a0, "8b5424088b442404e8"}, {0x41f7d0, "8b5424088b44240483ec14"},
    {0x41f830, "8b5424088b442404e8"}, {0x41f860, "518b54240c56578b7c2410"}, {0x41f940, "8b5424088b442404e8"},
    {0x41f9f0, "8b4424048b4808c701"}, {0x41fa10, "8b442404d94424088b4808"}, {0x41fa80, "5153558b6c2414565785ed"},
    {0x41fb60, "53578b7c240c8b47108b4824"}, {0x41fbe0, "8b4424048b480833d2"}, {0x41fc00, "8b4424048b48088b542408c7"},
    {0x41fc20, "558bec83e4f8518b550c"}, {0x41fdf0, "8b54240853568b74240c8b46"}, {0x41fc90, "8b4424048b48088b54240853"},
    {0x41fe20, "518b54240c535556"}, {0x41fcc0, "8b44240c8b5424085657"}, {0x41fea0, "558bec83e4f88b550c83ec0c"},
    {0x41fd00, "568b7424088b46108b4824"}, {0x41fd40, "8b542408568b7424088bc6e8"}, {0x41ff10, "8b542408568b742408578bc6"},
    {0x420000, "558bec83e4f88b551483ec0c"}, {0x420290, "568b742408e8360f00005e"}, {0x4202a0, "8b542408568b742408578bc6"},
    {0x420550, "8b4c240885c974088b54240c"}, {0x42fd00, "53558b6c24108d850f270000"}, {0x42fdf0, "558b6c241085ed7c77"},
    {0x4300c0, "558bec83e4f883ec088b450c"},
    {0x41f3a0, "8b4c2408568b7424088b4608"}, {0x41f8c0, "518b54240c56578b7c2410"}, {0x41f5c0, "8b542408568b7424088bc6e8"},
    {0x41f550, "8b542408568b7424088bc6e8"}, {0x41f510, "8b542408568b7424088bc6e8"}, {0x41f590, "8b54240856578b7c240c8b77"},
    {0x41fa30, "53568b74240c8b46108b4824"}, {0x41ff70, "8b542408568b742408578bc6"}, {0x41fda0, "8b542408568b7424088bc6e8"},
    {0x420590, "8b4424048b403cc3"}, {0x4205a0, "8b4424040fb64030c3"}, {0x420230, "8b4424048b48108b4124"},
    // message registry, script service, XLuaContext dtor, logic RNG, string/table openers
    {0x690d44, "558bec83ec10ff7508e87eff"}, {0x690cd0, "558bec83ec10c745f8200000"}, {0x690bf7, "558bec83ec14894df4ff7508"},
    {0x6958d2, "558bec83ec30894de46a00"}, {0x6956b9, "558bec83ec18894de88b45e8"}, {0x697d04, "558bec83ec24894ddcff7508"},
    {0x6953e9, "558bec81ec50010000568b45"}, {0x69566a, "558bec833da8cc9300ff742d"}, {0x698e88, "558bec83ec10894df08b45f0"},
    {0x6994c0, "558bec83ec0c894df46a006a"}, {0x78a202, "53568bf1578d7e246a008bcf"}, {0x6996f3, "558bec83ec64894d9c"},
    {0x68c053, "558bec8b4508a334d09600"}, {0x437f30, "8b4424046a0068788c8b00"}, {0x438fa0, "8b4424046a0068308c8b00"},
};

constexpr uintptr_t kFlowControl = 0x95a298;  // FlowControlService*; +0xdd0 = task handle of its XScriptService
constexpr uintptr_t kTaskManager = 0x96d030;  // +0x1c: task table (entries of 0x24: +0xc object, +0x14 handle)
constexpr uintptr_t kScriptServiceVtbl = 0x885574;
constexpr uintptr_t kMrs = 0x96d090, kRegTable = 0x96d094, kRegCap = 0x96d08c;
constexpr uintptr_t kRngLogic = 0x96d034;
constexpr uintptr_t kStringReg = 0x8b8c78, kTableReg = 0x8b8c30;
constexpr size_t kSsDenySend = 0x18c, kSsDenyData = 0x198, kSsAllowAll = 0x1b0, kSsCtx = 0x30, kSsL = 0x38,
                 kSsRun = 0x3c;

template <class T>
T Rd(uintptr_t a) {
    T v{};
    mem::SafeRead(a, &v, sizeof(T));
    return v;
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool Matches(const Prologue& p) {
    uint8_t want[32], got[32];
    size_t n = 0;
    for (const char* s = p.bytes; s[0] && s[1] && n < sizeof want; s += 2) {
        if (Hex(s[0]) < 0 || Hex(s[1]) < 0) return false;
        want[n++] = static_cast<uint8_t>(Hex(s[0]) << 4 | Hex(s[1]));
    }
    if (!mem::SafeRead(p.addr, got, n)) return false;
    return std::memcmp(want, got, n) == 0;
}

std::once_flag g_checkOnce;
bool g_checkOk = false;

std::mutex g_mx;
bool g_tracking = false;
uintptr_t g_trackedSs = 0;
State* g_trackedL = nullptr;
uint32_t g_seed = 0, g_serial = 0;
struct Observer {
    int handle;
    ContextFn fn;
    void* user;
};
std::vector<Observer> g_observers;
int g_nextHandle = 1;
SafetyHookInline g_hCreate, g_hDtor;

void Notify(bool created, State* L) {
    std::vector<Observer> obs;
    {
        std::lock_guard lk(g_mx);
        obs = g_observers;
    }
    for (auto& o : obs) o.fn(created, L, o.user);
}

void __fastcall HkCreateContext(uintptr_t ss, void*) {
    g_hCreate.thiscall<void>(ss);
    State* L = reinterpret_cast<State*>(Rd<uintptr_t>(ss + kSsL));
    {
        std::lock_guard lk(g_mx);
        g_trackedSs = ss;
        g_trackedL = L;
        g_seed = Rd<uint32_t>(kRngLogic);
        ++g_serial;
    }
    if (L) Notify(true, L);
}

void __fastcall HkCtxDtor(uintptr_t ctx, void*) {
    State* L = reinterpret_cast<State*>(Rd<uintptr_t>(ctx + 0x14));
    bool ours;
    {
        std::lock_guard lk(g_mx);
        ours = L && L == g_trackedL;
    }
    if (ours) {
        Notify(false, L);
        std::lock_guard lk(g_mx);
        g_trackedSs = 0;
        g_trackedL = nullptr;
    }
    g_hDtor.thiscall<void>(ctx);
}

// The engine keeps deny lists as std::vector<char*> of "Name" or "Name,param"; it tests every second entry.
bool ListDenies(uintptr_t vec, const char* name, const char* param) {
    uintptr_t b = Rd<uintptr_t>(vec), e = Rd<uintptr_t>(vec + 4);
    if (!b || e < b || e - b > 4096) return false;
    const uint32_t n = static_cast<uint32_t>((e - b) / 4);
    for (uint32_t i = 0; i < n; i += 2) {
        char entry[72] = {};
        if (!mem::SafeRead(Rd<uintptr_t>(b + i * 4), entry, sizeof entry - 1)) continue;
        char* p = entry;
        auto token = [&p]() -> const char* {
            while (*p == ',') ++p;
            if (!*p) return nullptr;
            const char* t = p;
            while (*p && *p != ',') ++p;
            if (*p) *p++ = 0;
            return t;
        };
        const char* first = token();
        const char* second = token();
        if (!first || !name || std::strcmp(first, name) != 0) continue;
        if (!second) return true;
        if (param && std::strcmp(second, param) == 0) return true;
    }
    return false;
}

uint32_t ElfSlot(const char* s, uint32_t cap) {
    uint32_t h = 0;
    for (; *s; ++s) {
        h = (h << 4) + static_cast<uint32_t>(static_cast<int>(static_cast<signed char>(*s)));
        const uint32_t g = h & 0xf0000000;
        if (g) h = (g >> 24) ^ h ^ g;
    }
    return h % cap;
}

constexpr uintptr_t kLuaOpen = 0x4217a0, kLuaClose = 0x421840;
constexpr Prologue kCompilePrologues[] = {{kLuaOpen, "53566a5c33db5353e8"}, {kLuaClose, "558bec83e4f88b45088b4810"}};
constexpr int kErrSyntax = 3;

// A private state per call: nothing is shared with the match VM, and the allocator is the CRT's.
int RawCompile(const char* text, size_t len, char* msg, size_t cap) {
    __try {
        State* L = At<State* (*)()>(kLuaOpen)();
        if (!L) return -1;
        const int rc = kApi.loadbuffer(L, text, len, "=script.lua");
        if (rc) {
            const char* m = kApi.tostring(L, -1);
            size_t n = 0;
            for (; m && m[n] && n + 1 < cap; ++n) msg[n] = m[n];
            msg[n] = 0;
        }
        At<void (*)(State*)>(kLuaClose)(L);
        return rc == 0 ? 0 : rc == kErrSyntax ? 1 : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
}  // namespace

const Api& A() { return kApi; }

int EntryPoints() { return static_cast<int>(std::size(kPrologues)); }

bool Check() {
    std::call_once(g_checkOnce, [] {
        if (!game::IsKnownBuild()) {
            LOG_WARN("[lua50] unknown build: the engine Lua 5.0.1 access is off");
            return;
        }
        int bad = 0;
        for (auto& p : kPrologues) {
            if (Matches(p)) continue;
            ++bad;
            LOG_WARN("[lua50] prologue mismatch at %08x", static_cast<unsigned>(p.addr));
        }
        g_checkOk = bad == 0;
        if (g_checkOk)
            LOG_INFO("[lua50] lua50: ok (%d entry points)", EntryPoints());
        else
            LOG_ERROR("[lua50] lua50: FAILED (%d of %d entry points differ); M2 match-VM features stay off", bad,
                      EntryPoints());
    });
    return g_checkOk;
}

uintptr_t ScriptService() {
    if (!Check()) return 0;
    const uintptr_t flow = Rd<uintptr_t>(kFlowControl);
    const uintptr_t mgr = Rd<uintptr_t>(kTaskManager);
    if (!flow || !mgr) return 0;
    const uint32_t handle = Rd<uint32_t>(flow + 0xdd0);
    if (handle == 0xffffffff || !handle) return 0;
    const uintptr_t table = Rd<uintptr_t>(Rd<uintptr_t>(mgr + 0x1c));
    if (!table) return 0;
    const uintptr_t entry = table + (handle & 0xfff) * 0x24;
    if (Rd<uint32_t>(entry + 0x14) != handle) return 0;
    const uintptr_t ss = Rd<uintptr_t>(entry + 0xc);
    if (!ss || Rd<uintptr_t>(ss) != kScriptServiceVtbl) return 0;
    return ss;
}

State* MatchState() {
    const uintptr_t ss = ScriptService();
    return ss ? reinterpret_cast<State*>(Rd<uintptr_t>(ss + kSsL)) : nullptr;
}

int RunState() {
    const uintptr_t ss = ScriptService();
    return ss ? Rd<int>(ss + kSsRun) : 0;
}

bool AllowAll() {
    const uintptr_t ss = ScriptService();
    return ss && Rd<uint8_t>(ss + kSsAllowAll) != 0;
}

bool EngineWouldDenySend(const char* name, const char* param) {
    const uintptr_t ss = ScriptService();
    return ss && ListDenies(ss + kSsDenySend, name, param);
}

bool EngineWouldDenyData(const char* name) {
    const uintptr_t ss = ScriptService();
    return ss && ListDenies(ss + kSsDenyData, name, nullptr);
}

uint16_t Lookup(const char* name) {
    if (!name || !*name || !Check()) return 0xffff;
    const uint32_t cap = Rd<uint32_t>(kRegCap);
    const uintptr_t tab = Rd<uintptr_t>(kRegTable);
    if (!cap || cap > 0x7fff || !tab) return 0xffff;
    const uint32_t start = ElfSlot(name, cap);
    uint32_t i = start;
    do {
        const uintptr_t s = Rd<uintptr_t>(tab + i * 4);
        if (!s) break;
        char buf[256] = {};
        if (mem::SafeRead(s, buf, sizeof buf - 1) && std::strcmp(buf, name) == 0)
            return static_cast<uint16_t>(i | 0x8000);
        i = (i + 1) % cap;
    } while (i != start);
    return 0xffff;
}

bool Register(const char* name) {
    if (!name || !*name || !Check()) return false;
    const uintptr_t mrs = Rd<uintptr_t>(kMrs);
    if (!mrs) return false;
    return At<int(__thiscall*)(uintptr_t, const char*)>(0x690bf7)(mrs, name) == 0;
}

uint32_t RegistryCount() {
    const uint32_t cap = Rd<uint32_t>(kRegCap);
    const uintptr_t tab = Rd<uintptr_t>(kRegTable);
    if (!cap || cap > 0x7fff || !tab) return 0;
    std::vector<uint32_t> slots(cap);
    if (!mem::SafeRead(tab, slots.data(), cap * 4)) return 0;
    uint32_t n = 0;
    for (uint32_t s : slots) n += s != 0;
    return n;
}

uint32_t RegistryCapacity() { return Rd<uint32_t>(kRegCap); }

uint32_t LogicSeed() {
    std::lock_guard lk(g_mx);
    return g_trackedL ? g_seed : 0;
}

uint32_t LogicRng() { return Rd<uint32_t>(kRngLogic); }

bool Track() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!Check()) return;
        g_hCreate = safetyhook::create_inline(kCreateContext, &HkCreateContext);
        g_hDtor = safetyhook::create_inline(kLuaCtxDtor, &HkCtxDtor);
        g_tracking = g_hCreate && g_hDtor;
        if (!g_tracking) {
            LOG_ERROR("[lua50] match VM tracking hooks failed");
            g_hCreate = {};
            g_hDtor = {};
            return;
        }
        const uintptr_t ss = ScriptService();
        std::lock_guard lk(g_mx);
        if (ss) {
            g_trackedSs = ss;
            g_trackedL = reinterpret_cast<State*>(Rd<uintptr_t>(ss + kSsL));
        }
    });
    return g_tracking;
}

int OnContext(ContextFn fn, void* user) {
    if (!fn) return 0;
    std::lock_guard lk(g_mx);
    g_observers.push_back({g_nextHandle, fn, user});
    return g_nextHandle++;
}

void RemoveOnContext(int handle) {
    std::lock_guard lk(g_mx);
    std::erase_if(g_observers, [handle](const Observer& o) { return o.handle == handle; });
}

uint32_t ContextSerial() {
    std::lock_guard lk(g_mx);
    return g_serial;
}

const LibReg* StringLib() { return Check() ? reinterpret_cast<const LibReg*>(kStringReg) : nullptr; }
const LibReg* TableLib() { return Check() ? reinterpret_cast<const LibReg*>(kTableReg) : nullptr; }

Compiled Compile(std::string_view text, int* line, std::string* message) {
    static const bool ok = std::all_of(std::begin(kCompilePrologues), std::end(kCompilePrologues), Matches);
    if (!Check() || !ok) return Compiled::Unavailable;
    char msg[512] = "";
    const int rc = RawCompile(text.data(), text.size(), msg, sizeof msg);
    if (rc < 0) return Compiled::Unavailable;
    if (rc == 0) return Compiled::Ok;
    // "script.lua:<line>: <message>"
    std::string_view m(msg);
    *line = 1;
    *message = msg;
    if (m.starts_with("script.lua:")) {
        m.remove_prefix(11);
        int n = 0;
        size_t i = 0;
        for (; i < m.size() && m[i] >= '0' && m[i] <= '9' && n < 1000000; ++i) n = n * 10 + (m[i] - '0');
        if (i && m.substr(i).starts_with(": ")) {
            *line = n;
            *message = std::string(m.substr(i + 2));
        }
    }
    return Compiled::SyntaxError;
}
}  // namespace melange::lua50
