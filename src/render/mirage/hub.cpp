#include "render/mirage/hub.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/events.h"
#include "core/log.h"
#include "core/mem.h"
#include "render/mirage/engine.h"
#include "render/mirage/pe.h"

namespace melange::mirage::hub {
namespace {
constexpr int kMaxFn = 1024, kStride = 64;
// Thunk layout (kStride bytes each):
//   +0  EB xx         jmp short: xx=0 falls into the counter, xx=0x0F skips to the passthrough jump
//   +2  FF 05 cnt     inc dword [cnt]
//   +8  80 3D log 00  cmp byte [logOn], 0
//   +15 75 06         jne +6
//   +17 FF 25 real    jmp [real]
//   +23 68 idx        push idx
//   +28 E9 rel32      jmp LogStub
// The mode switch rewrites only the first aligned dword.
constexpr uint8_t kSkipToJump = 0x0F;

struct Fn {
    char name[56];
    Src src;
    void* volatile real;  // what the thunk jumps to: the true function or the newest interposer
    void* truefn;
};
Fn g_fn[kMaxFn];
volatile uint32_t g_cnt[kMaxFn];
volatile int g_nfn = 0;
uint8_t* g_thunks = nullptr;
volatile uint8_t g_logOn = 0;
Mode g_mode = Mode::Passthrough;
bool g_installed = false;
std::recursive_mutex g_mx;

struct Link {
    void* hook;
    void** next;
};
std::map<std::string, std::vector<Link>> g_chains;

Rec* g_ring = nullptr;
uint32_t g_ringMask = 0;
volatile long g_ringPos = 0;

void* g_gpaExe = nullptr;
void* g_gpaCg = nullptr;

void* __cdecl LogCall(uint32_t idx, const uint32_t* f) {
    uint32_t pos = static_cast<uint32_t>(_InterlockedIncrement(&g_ringPos)) - 1;
    Rec& r = g_ring[pos & g_ringMask];
    r.frame = static_cast<uint32_t>(events::FrameCount());
    r.fn = static_cast<uint16_t>(idx);
    r.pass = static_cast<uint8_t>(engine::Pass());
    r.flags = 0;
    r.caller = f[0];
    memcpy(r.a, f + 1, sizeof(r.a));
    r.tsc = __rdtsc();
    return g_fn[idx].real;
}

// Entered from a thunk with the function index pushed on top of the caller's return address.
__declspec(naked) void LogStub() {
    __asm {
        pushad
        pushfd
        lea eax, [esp + 40]
        push eax
        push dword ptr [esp + 40]
        call LogCall
        add esp, 8
        mov [esp + 36], eax
        popfd
        popad
        ret
    }
}

uint8_t* Thunk(int i) { return g_thunks + i * kStride; }

void WriteHead(int i, bool count) {
    uint8_t* t = Thunk(i);
    uint32_t v;
    memcpy(&v, t, 4);
    v = (v & 0xFFFF00FFu) | (static_cast<uint32_t>(count ? 0 : kSkipToJump) << 8);
    _InterlockedExchange(reinterpret_cast<volatile long*>(t), static_cast<long>(v));
}

void BuildThunk(int i) {
    uint8_t* t = Thunk(i);
    uint8_t b[33];
    auto put32 = [&](int at, uint32_t v) { memcpy(b + at, &v, 4); };
    b[0] = 0xEB;
    b[1] = g_mode == Mode::Passthrough ? kSkipToJump : 0;
    b[2] = 0xFF, b[3] = 0x05;
    put32(4, reinterpret_cast<uint32_t>(&g_cnt[i]));
    b[8] = 0x80, b[9] = 0x3D;
    put32(10, reinterpret_cast<uint32_t>(&g_logOn));
    b[14] = 0;
    b[15] = 0x75, b[16] = 0x06;
    b[17] = 0xFF, b[18] = 0x25;
    put32(19, reinterpret_cast<uint32_t>(&g_fn[i].real));
    b[23] = 0x68;
    put32(24, static_cast<uint32_t>(i));
    b[28] = 0xE9;
    put32(29, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&LogStub) - reinterpret_cast<uintptr_t>(t + 33)));
    memcpy(t, b, sizeof(b));
    FlushInstructionCache(GetCurrentProcess(), t, kStride);
}

bool ValidProc(const void* p) {
    auto v = reinterpret_cast<intptr_t>(p);
    return p && v != 1 && v != 2 && v != 3 && v != -1;
}

int Find(const char* name, Src src) {
    for (int i = 0; i < g_nfn; ++i)
        if (g_fn[i].src == src && strcmp(g_fn[i].name, name) == 0) return i;
    return -1;
}

void* FirstTrue(const char* name) {
    for (int i = 0; i < g_nfn; ++i)
        if (strcmp(g_fn[i].name, name) == 0) return g_fn[i].truefn;
    return nullptr;
}

// `own`: a hub-internal wrapper that always sits in front (wglGetProcAddress).
void* ThunkFor(const char* name, Src src, void* truefn, void* own = nullptr) {
    std::lock_guard lk(g_mx);
    int i = Find(name, src);
    if (i >= 0 && g_fn[i].truefn == truefn) return Thunk(i);
    bool fresh = i < 0;
    if (fresh) {
        if (g_nfn >= kMaxFn) return truefn;
        i = g_nfn;
        strncpy_s(g_fn[i].name, name, _TRUNCATE);
        g_fn[i].src = src;
    }
    g_fn[i].truefn = truefn;
    void* real = own ? own : truefn;
    if (!own) {
        auto it = g_chains.find(name);
        if (it != g_chains.end() && !it->second.empty()) {
            if (!*it->second.front().next) *it->second.front().next = truefn;
            real = it->second.back().hook;
        }
    }
    g_fn[i].real = real;
    if (fresh) {
        BuildThunk(i);
        g_nfn = i + 1;
    }
    return Thunk(i);
}

PROC WINAPI GpaExe(LPCSTR name) {
    PROC p = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(g_gpaExe)(name);
    if (!name || !ValidProc(reinterpret_cast<void*>(p))) return p;
    return reinterpret_cast<PROC>(ThunkFor(name, Src::ExeProc, reinterpret_cast<void*>(p)));
}
PROC WINAPI GpaCg(LPCSTR name) {
    PROC p = reinterpret_cast<PROC(WINAPI*)(LPCSTR)>(g_gpaCg)(name);
    if (!name || !ValidProc(reinterpret_cast<void*>(p))) return p;
    return reinterpret_cast<PROC>(ThunkFor(name, Src::CgGLProc, reinterpret_cast<void*>(p)));
}

int ThunkImports(HMODULE mod, Src src, void*& gpaTrue, void* gpaHook) {
    return pe::ForEachImport(mod, "OPENGL32.dll", [&](const char* name, void** slot) {
        if (IsThunk(*slot)) return;
        void* own = nullptr;
        if (strcmp(name, "wglGetProcAddress") == 0) {
            gpaTrue = *slot;
            own = gpaHook;
        }
        void* t = ThunkFor(name, src, *slot, own);
        mem::Write(reinterpret_cast<uintptr_t>(slot), &t, 4);
    });
}
}  // namespace

bool Require(const char* who) {
    std::lock_guard lk(g_mx);
    if (g_installed) {
        LOG_INFO("[mirage] GL hub already installed; also required by %s", who);
        return true;
    }
    g_thunks = static_cast<uint8_t*>(VirtualAlloc(nullptr, kMaxFn * kStride, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_thunks) {
        LOG_ERROR("[mirage] GL hub: thunk allocation failed (required by %s)", who);
        return false;
    }
    int nExe = ThunkImports(GetModuleHandleW(nullptr), Src::ExeIat, g_gpaExe, reinterpret_cast<void*>(&GpaExe));
    int nCg = ThunkImports(GetModuleHandleW(L"cgGL.dll"), Src::CgGLIat, g_gpaCg, reinterpret_cast<void*>(&GpaCg));
    g_installed = true;
    LOG_INFO("[mirage] GL hub installed for %s: exe OPENGL32 imports=%d, cgGL OPENGL32 imports=%d", who, nExe, nCg);
    return true;
}

bool Installed() { return g_installed; }

bool Interpose(const char* glName, void* hook, void** next) {
    if (!glName || !hook || !next || !g_installed || strcmp(glName, "wglGetProcAddress") == 0) return false;
    std::lock_guard lk(g_mx);
    auto& chain = g_chains[glName];
    *next = chain.empty() ? FirstTrue(glName) : chain.back().hook;
    chain.push_back({hook, next});
    for (int i = 0; i < g_nfn; ++i)
        if (strcmp(g_fn[i].name, glName) == 0) g_fn[i].real = hook;
    return true;
}

void SetMode(Mode m) {
    std::lock_guard lk(g_mx);
    if (!g_installed) return;
    if (m == Mode::Log && !g_ring) {
        int log2 = std::clamp(config::GetInt("MirageTrace", "RingLog2", 17), 10, 22);
        g_ring = static_cast<Rec*>(VirtualAlloc(nullptr, sizeof(Rec) << log2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_ring) {
            LOG_ERROR("[mirage] GL hub: ring allocation failed; log mode unavailable");
            return;
        }
        g_ringMask = (1u << log2) - 1;
    }
    if (m != Mode::Log) g_logOn = 0;
    g_mode = m;
    for (int i = 0; i < g_nfn; ++i) WriteHead(i, m != Mode::Passthrough);
    FlushInstructionCache(GetCurrentProcess(), g_thunks, kMaxFn * kStride);
    if (m == Mode::Log) g_logOn = 1;
}

Mode GetMode() { return g_mode; }
int Count() { return g_nfn; }
const char* Name(int i) { return i >= 0 && i < g_nfn ? g_fn[i].name : ""; }
Src Source(int i) { return i >= 0 && i < g_nfn ? g_fn[i].src : Src::ExeIat; }
const volatile uint32_t* Counters() { return g_cnt; }

uint32_t ReadRing(uint32_t from, void (*sink)(const Rec&, void*), void* user) {
    uint32_t to = static_cast<uint32_t>(g_ringPos);
    if (!g_ring) return to;
    if (to - from > g_ringMask + 1) from = to - (g_ringMask + 1);
    for (uint32_t k = from; k != to; ++k) sink(g_ring[k & g_ringMask], user);
    return to;
}

uint32_t RingPos() { return static_cast<uint32_t>(g_ringPos); }

bool IsThunk(const void* p) {
    auto v = reinterpret_cast<const uint8_t*>(p);
    return g_thunks && v >= g_thunks && v < g_thunks + kMaxFn * kStride;
}
}  // namespace melange::mirage::hub
