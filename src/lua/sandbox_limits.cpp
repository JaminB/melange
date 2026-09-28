// Sandbox limits: the instruction budget (count hook) and the per-mod allocator.
#include <cstdlib>
#include <cstring>
#include <vector>

#include "lua/sandbox_core.h"

namespace melange::sandbox {
namespace {
constexpr int kMaxSlots = 512;
constexpr int kHookCount = 1000;
// Header in front of every block: the owner slot. 8 bytes keeps Lua's 8-byte alignment.
constexpr size_t kHeader = 8;
// Host code (slot 0: the VM's own tables, the console, C++ modules) may exceed the total cap by this much, so that
// Sandbox can still build error messages and tear mods down when mods have filled the VM.
constexpr size_t kHostSlack = 16u << 20;

size_t g_slot[kMaxSlots];
size_t g_total = 0;
uint64_t g_instructions = 0;
std::vector<Ctx> g_ctx;
bool g_fastHook = false;
// A refusal marks the running call out of memory. Lua's core retries once after an emergency full GC; if that retry
// succeeds, the mark set by the refusal is taken back. (Buffers in lauxlib do not retry.)
const void* g_refusedPtr = nullptr;
size_t g_refusedSize = 0;
bool g_refusedPending = false, g_refusedMarked = false;

bool Allow(int slot, size_t add) {
    const Limits& lim = GetLimits();
    if (g_total + add > lim.totalBytes + (slot == 0 ? kHostSlack : 0)) return false;
    return slot == 0 || g_slot[slot] + add <= lim.modBytes;
}

void Refused(const void* ptr, size_t nsize) {
    Ctx* c = TopCtx();
    g_refusedPending = true;
    g_refusedPtr = ptr;
    g_refusedSize = nsize;
    g_refusedMarked = c && !c->oom;
    if (c) c->oom = true;
}

void Granted(const void* ptr, size_t nsize) {
    if (!g_refusedPending) return;
    if (g_refusedMarked && g_refusedPtr == ptr && g_refusedSize == nsize)
        if (Ctx* c = TopCtx()) c->oom = false;
    g_refusedPending = false;
}

void Hook(lua_State* L, lua_Debug* ar) {
    if (ar->event != LUA_HOOKCOUNT || g_ctx.empty()) return;
    Ctx& c = g_ctx.back();
    const int n = lua_gethookcount(L);
    g_instructions += static_cast<uint64_t>(n);
    if (c.mod) c.mod->instructions += static_cast<uint64_t>(n);
    c.budget -= n;
    if (c.budget > 0 && !c.exhausted) return;
    c.exhausted = true;
    // A mod's own pcall can catch one raise: from now on raise on every instruction, on this thread and the main one.
    if (n != 1) lua_sethook(L, &Hook, LUA_MASKCOUNT, 1);
    lua_State* main = sandbox::L();
    if (main && main != L) lua_sethook(main, &Hook, LUA_MASKCOUNT, 1);
    g_fastHook = true;
    luaL_error(L, "instruction budget exceeded (%d instructions per call)", static_cast<int>(GetLimits().instrPerCall));
}
}  // namespace

void* Alloc(void*, void* ptr, size_t osize, size_t nsize) {
    if (nsize == 0) {
        if (ptr) {
            char* base = static_cast<char*>(ptr) - kHeader;
            uint16_t slot;
            memcpy(&slot, base, sizeof(slot));
            g_slot[slot] -= osize;
            g_total -= osize;
            free(base);
        }
        return nullptr;
    }
    const Ctx* c = TopCtx();
    const int cur = c && c->mod ? c->mod->slot : 0;
    if (!ptr) {
        if (!Allow(cur, nsize)) {
            Refused(nullptr, nsize);
            return nullptr;
        }
        char* base = static_cast<char*>(malloc(nsize + kHeader));
        if (!base) return nullptr;
        const uint16_t slot = static_cast<uint16_t>(cur);
        memcpy(base, &slot, sizeof(slot));
        g_slot[cur] += nsize;
        g_total += nsize;
        Granted(nullptr, nsize);
        return base + kHeader;
    }
    char* base = static_cast<char*>(ptr) - kHeader;
    uint16_t slot;
    memcpy(&slot, base, sizeof(slot));
    if (nsize > osize && !Allow(slot, nsize - osize)) {
        Refused(ptr, nsize);
        return nullptr;
    }
    char* nb = static_cast<char*>(realloc(base, nsize + kHeader));
    if (!nb) return nsize <= osize ? ptr : nullptr;  // Lua assumes shrinking never fails
    g_slot[slot] = g_slot[slot] - osize + nsize;
    g_total = g_total - osize + nsize;
    Granted(ptr, nsize);
    return nb + kHeader;
}

void InstallHook(lua_State* L) { lua_sethook(L, &Hook, LUA_MASKCOUNT, kHookCount); }

void PushCtx(ModRec* mod, Gen* gen, Callback* cb) {
    Ctx c;
    c.mod = mod;
    c.gen = gen;
    c.cb = cb;
    c.budget = GetLimits().instrPerCall;
    g_ctx.push_back(c);
}

Ctx PopCtx() {
    Ctx c = g_ctx.back();
    g_ctx.pop_back();
    if (g_fastHook) {
        bool still = false;
        for (const Ctx& o : g_ctx) still |= o.exhausted;
        if (!still) {
            g_fastHook = false;
            if (lua_State* main = sandbox::L()) lua_sethook(main, &Hook, LUA_MASKCOUNT, kHookCount);
        }
    }
    return c;
}

Ctx* TopCtx() { return g_ctx.empty() ? nullptr : &g_ctx.back(); }

size_t SlotBytes(int slot) { return slot >= 0 && slot < kMaxSlots ? g_slot[slot] : 0; }
size_t TotalBytes() { return g_total; }
uint64_t TotalInstructions() { return g_instructions; }

void ResetLimits() {
    g_ctx.clear();
    g_fastHook = false;
    g_refusedPending = false;
}

int MaxSlots() { return kMaxSlots; }
}  // namespace melange::sandbox
